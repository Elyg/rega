// Getting kitty images to the terminal: the escapes, shared memory, and
// ImageSlot preparing pictures on its own thread.

#include "term/ImageView.h"
#include "term/Kitty.h"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <libdeflate.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rega;
using namespace std::chrono_literals;

namespace
{

Rgba8Image noise(int w, int h)
{
	Rgba8Image img;
	img.m_width = w;
	img.m_height = h;
	img.m_pixels.resize(static_cast<size_t>(w) * h * 4);
	uint32_t x = 2463534242u;
	for(auto& p : img.m_pixels)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p = static_cast<uint8_t>(x);
	}
	return img;
}

// What a Transmitter wrote, from any thread.
struct Capture
{
	std::mutex m_mu;
	std::vector<std::string> m_writes;
	kitty::Transmitter::WriteFn fn()
	{
		return [this](std::string_view b, bool)
		{
			std::lock_guard lk(m_mu);
			m_writes.emplace_back(b);
		};
	}
	std::vector<std::string> take()
	{
		std::lock_guard lk(m_mu);
		return std::exchange(m_writes, {});
	}
};

std::string shmName(int n)
{
	return "/rega-" + std::to_string(getpid()) + "-" + std::to_string(n);
}

bool shmExists(const std::string& name)
{
	const int fd = shm_open(name.c_str(), O_RDONLY, 0);
	if(fd < 0)
	{
		return false;
	}
	close(fd);
	return true;
}

LayerImagePtr grey(float v)
{
	LayerImage img;
	img.m_width = img.m_height = 4;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 3};
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, Plane(std::vector<float>(16, v)));
	return std::make_shared<const LayerImage>(std::move(img));
}

