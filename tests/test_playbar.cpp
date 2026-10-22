#include "app/Playbar.h"
#include "app/Player.h"
#include "term/Kitty.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

using namespace rv;

namespace
{

// Row `y` of the playbar `el` drawn `width` cells wide, as text.
std::string draw(ftxui::Element el, int width, int y)
{
	ftxui::Screen screen(width, 2);
	ftxui::Render(screen, el);
	std::string row;
	for(int x = 0; x < width; ++x)
	{
		const std::string& c = screen.PixelAt(x, y).character;
		row += c.empty() ? " " : c;
	}
	return row;
}

PlaybarState sequence(int frames, int frame)
{
	PlaybarState s;
	s.m_frames = frames;
	s.m_frame = frame;
	s.m_range = {0, frames - 1};
	s.m_label = [](int f) { return std::to_string(1001 + f); };
	return s;
}

} // namespace

TEST(FrameRange, StepsWrapInsideAndEnterFromOutside)
{
	const FrameRange r{3, 5};
	EXPECT_EQ(r.count(), 3);
	EXPECT_EQ(r.next(3), 4);
	EXPECT_EQ(r.next(5), 3);
	EXPECT_EQ(r.prev(3), 5);
	EXPECT_EQ(r.prev(4), 3);
	EXPECT_EQ(r.next(0), 3); // before it: to the start
	EXPECT_EQ(r.next(9), 3);
	EXPECT_EQ(r.prev(9), 5); // past it: to the end
	EXPECT_TRUE(r.contains(5));
	EXPECT_FALSE(r.contains(6));
}

TEST(FrameRange, PlayerLoopsOverTheRangeAndPrefetchesInsideIt)
{
	Player::Clock::time_point now{};
	Player p({}, [&] { return now; });
	p.setFps(10);
	p.play();
	std::vector<int> shown, prefetched;
	int frame = 0; // outside: playback starts at the in point
	for(int i = 0; i < 4; ++i)
	{
		now += std::chrono::milliseconds(100);
		prefetched.clear();
		auto next = p.tick(
		    frame,
		    FrameRange{3, 5},
		    8,
		    [&](int f) { prefetched.push_back(f); },
		    [](int) { return true; });
		ASSERT_TRUE(next);
		frame = *next;
		shown.push_back(frame);
	}
	EXPECT_EQ(shown, (std::vector<int>{3, 4, 5, 3}));
	// From 5: the next ones round the range, as many as there are others.
	EXPECT_EQ(prefetched, (std::vector<int>{3, 4}));
}

TEST(Playbar, CellsShareFramesOrFramesShareCells)
{
	// Fewer frames than cells: each frame a run of cells.
	EXPECT_EQ(trackFrameAt(0, 10, 2), 0);
	EXPECT_EQ(trackFrameAt(4, 10, 2), 0);
	EXPECT_EQ(trackFrameAt(5, 10, 2), 1);
	EXPECT_EQ(trackFrameAt(9, 10, 2), 1);
	// More: each cell the first of its frames.
	EXPECT_EQ(trackFrameAt(0, 2, 10), 0);
	EXPECT_EQ(trackFrameAt(1, 2, 10), 5);
	// Off the ends: clamped.
	EXPECT_EQ(trackFrameAt(-3, 10, 2), 0);
	EXPECT_EQ(trackFrameAt(30, 10, 2), 1);
}

TEST(Playbar, ShowsTheCountTheBarTheNumbersAndTheFps)
{
	Playbar bar;
	const PlaybarState s = sequence(20, 0);
	const std::string row = draw(bar.render(s, 80), 80, 1);
	EXPECT_EQ(row.rfind("    20 frames ▀", 0), 0u) << row; // room for 20/20
	EXPECT_NE(row.find("‖ 24.00  0.0 fps "), std::string::npos) << row;
	// The current frame over the bar's start, the last at its end.
	EXPECT_EQ(draw(bar.render(s, 80), 80, 0),
	          std::string(14, ' ') + "1001" + std::string(32, ' ') + "1020" +
	              std::string(26, ' ')); // 2 cells a frame: 40 of 48
}

TEST(Playbar, InAndOutThinTheBarOutsideThem)
{
	Playbar bar;
	PlaybarState s = sequence(20, 10);
	s.m_range = {5, 14};
	s.m_hasIn = s.m_hasOut = true;
	s.m_playing = true;
	s.m_measured = 23.9;
	const std::string row = draw(bar.render(s, 80), 80, 1);
	EXPECT_EQ(row.rfind(" 10/20 frames ▔", 0), 0u) << row;
	EXPECT_NE(row.find("▀▔"), std::string::npos) << row;
	EXPECT_NE(row.find("▸ 24.00 23.9 fps "), std::string::npos) << row;
	const std::string labels = draw(bar.render(s, 80), 80, 0);
	EXPECT_NE(labels.find("1006"), std::string::npos) << labels; // in
	EXPECT_NE(labels.find("1011"), std::string::npos) << labels; // current
	EXPECT_NE(labels.find("1015"), std::string::npos) << labels; // out
}

