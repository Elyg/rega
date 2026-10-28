#include "app/Annotations.h"
#include "image/Overlay.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

using namespace rega;
namespace fs = std::filesystem;

namespace
{

KeyLookup mapLookup(std::map<std::string, std::string> attrs,
                    std::map<std::string, std::string> builtins)
{
	return [attrs, builtins](const std::string& k,
	                         bool builtin) -> std::optional<std::string>
	{
		const auto& m = builtin ? builtins : attrs;
		auto it = m.find(k);
		if(it == m.end())
		{
			return std::nullopt;
		}
		return it->second;
	};
}

std::string plain(const OverlayLine& l)
{
	std::string s;
	for(const auto& r : l)
	{
		s += r.m_text;
	}
	return s;
}

OverlayLine line(const std::string& s)
{
	return {{s, false}};
}

// Text on row `y` between columns [x0, x0 + w), spaces where nothing is.
std::string row(const std::vector<OverlayCell>& cells, int y, int x0, int w)
{
	std::string s(w, ' ');
	std::map<int, std::string> at;
	for(const auto& c : cells)
	{
		if(c.m_y == y && c.m_x >= x0 && c.m_x < x0 + w)
		{
			at[c.m_x - x0] = c.m_glyph;
		}
	}
	std::string out;
	for(int x = 0; x < w; ++x)
	{
		out += at.count(x) ? at[x] : " ";
	}
	return out;
}

} // namespace

TEST(Annotations, ResolvesAttributesAndBuiltins)
{
	auto look = mapLookup({{"owner", "jdoe"}, {"name", "beauty"}},
	                      {{"frame", "1001"}, {"file", "a.1001.exr"}});
	EXPECT_EQ(plain(resolveLine("[#owner] · f[#@frame]", look)),
	          "jdoe · f1001");
	// `name` is an attribute, `@name` is not a builtin: no clash.
	EXPECT_EQ(plain(resolveLine("[#name]/[#@file]", look)),
	          "beauty/a.1001.exr");
	auto l = resolveLine("x [#ownr] [#@nope] y", look);
	ASSERT_EQ(l.size(), 5u);
	EXPECT_EQ(l[1].m_text, "[#ownr]");
	EXPECT_TRUE(l[1].m_dim);
	EXPECT_EQ(l[3].m_text, "[#@nope]");
	EXPECT_TRUE(l[3].m_dim);
	// Not placeholders: plain brackets, an unclosed one, an empty key.
	EXPECT_EQ(plain(resolveLine("[x] [#owner", look)), "[x] [#owner");
	EXPECT_EQ(plain(resolveLine("a[#]b", look)), "a[#]b");
}

TEST(Annotations, LayoutCornersWinCentreTruncates)
{
	OverlayText t;
	t[static_cast<int>(Slot::TL)] = {line("left")};
	t[static_cast<int>(Slot::TC)] = {line("centre")};
	t[static_cast<int>(Slot::TR)] = {line("right")};
	// Wide frame: all three fit, inset by one cell.
	auto cells = layoutOverlay(0, 0, 30, 5, t);
	EXPECT_EQ(row(cells, 1, 0, 30), " left       centre      right ");
	// Narrow: the centre is squeezed into the gap with an ellipsis.
	cells = layoutOverlay(0, 0, 18, 5, t);
	EXPECT_EQ(row(cells, 1, 0, 18), " left cent… right ");
	// Too narrow for the centre: it is dropped, corners stay.
	cells = layoutOverlay(0, 0, 14, 5, t);
	EXPECT_EQ(row(cells, 1, 0, 14), " left   right ");
	// Corners share the width when both are too long.
	t[static_cast<int>(Slot::TC)] = {};
	t[static_cast<int>(Slot::TL)] = {line("abcdefghij")};
	t[static_cast<int>(Slot::TR)] = {line("klmnopqrst")};
	cells = layoutOverlay(0, 0, 12, 5, t);
	EXPECT_EQ(row(cells, 1, 0, 12), " abcd… klm… ");
}