template <class Pred>
bool waitFor(Pred&& pred)
{
	const auto end = std::chrono::steady_clock::now() + 3s;
	while(!pred())
	{
		if(std::chrono::steady_clock::now() > end)
		{
			return false;
		}
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

} // namespace

TEST(Kitty, TmuxWrapCopiesTheRunsBetweenEscapes)
{
	// Tmux in tmux: the inner one unwraps, the outer one gets it wrapped.
	EXPECT_EQ(kitty::tmuxWrap(std::string("plain"), 0), "plain");
	EXPECT_EQ(kitty::tmuxWrap(std::string("plain"), 2),
	          "\x1bPtmux;\x1b\x1bPtmux;plain\x1b\x1b\\\x1b\\");
	EXPECT_EQ(kitty::tmuxWrap("plain"), "\x1bPtmux;plain\x1b\\");
	EXPECT_EQ(kitty::tmuxWrap("\x1b\x1b"
	                          "ab\x1b"),
	          "\x1bPtmux;\x1b\x1b\x1b\x1b"
	          "ab\x1b\x1b\x1b\\");
}

TEST(Kitty, CompactPlaceholdersKeepsDiacriticsOnlyAfterABreak)
{
	ftxui::Screen screen(7, 2);
	const auto id = ftxui::Color::Palette256(7);
	for(int x = 0; x < 7; ++x)
	{
		for(int y = 0; y < 2; ++y)
		{
			screen.PixelAt(x, y).character = kitty::placeholderCell(y, x);
			screen.PixelAt(x, y).foreground_color = id;
		}
	}
	screen.PixelAt(3, 0).character = "a"; // something drawn over the image
	screen.PixelAt(4, 1).foreground_color = ftxui::Color::Palette256(8);
	compactPlaceholders(screen);
	const std::string bare = kitty::barePlaceholderCell();
	const std::vector<std::string> row0 = {kitty::placeholderCell(0, 0),
	                                       bare,
	                                       bare,
	                                       "a",
	                                       kitty::placeholderCell(0, 4),
	                                       bare,
	                                       bare};
	const std::vector<std::string> row1 = {kitty::placeholderCell(1, 0),
	                                       bare,
	                                       bare,
	                                       bare,
	                                       kitty::placeholderCell(1, 4),
	                                       kitty::placeholderCell(1, 5),
	                                       bare};
	for(int x = 0; x < 7; ++x)
	{
		EXPECT_EQ(screen.PixelAt(x, 0).character, row0[x]) << x;
		EXPECT_EQ(screen.PixelAt(x, 1).character, row1[x]) << x;
	}
}

TEST(Kitty, CompactPlaceholdersKeepsDiacriticsOutOfTheLastColumn)
{
	// Overlay text ending one cell short of the right edge: tmux (autowrap
	// off) would hang the last cell's diacritics on the text.
	ftxui::Screen screen(4, 1);
	const auto strip = ftxui::Color::RGB(40, 40, 40);
	for(int x = 0; x < 4; ++x)
	{
		screen.PixelAt(x, 0).character = kitty::placeholderCell(0, x);
		screen.PixelAt(x, 0).foreground_color = ftxui::Color::Palette256(7);
	}
	screen.PixelAt(2, 0).character = "e";
	screen.PixelAt(2, 0).background_color = strip;
	compactPlaceholders(screen);
	EXPECT_EQ(screen.PixelAt(2, 0).character, "e");
	EXPECT_EQ(screen.PixelAt(3, 0).character, " ");
	EXPECT_EQ(screen.PixelAt(3, 0).background_color, strip);
}

namespace
{

// A PNG's pixels, checked and decoded the long way: chunk CRCs, inflate,
// then each row's filter undone. Channels as stored (1-4).
struct DecodedPng
{
	uint32_t m_width = 0, m_height = 0;
	int m_colourType = -1, m_channels = 0;
	std::vector<uint8_t> m_pixels;
};

uint32_t be32(const std::string& s, size_t at)
{
	return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24 |
	       static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16 |
	       static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8 |
	       static_cast<uint8_t>(s[at + 3]);
}

DecodedPng decodePng(const std::string& png)
{
	DecodedPng d;
	EXPECT_EQ(png.substr(0, 8), "\x89PNG\r\n\x1a\n");
	std::string idat;
	for(size_t at = 8; at + 12 <= png.size();)
	{
		const uint32_t len = be32(png, at);
		const std::string type = png.substr(at + 4, 4);
		EXPECT_EQ(be32(png, at + 8 + len),
		          libdeflate_crc32(0, png.data() + at + 4, 4 + len))
		    << type;
		const std::string data = png.substr(at + 8, len);
		if(type == "IHDR")
		{
			d.m_width = be32(data, 0);
			d.m_height = be32(data, 4);
			d.m_colourType = data[9];
			d.m_channels = std::map<int, int>{{0, 1},
			                                  {2, 3},
			                                  {4, 2},
			                                  {6, 4}}[d.m_colourType];
		}
		else if(type == "IDAT")
		{
			idat += data;
		}
		at += 12 + len;
	}
	const size_t stride = d.m_width * d.m_channels;
	std::vector<uint8_t> rows((stride + 1) * d.m_height);
	libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
	EXPECT_EQ(libdeflate_zlib_decompress(dec,
	                                     idat.data(),
	                                     idat.size(),
	                                     rows.data(),
	                                     rows.size(),
	                                     nullptr),
	          LIBDEFLATE_SUCCESS);
	libdeflate_free_decompressor(dec);
	d.m_pixels.resize(stride * d.m_height);
	for(size_t y = 0; y < d.m_height; ++y)
	{
		const uint8_t* in = &rows[y * (stride + 1)];
		EXPECT_EQ(in[0], 2) << "Up filter"; // what encodePng uses by default
		for(size_t i = 0; i < stride; ++i)
		{
			const uint8_t up = y > 0 ? d.m_pixels[(y - 1) * stride + i] : 0;
			d.m_pixels[y * stride + i] = static_cast<uint8_t>(in[1 + i] + up);
		}
	}
	return d;
}

} // namespace

TEST(Kitty, PngKeepsOnlyTheChannelsNeeded)
{
	Rgba8Image img = noise(37, 11); // odd sizes: no row lines up
	auto expect = [&](int colourType, std::initializer_list<int> keep)
	{
		const DecodedPng d = decodePng(kitty::encodePng(img));
		EXPECT_EQ(d.m_width, 37u);
		EXPECT_EQ(d.m_height, 11u);
		ASSERT_EQ(d.m_colourType, colourType);
		std::vector<uint8_t> want;
		for(size_t i = 0; i < img.m_pixels.size(); i += 4)
		{
			for(int c : keep)
			{
				want.push_back(img.m_pixels[i + c]);
			}
		}
		EXPECT_EQ(d.m_pixels, want);
	};
	expect(6, {0, 1, 2, 3}); // colour, some alpha
	for(size_t i = 3; i < img.m_pixels.size(); i += 4)
	{
		img.m_pixels[i] = 255;
	}
	expect(2, {0, 1, 2}); // colour, opaque
	for(size_t i = 0; i < img.m_pixels.size(); i += 4)
	{
		img.m_pixels[i + 1] = img.m_pixels[i + 2] = img.m_pixels[i];
	}
	expect(0, {0}); // grey (a channel, the alpha view), opaque
	img.m_pixels[3] = 7;
	expect(4, {0, 3}); // grey, some alpha
}

TEST(Kitty, TransmitSendsAPng)
{
	kitty::TransmitOptions opt;
	opt.m_id = 9;
	const Rgba8Image img = noise(512, 256);
	const std::string s = kitty::transmit(img, opt);
	// A PNG says its own size: no s= / v=, and no o=z (it is deflated).
	EXPECT_EQ(s.rfind("\x1b_Ga=T,f=100,q=2,i=9,U=1,p=1,m=1;", 0), 0u);
	std::string b64; // the payload, chunks joined
	for(size_t at = 0; (at = s.find(';', at)) != std::string::npos; ++at)
	{
		b64.append(s, at + 1, s.find('\x1b', at) - at - 1);
	}
	const std::string png = kitty::encodePng(img);
	EXPECT_EQ(b64,
	          kitty::base64(reinterpret_cast<const uint8_t*>(png.data()),
	                        png.size()));
}

TEST(Kitty, TransmitSharedSendsOnlyTheName)
{
	kitty::TransmitOptions opt;
	opt.m_id = 7;
	opt.m_cols = 1;
	opt.m_rows = 1;
	const std::string s = kitty::transmitShared(noise(2, 2), "/x-1", opt);
	// base64("/x-1") = "L3gtMQ=="
	EXPECT_EQ(s,
	          "\x1b_Ga=T,f=32,t=s,S=16,q=2,s=2,v=2,i=7,U=1,p=1,c=1,r=1;L3gtMQ=="
	          "\x1b\\");
}

TEST(Transmitter, SharedMemoryHoldsTheRawPixels)
{
	Capture cap;
	const Rgba8Image img = noise(16, 8);
	std::string name;
	{
		kitty::Transmitter tx(false, Transfer::SHARED_MEMORY, cap.fn());
		const std::string esc = tx.encode(img, {});
		EXPECT_NE(esc.find("t=s"), std::string::npos);
		name = shmName(0);
		const int fd = shm_open(name.c_str(), O_RDONLY, 0);
		ASSERT_GE(fd, 0);
		const size_t n = img.m_pixels.size();
		void* p = mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0);
		ASSERT_NE(p, MAP_FAILED);
		EXPECT_EQ(std::memcmp(p, img.m_pixels.data(), n), 0);
		munmap(p, n);
		close(fd);
		EXPECT_TRUE(cap.take().empty()); // encode writes nothing
	}
	// Never read by a terminal: gone with the transmitter.
	EXPECT_FALSE(shmExists(name));
}

