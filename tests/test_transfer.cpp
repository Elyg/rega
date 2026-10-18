// Getting kitty images to the terminal: the escapes, shared memory, and
// ImageSlot preparing pictures on its own thread.

#include "term/ImageView.h"
#include "term/Kitty.h"

#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rv;
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
	return "/rvtui-" + std::to_string(getpid()) + "-" + std::to_string(n);
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

TEST(Kitty, CompressesOnlyWhenItPays)
{
	const Rgba8Image grain = noise(512, 512); // 1 MiB that won't shrink
	EXPECT_FALSE(
	    kitty::worthCompressing(grain.m_pixels.data(), grain.m_pixels.size()));
	const std::vector<uint8_t> flat(1 << 20, 7);
	EXPECT_TRUE(kitty::worthCompressing(flat.data(), flat.size()));
	EXPECT_TRUE(kitty::worthCompressing(grain.m_pixels.data(), 1000)); // small
}

TEST(Kitty, TransmitSendsIncompressiblePixelsUncompressed)
{
	kitty::TransmitOptions opt;
	opt.m_id = 9;
	const std::string s = kitty::transmit(noise(512, 256), opt);
	EXPECT_EQ(s.rfind("\x1b_Ga=T,f=32,q=2,s=512,v=256,i=9,U=1,p=1,m=1;", 0),
	          0u);
	EXPECT_EQ(s.find("o=z"), std::string::npos);
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
	// leave t=f files alone: rvtui removes them, MAX_IN_FLIGHT later.
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
		return d + "/rvtui-" + std::to_string(getpid()) + "-" +
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
	const auto stale = make("rvtui-" + std::to_string(dead) + "-3.rgba");
	const auto mine = make("rvtui-" + std::to_string(getpid()) + "-999.rgba");
	const auto other = make("rvtui-notes.rgba");
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