TEST(Annotations, LayoutStacksAndBottomWins)
{
	OverlayText t;
	t[static_cast<int>(Slot::TL)] = {line("t1"), line("t2"), line("t3")};
	t[static_cast<int>(Slot::BR)] = {line("b1"), line("b2")};
	// Tall enough: top grows down, bottom ends on the last inner row.
	auto cells = layoutOverlay(10, 20, 10, 8, t);
	EXPECT_EQ(row(cells, 21, 10, 10), " t1       ");
	EXPECT_EQ(row(cells, 23, 10, 10), " t3       ");
	EXPECT_EQ(row(cells, 25, 10, 10), "       b1 ");
	EXPECT_EQ(row(cells, 26, 10, 10), "       b2 ");
	// 3 inner rows: both bottom lines stay, the top keeps only its first.
	cells = layoutOverlay(0, 0, 10, 5, t);
	EXPECT_EQ(row(cells, 1, 0, 10), " t1       ");
	EXPECT_EQ(row(cells, 2, 0, 10), "       b1 ");
	EXPECT_EQ(row(cells, 3, 0, 10), "       b2 ");
	// Nothing ever lands outside the frame.
	cells = layoutOverlay(0, 0, 4, 2, t);
	for(const auto& c : cells)
	{
		EXPECT_GE(c.m_x, 0);
		EXPECT_LT(c.m_x, 4);
		EXPECT_GE(c.m_y, 0);
		EXPECT_LT(c.m_y, 2);
	}
}

TEST(Annotations, StateRoundTripPruneAndHistory)
{
	Annotations a;
	a.global().lines(Slot::TL).push_back("[#owner]");
	a.global().lines(Slot::BC).push_back("  [#@file]  ·  [#@frame]");
	a.source("/shots/a.####.exr").lines(Slot::BL).push_back("lighting v3");
	a.source("/shots/empty.exr"); // empty sets are not written
	for(int i = 0; i < 120; ++i)
	{
		a.addHistory("line " + std::to_string(i % 110));
	}
	a.addHistory("   ");
	EXPECT_EQ(a.history().size(), Annotations::MAX_HISTORY);
	EXPECT_EQ(a.history().front(), "line 9");

	const fs::path p = fs::temp_directory_path() / "rega-tests" / "ann-state";
	fs::remove(p);
	ASSERT_TRUE(a.save(p));
	Annotations b;
	ASSERT_TRUE(b.load(p));
	EXPECT_EQ(b.global().lines(Slot::TL), std::vector<std::string>{"[#owner]"});
	EXPECT_EQ(b.global().lines(Slot::BC),
	          std::vector<std::string>{"  [#@file]  ·  [#@frame]"});
	ASSERT_NE(b.findSource("/shots/a.####.exr"), nullptr);
	EXPECT_EQ(b.findSource("/shots/a.####.exr")->lines(Slot::BL),
	          std::vector<std::string>{"lighting v3"});
	EXPECT_EQ(b.findSource("/shots/empty.exr"), nullptr);
	EXPECT_EQ(b.history(), a.history());

	// Only the most recently used sources are kept.
	Annotations c;
	for(size_t i = 0; i < Annotations::MAX_SOURCES + 5; ++i)
	{
		c.source("/s/" + std::to_string(i)).lines(Slot::TL).push_back("x");
	}
	Annotations d;
	d.parse(c.serialize());
	EXPECT_NE(d.findSource("/s/204"), nullptr);
	EXPECT_EQ(d.findSource("/s/4"), nullptr);
	EXPECT_NE(d.findSource("/s/5"), nullptr);
}

TEST(Labels, SplitOffThePathOneSlotEach)
{
	auto [path, labels] =
	    splitLabels("dir/a.exr:label=top:main v2:label=bl:[#@res]");
	EXPECT_EQ(path, "dir/a.exr");
	EXPECT_EQ(labels.lines(Slot::TC), std::vector<std::string>{"main v2"});
	EXPECT_EQ(labels.lines(Slot::BL), std::vector<std::string>{"[#@res]"});
	// The text runs to the next label, colons and all.
	EXPECT_EQ(splitLabels("a.exr:label=tr:10:30").second.lines(Slot::TR),
	          std::vector<std::string>{"10:30"});
	EXPECT_EQ(splitLabels("a.exr:label=bottom:x").second.lines(Slot::BC),
	          std::vector<std::string>{"x"});
}

TEST(Labels, AnExistingFileOrAnUnknownSlotStaysWhole)
{
	EXPECT_EQ(splitLabels("a.exr:label=middle:x").first,
	          "a.exr:label=middle:x");
	EXPECT_TRUE(splitLabels("a.exr:label=middle:x").second.empty());
	EXPECT_TRUE(splitLabels("plain.exr").second.empty());
	const fs::path odd =
	    fs::temp_directory_path() / "rega-tests" / "x.exr:label=top:y";
	fs::create_directories(odd.parent_path());
	std::ofstream(odd) << "";
	EXPECT_EQ(splitLabels(odd.string()).first, odd.string());
	EXPECT_TRUE(splitLabels(odd.string()).second.empty());
}