TEST(Transmitter, UnlinksSharedMemoryNoTerminalRead)
{
	Capture cap;
	kitty::Transmitter tx(false, Transfer::SHARED_MEMORY, cap.fn());
	const Rgba8Image img = noise(4, 4);
	for(size_t i = 0; i <= kitty::Transmitter::MAX_IN_FLIGHT; ++i)
	{
		(void)tx.encode(img, {});
	}
	EXPECT_FALSE(shmExists(shmName(0))); // the oldest, one past the limit
	EXPECT_TRUE(shmExists(shmName(1)));
}

TEST(Transmitter, KeepsEachImageIdsNewestFiles)
{
	// A tile sheet sends one picture per tile at once: none may go before
	// the terminal reads it, however many tiles there are.
	Capture cap;
	kitty::Transmitter tx(false, Transfer::SHARED_MEMORY, cap.fn());
	const Rgba8Image img = noise(4, 4);
	kitty::TransmitOptions opt;
	for(uint32_t id = 1; id <= 3 * kitty::Transmitter::MAX_IN_FLIGHT; ++id)
	{
		opt.m_id = id;
		(void)tx.encode(img, opt);
	}
	EXPECT_TRUE(shmExists(shmName(0))); // the first tile's, still there
}

TEST(Transmitter, TempFilesHoldTheRawPixelsAndAreRemovedByUs)
{
	// Inside tmux every attached client reads the same image, and terminals
	// leave t=f files alone: rega removes them, MAX_IN_FLIGHT later.
	Capture cap;
	const Rgba8Image img = noise(16, 8);
	const std::string dir = std::filesystem::temp_directory_path().string();
	auto path = [&](int n)
	{
		std::string d = dir;
		while(d.size() > 1 && d.back() == '/')
		{
			d.pop_back();
		}
		return d + "/rega-" + std::to_string(getpid()) + "-" +
		       std::to_string(n) + ".rgba";
	};
	{
		kitty::Transmitter tx(true, Transfer::TEMP_FILE, cap.fn());
		const std::string esc = tx.encode(img, {});
		EXPECT_NE(esc.find("t=f,q=2"),
		          std::string::npos); // no S=: see transmitShared
		EXPECT_NE(esc.find(kitty::base64(reinterpret_cast<const uint8_t*>(
		                                     path(0).data()),
		                                 path(0).size())),
		          std::string::npos);
		std::ifstream in(path(0), std::ios::binary);
		const std::vector<uint8_t> got{std::istreambuf_iterator<char>(in), {}};
		EXPECT_EQ(got, img.m_pixels);
		for(size_t i = 0; i < kitty::Transmitter::MAX_IN_FLIGHT; ++i)
		{
			(void)tx.encode(img, {});
		}
		EXPECT_FALSE(std::filesystem::exists(path(0)));
		EXPECT_TRUE(std::filesystem::exists(path(1)));
	}
	EXPECT_FALSE(std::filesystem::exists(path(1))); // gone on exit
}

