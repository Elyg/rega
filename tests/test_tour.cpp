#include "app/Tour.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

using rega::Tour;
using rega::TourView;

namespace
{

// The card's cells, rendered: (x, y) of where `word` starts, or nullopt.
std::optional<std::pair<int, int>> findOnScreen(const ftxui::Screen& screen,
                                                const std::string& word)
{
	for(int y = 0; y < screen.dimy(); ++y)
	{
		for(int x = 0; x + static_cast<int>(word.size()) <= screen.dimx(); ++x)
		{
			bool match = true;
			for(size_t i = 0; i < word.size() && match; ++i)
			{
				match = screen.PixelAt(x + static_cast<int>(i), y).character ==
				        std::string(1, word[i]);
			}
			if(match)
			{
				return std::pair(x, y);
			}
		}
	}
	return std::nullopt;
}

ftxui::Screen card(const Tour& t)
{
	auto screen = ftxui::Screen(60, 12);
	ftxui::Render(screen, t.render());
	return screen;
}

TourView browser()
{
	return {};
}

TourView viewer(const std::string& open, int sources = 1)
{
	return {.m_viewer = true, .m_open = open, .m_sources = sources};
}

ftxui::Event key(const std::string& k)
{
	return ftxui::Event::Character(k);
}

// Press `keys` and update with `now` after each, as App does.
bool pressAll(Tour& t,
              std::initializer_list<ftxui::Event> keys,
              const TourView& now)
{
	bool done = false;
	for(const auto& e : keys)
	{
		t.press(e);
		done = t.update(now) || done;
	}
	return done;
}

} // namespace

TEST(Tour, StepNeedsEveryKeyItNames)
{
	Tour t;
	EXPECT_EQ(t.keys(), (std::vector<std::string>{"j", "k"}));
	t.press(key("j"));
	EXPECT_FALSE(t.update(browser()));
	EXPECT_TRUE(t.pressed("j"));
	EXPECT_FALSE(t.pressed("k"));
	t.press(ftxui::Event::ArrowUp); // the arrows count as j / k
	EXPECT_TRUE(t.update(browser()));
	EXPECT_TRUE(t.done());
	EXPECT_EQ(t.step(), 0) << "a done step waits for a key";
}

TEST(Tour, NextKeyMovesOnAndCountsThere)
{
	Tour t;
	pressAll(t, {key("j"), key("k")}, browser());
	ASSERT_TRUE(t.done());
	t.press(ftxui::Event::Return); // moves on, and is this step's key
	EXPECT_EQ(t.title(), "Open the sequence");
	EXPECT_TRUE(t.pressed("Enter"));
	EXPECT_FALSE(t.update(viewer("chart.exr"))) << "not the sequence";
	EXPECT_TRUE(t.update(viewer("shot.####.exr")));
}

TEST(Tour, MouseMovesAndF1DoNotMoveOn)
{
	Tour t;
	pressAll(t, {key("j"), key("k")}, browser());
	ftxui::Mouse m;
	m.button = ftxui::Mouse::None;
	m.motion = ftxui::Mouse::Moved;
	t.press(ftxui::Event::Mouse("", m));
	t.press(ftxui::Event::F1);
	EXPECT_EQ(t.step(), 0);
	EXPECT_TRUE(t.done());
}

TEST(Tour, MouseKeys)
{
	Tour t;
	for(int i = 0; i < 9; ++i)
	{
		t.skip();
	}
	ASSERT_EQ(t.title(), "Read a pixel");
	ftxui::Mouse click;
	click.button = ftxui::Mouse::Left;
	click.motion = ftxui::Mouse::Pressed;
	t.press(ftxui::Event::Mouse("", click));
	EXPECT_FALSE(t.pressed("Ctrl+click")) << "no Ctrl";
	click.control = true;
	t.press(ftxui::Event::Mouse("", click));
	EXPECT_TRUE(t.pressed("Ctrl+click"));
	EXPECT_FALSE(t.update(viewer("shot.####.exr"))); // 4 still to press
	t.press(key("4"));
	EXPECT_TRUE(t.update(viewer("shot.####.exr")));
}

