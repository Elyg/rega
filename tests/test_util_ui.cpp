#include "util/Ui.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ftxui;
using namespace rega;

namespace
{

// The first row of `el` drawn `width` cells wide, as text (a wide glyph is
// one entry; the cell it spills into adds nothing; empty cells are spaces).
std::string draw(Element el, int width, Screen* out = nullptr)
{
	Screen screen(width, 1);
	Render(screen, el);
	std::string row;
	bool wide = false; // the cell before held a two-cell glyph
	for(int x = 0; x < width; ++x)
	{
		const std::string& c = screen.PixelAt(x, 0).character;
		row += c.empty() && !wide ? " " : c;
		wide = string_width(c) == 2;
	}
	if(out)
	{
		*out = screen;
	}
	return row;
}

// Trailing spaces off (the bar fills the width).
std::string trimmed(std::string s)
{
	while(!s.empty() && s.back() == ' ')
	{
		s.pop_back();
	}
	return s;
}

Elements frameStatus()
{
	return {text(" frame 1 ")}; // 9 cells
}

} // namespace

TEST(Ui, HighlightedMarksTheMatchedRuns)
{
	const std::vector<size_t> hits{0, 1, 4};
	Screen screen(0, 0);
	const std::string row =
	    draw(ui::highlighted("Beachball", hits, nothing), 12, &screen);
	EXPECT_EQ(trimmed(row), "Beachball");
	for(int x : {0, 1, 4})
	{
		EXPECT_TRUE(screen.PixelAt(x, 0).bold) << x;
		EXPECT_TRUE(screen.PixelAt(x, 0).underlined) << x;
	}
	for(int x : {2, 3, 5, 8})
	{
		EXPECT_FALSE(screen.PixelAt(x, 0).bold) << x;
	}
}

TEST(Ui, HighlightedWithoutHitsIsPlain)
{
	Screen screen(0, 0);
	EXPECT_EQ(trimmed(draw(ui::highlighted("abc", {}, nothing), 5, &screen)),
	          "abc");
	EXPECT_FALSE(screen.PixelAt(0, 0).bold);
}

TEST(Ui, PaneHintsReadAsKeyThenWhat)
{
	EXPECT_EQ(trimmed(
	              draw(ui::paneHints("3/10", {{"y", "copy"}, {"2", ""}}), 40)),
	          " 3/10 (y copy) (2)");
}

TEST(Ui, PaneTitleFillsTheWidth)
{
	const std::string t = ui::paneTitle("─[4]─Inspector", 20);
	EXPECT_EQ(string_width(t), 20);
	EXPECT_TRUE(t.starts_with("─[4]─Inspector──"));
	// Already wider: left as it is.
	EXPECT_EQ(ui::paneTitle("─[4]─Inspector", 5), "─[4]─Inspector");
}

TEST(Ui, StatusLineShowsHintsAndTailWhenTheyFit)
{
	const std::string row = draw(ui::statusLine({.m_hints = "q quit",
	                                             .m_status = frameStatus(),
	                                             .m_tail = "cache 1/2"},
	                                            40),
	                             40);
	EXPECT_EQ(row, " frame 1              q quit  cache 1/2 ");
}

TEST(Ui, StatusLineDropsHintsFirstThenTheTail)
{
	auto line = [](int width)
	{
		return draw(ui::statusLine({.m_hints = "q quit",
		                            .m_status = frameStatus(),
		                            .m_tail = "cache 1/2"},
		                           width),
		            width);
	};
	// 25 cells: the tail (11) fits, the hints (7) no longer do.
	EXPECT_EQ(line(25), " frame 1       cache 1/2 ");
	// 15 cells: neither; the status stays.
	EXPECT_EQ(trimmed(line(15)), " frame 1");
}

TEST(Ui, StatusLineMessageReplacesHintsAndIsTruncatedLast)
{
	const std::string row =
	    draw(ui::statusLine({.m_message = "copied: something long",
	                         .m_hints = "q quit",
	                         .m_status = frameStatus()},
	                        20),
	         20);
	EXPECT_EQ(row, " frame 1  copied: s…");
}

TEST(Ui, StatusLineMessageKeepsTheTailWhenItFits)
{
	const std::string row =
	    draw(ui::statusLine({.m_message = "hi", .m_tail = "cache"}, 20), 20);
	EXPECT_EQ(row, " hi           cache ");
}