TEST(Transmitter, RemovesFilesLeftByDeadProcesses)
{
	// The highest pid a test can rely on being unused: a fork that exited.
	const pid_t dead = fork();
	if(dead == 0)
	{
		_exit(0);
	}
	waitpid(dead, nullptr, 0);
	const auto dir = std::filesystem::temp_directory_path();
	auto make = [&](const std::string& name)
	{
		std::ofstream(dir / name) << "x";
		return dir / name;
	};
	const auto stale = make("rega-" + std::to_string(dead) + "-3.rgba");
	const auto mine = make("rega-" + std::to_string(getpid()) + "-999.rgba");
	const auto other = make("rega-notes.rgba");
	Capture cap;
	kitty::Transmitter tx(true, Transfer::TEMP_FILE, cap.fn());
	EXPECT_FALSE(std::filesystem::exists(stale));
	EXPECT_TRUE(std::filesystem::exists(mine));  // a live process's
	EXPECT_TRUE(std::filesystem::exists(other)); // not ours
	std::filesystem::remove(mine);
	std::filesystem::remove(other);
}

TEST(Transmitter, QueuesDeletesUntilTaken)
{
	Capture cap;
	kitty::Transmitter tx(true, Transfer::DIRECT, cap.fn());
	EXPECT_FALSE(tx.hasPendingDeletes());
	tx.queueDelete(5);
	tx.queueDelete(6);
	EXPECT_TRUE(tx.hasPendingDeletes());
	EXPECT_EQ(tx.takePendingDeletes(),
	          kitty::deleteImage(5, true) + kitty::deleteImage(6, true));
	EXPECT_FALSE(tx.hasPendingDeletes());
}

namespace
{

// A kitty slot on a 10x4 screen drawing through a captured transmitter.
struct KittySlot
{
	TermCaps m_caps;
	Capture m_cap;
	kitty::Transmitter m_tx{false, Transfer::DIRECT, m_cap.fn()};
	std::atomic<int> m_ready{0};
	std::unique_ptr<ImageSlot> m_slot;
	ftxui::Screen m_screen{10, 4};
	ftxui::Box m_box{0, 9, 0, 3};