TEST(Playbar, FrameAtFollowsTheTrackAsDrawn)
{
	Playbar bar;
	const PlaybarState s = sequence(20, 0);
	ftxui::Screen screen(80, 2);
	ftxui::Render(screen, bar.render(s, 80));
	EXPECT_TRUE(bar.contains(40, 1));
	EXPECT_FALSE(bar.contains(40, 2));
	EXPECT_EQ(bar.frameAt(0), 0);   // the count, left of the track
	EXPECT_EQ(bar.frameAt(79), 19); // the fps, right of it
	EXPECT_EQ(bar.frameAt(40), 13); // cell 26 of 40, 2 cells a frame
}

TEST(Playbar, ACellAcrossInOrOutIsCachedByItsFramesInside)
{
	// 40 frames on a 10-cell bar: 4 a cell. In / out at 2..5 straddle
	// cells 0 and 1; only frames 2..5 are decoded.
	PlaybarState s = sequence(40, 20);
	s.m_range = {2, 5};
	s.m_hasIn = s.m_hasOut = true;
	s.m_cached.assign(40, false);
	for(int f = 2; f <= 5; ++f)
	{
		s.m_cached[f] = true;
	}
	Playbar bar;
	ftxui::Screen screen(12, 2);
	ftxui::Render(screen, bar.render(s, 12)); // narrow: the bar only
	EXPECT_EQ(screen.PixelAt(1, 1).foreground_color,
	          ftxui::Color::RGB(0x9e, 0xcd, 0x6a)); // decoded
}

TEST(Playbar, TheBarGivesEveryCellTheSameNumberOfFrames)
{
	EXPECT_EQ(evenTrackWidth(400, 86), 80); // 5 frames a cell
	EXPECT_EQ(evenTrackWidth(8, 86), 80);   // 10 cells a frame
	EXPECT_EQ(evenTrackWidth(400, 80), 80); // already even
	EXPECT_EQ(evenTrackWidth(86, 86), 86);
	// Evening out would leave too much empty: all of it, uneven.
	EXPECT_EQ(evenTrackWidth(90, 86), 86); // 45 at 2 a cell
	EXPECT_EQ(evenTrackWidth(50, 86), 86); // 50 at 1 cell a frame
	EXPECT_EQ(evenTrackWidth(1, 0), 1);
}

TEST(Playbar, TheLastCellReachesTheLastFrame)
{
	// 400 frames on 80 cells: 5 a cell. The last cell is the last frame,
	// not the first of its five.
	Playbar bar;
	PlaybarState s = sequence(400, 0);
	ftxui::Screen screen(120, 2);
	ftxui::Render(screen, bar.render(s, 120));
	int last = -1; // the bar's last column
	for(int x = 0; x < 120; ++x)
	{
		if(screen.PixelAt(x, 1).character == "▀")
		{
			last = x;
		}
	}
	ASSERT_GT(last, 0);
	EXPECT_EQ(bar.frameAt(last), 399);
	EXPECT_EQ(bar.frameAt(last + 3), 399); // in the gap after it
	EXPECT_EQ(bar.frameAt(last - 1), 390); // the cell before: its first
}

namespace
{

// The RGB of pixel (x, y), or nothing where transparent.
std::optional<std::array<uint8_t, 3>> pixel(const Rgba8Image& img, int x, int y)
{
	const uint8_t* p =
	    &img.m_pixels[(static_cast<size_t>(y) * img.m_width + x) * 4];
	if(!p[3])
	{
		return std::nullopt;
	}
	return std::array<uint8_t, 3>{p[0], p[1], p[2]};
}

} // namespace

TEST(Playbar, ThePictureHasTheCurrentFrameAtItsExactPlace)
{
	// 400 frames on 1000 px: 2.5 px a frame, the playhead at least 3. Two
	// rows of 20 px: the numbers, then the bar (its top half: 20..29).
	PlaybarState s = sequence(400, 200);
	s.m_cached.assign(400, true);
	const Rgba8Image img = paintPlaybar(s, 1000, 40);
	ASSERT_EQ(img.m_width, 1000);
	ASSERT_EQ(img.m_height, 40);
	const auto bar = pixel(img, 10, 20);
	const auto head = pixel(img, 500, 20);
	ASSERT_TRUE(bar && head);
	EXPECT_NE(*bar, *head);
	EXPECT_EQ(pixel(img, 502, 20), head);
	EXPECT_EQ(pixel(img, 503, 20), bar); // past it
	EXPECT_EQ(pixel(img, 499, 20), bar); // before it
	// The next frame moves it by its 2.5 px, not by a whole cell.
	s.m_frame = 201;
	const Rgba8Image next = paintPlaybar(s, 1000, 40);
	EXPECT_EQ(pixel(next, 501, 20), bar);
	EXPECT_EQ(pixel(next, 502, 20), head);
	// The bar's row: its top half only.
	EXPECT_TRUE(pixel(img, 10, 29));
	EXPECT_FALSE(pixel(img, 10, 30));
}