TEST(Tour, ChannelsNeedsAllSix)
{
	Tour t;
	for(int i = 0; i < 7; ++i)
	{
		t.skip();
	}
	ASSERT_EQ(t.title(), "Channels");
	const TourView v = viewer("shot.####.exr");
	EXPECT_FALSE(
	    pressAll(t, {key("c"), key("r"), key("g"), key("b"), key("a")}, v));
	EXPECT_TRUE(pressAll(t, {key("u")}, v));
}

TEST(Tour, PlaceMattersAsWellAsKeys)
{
	Tour t;
	for(int i = 0; i < 11; ++i)
	{
		t.skip();
	}
	ASSERT_EQ(t.title(), "Brighter than white");
	TourView v = viewer("hdr-sky.exr");
	v.m_exposure = -1.0f;
	EXPECT_FALSE(pressAll(t, {key("E")}, v)) << "not stopped down enough";
	v.m_exposure = -3.0f;
	EXPECT_TRUE(t.update(v));
}

TEST(Tour, BackRestartsThePreviousStep)
{
	Tour t;
	t.back(); // nowhere before the first
	EXPECT_EQ(t.step(), 0);
	pressAll(t, {key("j"), key("k")}, browser());
	t.press(ftxui::Event::Return);
	ASSERT_EQ(t.step(), 1);
	t.back();
	EXPECT_EQ(t.step(), 0);
	EXPECT_FALSE(t.done());
	EXPECT_FALSE(t.pressed("j")) << "its keys to press again";
	EXPECT_FALSE(findOnScreen(card(t), "F3")) << "no step before it";
	t.skip();
	EXPECT_TRUE(findOnScreen(card(t), "F3 back"));
}

TEST(Tour, SkipStopsAtTheLastStep)
{
	Tour t;
	for(int i = 0; i < Tour::stepCount() + 3; ++i)
	{
		t.skip();
	}
	EXPECT_TRUE(t.finished());
	EXPECT_EQ(t.step(), Tour::stepCount() - 1);
	EXPECT_FALSE(pressAll(t, {key("?"), key("2"), ftxui::Event::F1}, browser()))
	    << "the last step is never done";
	EXPECT_TRUE(t.pressed("?"));
}

TEST(Tour, CardShowsProgressAndKeys)
{
	Tour t;
	const ftxui::Screen screen = card(t);
	EXPECT_TRUE(findOnScreen(screen, "tutorial 1/"));
	EXPECT_TRUE(findOnScreen(screen, "The browser"));
	EXPECT_TRUE(findOnScreen(screen, "F2 skip"));
	EXPECT_FALSE(findOnScreen(screen, "{")) << "key markup is not shown";
}

TEST(Tour, KeysAreHighlighted)
{
	Tour t;
	const ftxui::Screen screen = card(t);
	// "{j} / {k} (or the arrows) move...": j is a key, "move" is not; keys
	// pressed turn green.
	const auto j = findOnScreen(screen, "j / k");
	const auto move = findOnScreen(screen, "move");
	ASSERT_TRUE(j && move);
	const auto& key = screen.PixelAt(j->first, j->second);
	EXPECT_TRUE(key.bold);
	EXPECT_EQ(key.foreground_color, ftxui::Color(ftxui::Color::Yellow));
	EXPECT_FALSE(screen.PixelAt(j->first + 2, j->second).bold); // the "/"
	EXPECT_FALSE(screen.PixelAt(move->first, move->second).bold);

	t.press(ftxui::Event::Character("k"));
	const ftxui::Screen after = card(t);
	EXPECT_EQ(after.PixelAt(j->first, j->second).foreground_color,
	          ftxui::Color(ftxui::Color::Yellow));
	EXPECT_EQ(after.PixelAt(j->first + 4, j->second).foreground_color,
	          ftxui::Color(ftxui::Color::Green));
}