	KittySlot()
	{
		// As App sets it: with the 16 colours a bare environment (a build
		// sandbox, no TERM) implies, FTXUI folds the palette colours that
		// carry image ids together.
		ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
		m_caps.m_graphics = GraphicsMode::KITTY;
		m_caps.m_cellW = 2;
		m_caps.m_cellH = 4;
		m_slot =
		    std::make_unique<ImageSlot>(m_caps, m_tx, [this] { ++m_ready; });
	}
	void draw(const LayerImagePtr& img)
	{
		m_slot->draw(m_screen, m_box, img, {}, {});
	}
	// The kitty id an escape transmits under.
	static uint32_t idOf(const std::string& esc)
	{
		const size_t at = esc.find(",i=");
		return at == std::string::npos
		           ? 0
		           : static_cast<uint32_t>(std::stoul(esc.substr(at + 3)));
	}
};

} // namespace

namespace
{

// A frame read from `origin` (as ImageService tags them).
LayerImagePtr frame(float v, const std::string& origin)
{
	LayerImage img = *grey(v);
	img.m_origin = origin;
	return std::make_shared<const LayerImage>(std::move(img));
}

// Draw until `img` is on screen; what that wrote.
std::vector<std::string> show(KittySlot& k,
                              const LayerImagePtr& img,
                              const DisplayParams& disp = {})
{
	const int was = k.m_ready;
	k.m_slot->draw(k.m_screen, k.m_box, img, {}, disp);
	if(k.m_slot->pending())
	{
		EXPECT_TRUE(waitFor([&] { return k.m_ready > was; }));
		k.m_slot->draw(k.m_screen, k.m_box, img, {}, disp);
	}
	return k.m_cap.take();
}

size_t count(const std::vector<std::string>& writes, std::string_view what)
{
	return static_cast<size_t>(
	    std::ranges::count_if(writes,
	                          [&](const std::string& w)
	                          { return w.find(what) != w.npos; }));
}

} // namespace

TEST(ImageSlot, PlayedFramesStayInTheTerminalAndShowAgainByIdAlone)
{
	KittySlot k;
	k.m_tx.setTerminalCache(1 << 20);
	k.m_slot->setTerminalCache(true); // playing
	const auto a = frame(0.2f, "L|a|rgba|1"), b = frame(0.6f, "L|b|rgba|1");
	ASSERT_EQ(count(show(k, a), "a=T"), 1u);
	const auto idA = k.m_screen.PixelAt(0, 0).foreground_color;
	ASSERT_EQ(count(show(k, b), "a=T"), 1u);
	const auto idB = k.m_screen.PixelAt(0, 0).foreground_color;
	EXPECT_NE(idA, idB); // each under its own id
	EXPECT_EQ(k.m_tx.cacheUse().m_frames, 2u);

	// The loop comes round: nothing sent, the cells show the kept id. Also
	// for the same frame read again (the decode cache let it go).
	EXPECT_TRUE(show(k, a).empty());
	EXPECT_EQ(k.m_screen.PixelAt(0, 0).foreground_color, idA);
	EXPECT_TRUE(show(k, frame(0.6f, "L|b|rgba|1")).empty());
	EXPECT_EQ(k.m_screen.PixelAt(0, 0).foreground_color, idB);

	// Another exposure (or a pan, a zoom): a new picture. The kept ones stay
	// while there is room, for when the view comes back.
	DisplayParams brighter;
	brighter.m_exposure = 1.0f;
	const auto writes = show(k, a, brighter);
	EXPECT_EQ(count(writes, "a=T"), 1u);
	EXPECT_EQ(count(writes, "a=d,d=I"), 0u);
	EXPECT_EQ(k.m_slot->cachedFrames(), 3u);
	EXPECT_TRUE(show(k, b).empty()); // back: still there
	EXPECT_EQ(k.m_screen.PixelAt(0, 0).foreground_color, idB);
	// Kept in some view, by file (the playbar's dim cyan).
	EXPECT_TRUE(k.m_slot->storedInAnyView("a|"));
	EXPECT_FALSE(k.m_slot->storedInAnyView("c|"));

	// Paused: drawn fresh (full resolution), not from what was kept.
	k.m_slot->setTerminalCache(false);
	EXPECT_EQ(count(show(k, b, brighter), "a=T"), 1u);
}