TEST(Playbar, TheNumberGlidesWithThePlayhead)
{
	// Where the current frame's number starts: its leftmost lit pixel in
	// the numbers row.
	auto numberX = [](const Rgba8Image& img)
	{
		for(int x = 0; x < img.m_width; ++x)
		{
			for(int y = 0; y < img.m_height / 2; ++y)
			{
				if(pixel(img, x, y) == std::array<uint8_t, 3>{0xd4, 0xf7, 0xcc})
				{
					return x;
				}
			}
		}
		return -1;
	};
	PlaybarState s = sequence(400, 200);
	const int at200 = numberX(paintPlaybar(s, 1000, 40));
	s.m_frame = 202;
	const int at202 = numberX(paintPlaybar(s, 1000, 40));
	ASSERT_GT(at200, 0);
	EXPECT_EQ(at202 - at200, 5); // two frames of 2.5 px
	// The ends' numbers are there too, in grey.
	const Rgba8Image img = paintPlaybar(s, 1000, 40);
	bool start = false; // the first frame's number, at the bar's start
	for(int x = 0; x < 20; ++x)
	{
		for(int y = 0; y < 20; ++y)
		{
			start = start || pixel(img, x, y).has_value();
		}
	}
	EXPECT_TRUE(start);
}

TEST(Playbar, FramesKeptInTheTerminalAreCyanBesideDecodedGreen)
{
	// 10 frames, a cell each: 0..3 kept in the terminal (and decoded),
	// 4..6 decoded only, the rest neither; the current frame is 9.
	PlaybarState s = sequence(10, 9);
	s.m_cached.assign(10, false);
	s.m_stored.assign(10, false);
	for(int f = 0; f <= 6; ++f)
	{
		s.m_cached[f] = true;
		s.m_stored[f] = f <= 3;
	}
	Playbar bar;
	ftxui::Screen screen(10, 2);
	ftxui::Render(screen, bar.render(s, 10)); // narrow: the bar only
	EXPECT_EQ(screen.PixelAt(1, 1).foreground_color,
	          ftxui::Color::RGB(0x7d, 0xcf, 0xff)); // kept
	EXPECT_EQ(screen.PixelAt(5, 1).foreground_color,
	          ftxui::Color::RGB(0x9e, 0xcd, 0x6a)); // decoded
	// The picture too (10 px a frame; the bar's row is 16..31).
	const Rgba8Image img = paintPlaybar(s, 100, 32);
	EXPECT_EQ(pixel(img, 15, 23), (std::array<uint8_t, 3>{0x7d, 0xcf, 0xff}));
	EXPECT_EQ(pixel(img, 55, 23), (std::array<uint8_t, 3>{0x9e, 0xcd, 0x6a}));
	// Kept for another view only (a pan away): dim cyan, still there.
	s.m_storedElsewhere.assign(10, false);
	s.m_storedElsewhere[5] = true;
	ftxui::Render(screen, bar.render(s, 10));
	EXPECT_EQ(screen.PixelAt(5, 1).foreground_color,
	          ftxui::Color::RGB(0x3b, 0x6a, 0x82));
	EXPECT_EQ(screen.PixelAt(6, 1).foreground_color,
	          ftxui::Color::RGB(0x9e, 0xcd, 0x6a));
}

TEST(Playbar, ThePictureThinsOutsideInOutAndGreysWhatIsNotDecoded)
{
	PlaybarState s = sequence(10, 5);
	s.m_range = {4, 7};
	s.m_hasIn = s.m_hasOut = true;
	s.m_cached.assign(10, false);
	s.m_cached[4] = true;
	const Rgba8Image img = paintPlaybar(s, 100, 32); // rows of 16 px
	EXPECT_TRUE(pixel(img, 5, 17)); // outside: two rows (16 / 8)
	EXPECT_FALSE(pixel(img, 5, 18));
	EXPECT_TRUE(pixel(img, 45, 23));                   // inside: eight (16 / 2)
	EXPECT_NE(pixel(img, 45, 16), pixel(img, 65, 16)); // decoded / not
}

TEST(Playbar, WithKittyTheBarIsAPictureSentOnlyWhenItChanges)
{
	std::string sent;
	kitty::Transmitter tx(false,
	                      Transfer::DIRECT,
	                      [&](std::string_view b, bool) { sent += b; });
	TermCaps caps;
	caps.m_graphics = GraphicsMode::KITTY;
	caps.m_cellW = 4;
	caps.m_cellH = 8;
	Playbar bar(caps, tx);
	PlaybarState s = sequence(400, 0);
	auto draw = [&]
	{
		ftxui::Screen screen(80, 2);
		ftxui::Render(screen, bar.render(s, 80));
		return screen;
	};
	ftxui::Screen screen = draw();
	EXPECT_FALSE(sent.empty());
	EXPECT_TRUE(kitty::placeholderRowCol(screen.PixelAt(40, 1).character));
	sent.clear();
	draw(); // nothing changed
	EXPECT_TRUE(sent.empty());
	s.m_frame = 50; // a few pixels on: a new picture
	draw();
	EXPECT_FALSE(sent.empty());
}