TEST(Ui, StatusLineCutsWideGlyphsByWidth)
{
	// Two cells each: " 日本語の" is 9 cells, then the ellipsis.
	Screen screen(0, 0);
	const std::string row =
	    draw(ui::statusLine({.m_message = "日本語のテキスト"}, 10),
	         10,
	         &screen);
	EXPECT_EQ(row, " 日本語の…");
	EXPECT_EQ(string_width(row), 10);
}

TEST(Ui, StatusLineWarningStaysAfterTheStatus)
{
	const std::string row =
	    draw(ui::statusLine({.m_status = frameStatus(), .m_warning = "no tmux"},
	                        30),
	         30);
	EXPECT_TRUE(row.starts_with(" frame 1  ⚠ no tmux ")) << row;
}

TEST(Ui, EllipsizeEndKeepsWhatFitsAndCutsTheRest)
{
	EXPECT_EQ(ui::ellipsizeEnd("2.63.0", 10), "2.63.0");
	EXPECT_EQ(ui::ellipsizeEnd("/job/rendering/people", 8), "/job/re…");
	EXPECT_EQ(ui::ellipsizeEnd("abc", 1), "…");
	EXPECT_EQ(ui::ellipsizeEnd("abc", 0), "");
	// By width: a two-cell glyph that would straddle the cut goes whole.
	EXPECT_EQ(ui::ellipsizeEnd("日本語", 4), "日…");
}

TEST(Ui, EllipsizeStartKeepsTheTail)
{
	EXPECT_EQ(ui::ellipsizeStart("/a/b.exr", 10), "/a/b.exr");
	EXPECT_EQ(ui::ellipsizeStart("/job/shot/render.ass", 11), "…render.ass");
	EXPECT_EQ(ui::ellipsizeStart("/日本語", 4), "…語");
}

TEST(Ui, EllipsizeMiddleKeepsBothEnds)
{
	EXPECT_EQ(ui::ellipsizeMiddle("screenWindow", 20), "screenWindow");
	const std::string a =
	    ui::ellipsizeMiddle("shaders.camera.overscanLeft", 18);
	const std::string b =
	    ui::ellipsizeMiddle("shaders.camera.overscanRight", 18);
	EXPECT_EQ(a, "shaders.c…scanLeft");
	EXPECT_EQ(string_width(a), 18);
	EXPECT_NE(a, b); // names with a long shared prefix stay apart
	EXPECT_EQ(ui::ellipsizeMiddle("abcdef", 1), "…");
}

TEST(Ui, EllipsizeMiddleMovesHighlightsWithTheirBytes)
{
	// Hits on a, e (cut out) and i: "abc…hij", 'i' after the 3-byte "…".
	const std::vector<size_t> hits{0, 4, 8};
	const auto [cut, at] = ui::ellipsizeMiddle("abcdefghij", 7, hits);
	EXPECT_EQ(cut, "abc…hij");
	EXPECT_EQ(at, (std::vector<size_t>{0, 7}));
	EXPECT_EQ(cut.substr(at[1], 1), "i");
	// Whole: as they were.
	const auto [whole, same] = ui::ellipsizeMiddle("abcdefghij", 10, hits);
	EXPECT_EQ(whole, "abcdefghij");
	EXPECT_EQ(same, hits);
}

TEST(Ui, WrapWidthBreaksByCells)
{
	EXPECT_EQ(ui::wrapWidth("", 5), std::vector<std::string>{""});
	EXPECT_EQ(ui::wrapWidth("abcdefg", 3),
	          (std::vector<std::string>{"abc", "def", "g"}));
	EXPECT_EQ(ui::wrapWidth("日本語", 3),
	          (std::vector<std::string>{"日", "本", "語"}));
	EXPECT_EQ(ui::wrapWidth("ab", 0), (std::vector<std::string>{"a", "b"}));
}

TEST(UiFind, FirstMatchIgnoringCase)
{
	EXPECT_EQ(ui::findIgnoringCase("Play / Pause", "pause"),
	          (std::vector<size_t>{7, 8, 9, 10, 11}));
	EXPECT_EQ(ui::findIgnoringCase("in / out point", "O"),
	          (std::vector<size_t>{5})); // the first
	EXPECT_TRUE(ui::findIgnoringCase("zoom", "pan").empty());
	EXPECT_TRUE(ui::findIgnoringCase("zoom", "").empty());
}