TEST(ImageSlot, ALoopLongerThanTheBudgetKeepsItsFirstFrames)
{
	KittySlot k;
	// 10 x 4 cells of 2 x 4 px: 20 x 16 RGBA, 1280 bytes a frame.
	k.m_tx.setTerminalCache(3000);
	k.m_slot->setTerminalCache(true);
	const std::vector<LayerImagePtr> loop{frame(0.1f, "L|a|rgba|1"),
	                                      frame(0.4f, "L|b|rgba|1"),
	                                      frame(0.7f, "L|c|rgba|1")};
	for(const auto& f : loop)
	{
		const auto writes = show(k, f);
		EXPECT_EQ(count(writes, "a=T"), 1u);
		EXPECT_EQ(count(writes, "a=d,d=I"), 0u); // nothing dropped for it
	}
	EXPECT_EQ(k.m_slot->cachedFrames(), 2u);
	EXPECT_LE(k.m_tx.cacheUse().m_bytes, 3000u);
	// Every pass after: a and b shown by id, only c sent. Dropping the least
	// recently shown instead sent all three, every pass.
	for(int pass = 0; pass < 3; ++pass)
	{
		EXPECT_TRUE(show(k, loop[0]).empty());
		EXPECT_TRUE(show(k, loop[1]).empty());
		EXPECT_EQ(count(show(k, loop[2]), "a=T"), 1u);
	}
	EXPECT_EQ(k.m_tx.linkStats().m_keptShown, 6u);
}

TEST(ImageSlot, AnotherViewOrLayerMakesRoomOnlyWhenNeeded)
{
	KittySlot k;
	// Room for two 1280-byte frames.
	k.m_tx.setTerminalCache(3000);
	k.m_slot->setTerminalCache(true);
	const auto a = frame(0.1f, "L|shot.1001|rgba|2"),
	           b = frame(0.4f, "L|shot.1002|rgba|2");
	(void)show(k, a);
	(void)show(k, b);
	ASSERT_EQ(k.m_slot->cachedFrames(), 2u);
	// The alpha AOV: no room left, so the least recently shown rgba frame
	// makes it (one dropped, not both).
	const auto writes = show(k, frame(0.9f, "L|shot.1001|alpha|2"));
	EXPECT_EQ(count(writes, "a=T"), 1u);
	EXPECT_EQ(count(writes, "a=d,d=I"), 1u);
	EXPECT_EQ(k.m_slot->cachedFrames(), 2u);
	EXPECT_TRUE(show(k, b).empty());         // back to rgba: b stayed
	EXPECT_EQ(count(show(k, a), "a=T"), 1u); // a made the room
}

TEST(ImageSlot, FramesStoredWhilePausedPlayWithoutSendingAny)
{
	KittySlot k;
	k.m_tx.setTerminalCache(1 << 20);
	const std::vector<LayerImagePtr> loop{frame(0.1f, "L|a|rgba|1"),
	                                      frame(0.4f, "L|b|rgba|1"),
	                                      frame(0.7f, "L|c|rgba|1")};
	(void)show(k, loop[0]); // paused on the first frame: drawn as ever
	EXPECT_FALSE(k.m_slot->store(nullptr, 1000));
	for(const auto& f : loop) // what fillTerminal does, frame by frame
	{
		const int was = k.m_ready;
		ASSERT_TRUE(k.m_slot->store(f, 1000));
		EXPECT_FALSE(k.m_slot->store(f, 1000)); // one at a time
		ASSERT_TRUE(waitFor([&] { return k.m_ready > was; }));
		ASSERT_TRUE(k.m_slot->flushStored());
		EXPECT_TRUE(k.m_slot->stored(f->m_origin));
		EXPECT_FALSE(k.m_slot->storing());
	}
	const auto sent = k.m_cap.take();
	EXPECT_EQ(count(sent, "a=T"), 3u);
	EXPECT_FALSE(k.m_slot->store(loop[1], 1000)); // already kept
	// Not shown while storing: the cells still show the paused frame.
	const auto paused = k.m_screen.PixelAt(0, 0).foreground_color;
	k.m_slot->draw(k.m_screen, k.m_box, loop[0], {}, {});
	EXPECT_EQ(k.m_screen.PixelAt(0, 0).foreground_color, paused);
	// Play: every frame by its id alone, none sent.
	k.m_slot->setTerminalCache(true);
	for(int pass = 0; pass < 2; ++pass)
	{
		for(const auto& f : loop)
		{
			EXPECT_TRUE(show(k, f).empty());
		}
	}
	// The first pass starts on the paused frame, on screen already.
	EXPECT_EQ(k.m_tx.linkStats().m_keptShown, 5u);
}

TEST(ImageSlot, OnlyInlinePicturesWithAnOriginAreKept)
{
	KittySlot k;
	k.m_tx.setTerminalCache(1 << 20);
	k.m_slot->setTerminalCache(true);
	const auto untagged = grey(0.3f); // a picture made, not read
	(void)show(k, untagged);
	EXPECT_EQ(k.m_slot->cachedFrames(), 0u);
	k.m_tx.setTerminalCache(0); // --terminal-cache 0
	(void)show(k, frame(0.5f, "a"));
	EXPECT_EQ(k.m_slot->cachedFrames(), 0u);
}

TEST(ImageSlot, KittyPreparesInTheBackgroundThenSends)
{
	KittySlot k;
	const auto img = grey(0.5f);
	k.draw(img);
	EXPECT_TRUE(k.m_cap.take().empty()); // nothing yet: being prepared
	ASSERT_TRUE(waitFor([&] { return k.m_ready > 0; }));
	k.draw(img);
	const auto writes = k.m_cap.take();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0].rfind("\x1b_Ga=T", 0), 0u);
	EXPECT_TRUE(k.m_slot->drawn());
	EXPECT_EQ(k.m_screen.PixelAt(0, 0).character, kitty::placeholderCell(0, 0));
	// Unchanged: nothing more to prepare or send.
	k.draw(img);
	EXPECT_FALSE(k.m_slot->pending());
	EXPECT_TRUE(k.m_cap.take().empty());
}

TEST(ImageSlot, PrepareAheadIsQuietAndTheNextDrawSendsAtOnce)
{
	KittySlot k;
	const auto first = grey(0.25f), next = grey(0.75f);
	k.draw(first);
	ASSERT_TRUE(waitFor([&] { return k.m_ready > 0; }));
	k.draw(first);
	const uint32_t shown = KittySlot::idOf(k.m_cap.take().at(0));
	const int readies = k.m_ready;

	k.m_slot->prepareAhead(next);
	// A 20x16 px render: long done, with room for a slow CI machine.
	std::this_thread::sleep_for(500ms);
	EXPECT_EQ(k.m_ready.load(), readies); // quiet: no redraw asked for
	k.draw(next); // already prepared: goes out in this draw
	const auto writes = k.m_cap.take();
	ASSERT_EQ(writes.size(), 1u);
	// Replaces the picture under the same id: the cells stay, nothing to
	// delete.
	EXPECT_EQ(KittySlot::idOf(writes[0]), shown);
	EXPECT_FALSE(k.m_tx.hasPendingDeletes());
}

TEST(ImageSlot, ANewSizeReplacesThePlacementInsteadOfAddingOne)
{
	// The bug: zoomed tiles (and a fold) sent pictures of new sizes under the
	// same image id with no placement id; terminals kept every placement and
	// drew any of them, at stale sizes.
	KittySlot k;
	const auto img = grey(0.5f);
	k.draw(img);
	ASSERT_TRUE(waitFor([&] { return k.m_ready > 0; }));
	k.draw(img);
	auto writes = k.m_cap.take();
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_NE(writes[0].find(",U=1,p=1,"), std::string::npos) << writes[0];
	EXPECT_EQ(writes[0].find("a=d"), std::string::npos);

	// The same picture in a smaller box: the old placement goes first.
	k.m_box = {0, 4, 0, 1};
	const int readies = k.m_ready;
	k.draw(img);
	ASSERT_TRUE(waitFor([&] { return k.m_ready > readies; }));
	k.draw(img);
	writes = k.m_cap.take();
	ASSERT_EQ(writes.size(), 2u);
	EXPECT_EQ(writes[0],
	          kitty::deletePlacements(KittySlot::idOf(writes[1]), false));
	EXPECT_NE(writes[1].find(",p=1,c=5,r=2"), std::string::npos) << writes[1];
}

TEST(ImageSlot, EveryPictureGoesOutUnderTheSlotsOneId)
{
	KittySlot k;
	std::vector<LayerImagePtr> imgs;
	for(int i = 0; i < 12; ++i)
	{
		imgs.push_back(grey(i / 12.0f));
	}
	k.draw(imgs[0]);
	ASSERT_TRUE(waitFor([&] { return k.m_ready > 0; }));
	k.draw(imgs[0]);
	const uint32_t shown = KittySlot::idOf(k.m_cap.take().at(0));
	// Scrub: ahead requests superseding each other, draws in between.
	for(int i = 1; i < 12; ++i)
	{
		k.m_slot->prepareAhead(imgs[i]);
		if(i % 3 == 0)
		{
			// Draw until it goes out (it may be in progress or done).
			std::vector<std::string> writes;
			ASSERT_TRUE(waitFor(
			    [&]
			    {
				    k.draw(imgs[i]);
				    writes = k.m_cap.take();
				    return !writes.empty();
			    }));
			ASSERT_EQ(writes.size(), 1u);
			EXPECT_EQ(KittySlot::idOf(writes[0]), shown);
		}
	}
}

TEST(Transmitter, InlineWritesRunUpALinkBacklog)
{
	// tmux takes all a client over ssh can't yet: the writer keeps count.
	Capture cap;
	kitty::Transmitter tx(true, Transfer::DIRECT, cap.fn());
	EXPECT_EQ(tx.linkBacklog(), 0.0);
	tx.write(std::string(5000, 'x')); // unpaced: no rate
	EXPECT_EQ(tx.linkBacklog(), 0.0);
	tx.setLinkRate(10'000);
	tx.write(std::string(5000, 'x'));
	tx.write(std::string(5000, 'x'));
	const double backlog = tx.linkBacklog(); // a second's worth, draining
	EXPECT_GT(backlog, 0.8);
	EXPECT_LE(backlog, 1.0);

	// Shared memory / files never cross a link.
	kitty::Transmitter local(false, Transfer::SHARED_MEMORY, cap.fn());
	local.setLinkRate(10'000);
	local.write(std::string(5000, 'x'));
	EXPECT_EQ(local.linkRate(), 0.0);
	EXPECT_EQ(local.linkBacklog(), 0.0);
}

TEST(Transmitter, InlinePicturesMeasureWhatAPixelCosts)
{
	Capture cap;
	kitty::Transmitter tx(false, Transfer::DIRECT, cap.fn());
	EXPECT_EQ(tx.linkBytesPerPixel(),
	          kitty::Transmitter::DEFAULT_BYTES_PER_PIXEL);
	(void)tx.encode(noise(256, 256), {}); // noise: about 5.3 (RGBA, base64)
	EXPECT_GT(tx.linkBytesPerPixel(), 5.0);
	Rgba8Image flat = noise(256, 256);
	std::fill(flat.m_pixels.begin(), flat.m_pixels.end(), uint8_t(255));
	for(int i = 0; i < 20; ++i)
	{
		(void)tx.encode(flat, {}); // next to nothing, floored at 0.1
	}
	EXPECT_LT(tx.linkBytesPerPixel(), 0.2);
	EXPECT_GE(tx.linkBytesPerPixel(), 0.1);
	const double flatCost = tx.linkBytesPerPixel();
	kitty::TransmitOptions playbar;
	playbar.m_measureCost = false;
	(void)tx.encode(noise(256, 256), playbar); // not the picture: not counted
	EXPECT_EQ(tx.linkBytesPerPixel(), flatCost);
}

TEST(ImageSlot, ScrubShowsTheNewestPictureWhileTheNextIsPrepared)
{
	// The bug: scrubbing over ssh, every picture was done after the draw had
	// moved to another frame, so none showed until the mouse stopped.
	KittySlot k;
	const auto a = grey(0.2f), b = grey(0.4f), c = grey(0.6f);
	k.draw(a);
	ASSERT_TRUE(waitFor([&] { return k.m_ready == 1; }));
	k.draw(a);
	ASSERT_EQ(k.m_cap.take().size(), 1u);
	k.draw(b); // asked for, then the mouse moves on before it is done
	ASSERT_TRUE(waitFor([&] { return k.m_ready == 2; }));
	k.draw(c);
	EXPECT_EQ(k.m_cap.take().size(), 1u); // b, meanwhile
	ASSERT_TRUE(waitFor([&] { return k.m_ready == 3; }));
	k.draw(c);
	EXPECT_EQ(k.m_cap.take().size(), 1u); // then c
}
