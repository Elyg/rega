#include "TestContext.h"
#include "app/ColourPane.h"
#include "app/FilesPane.h"
#include "app/InspectorPane.h"
#include "app/LayersPane.h"
#include "app/MetaPane.h"
#include "app/ViewerState.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>

using namespace rega;
using namespace regatest;
using ftxui::Event;

namespace
{

// Clipboard writes (OSC 52) go to std::cout: keep them out of the test log.
struct MuteCout
{
	std::ostringstream m_sink;
	std::streambuf* m_old = std::cout.rdbuf(m_sink.rdbuf());
	~MuteCout()
	{
		std::cout.rdbuf(m_old);
	}
};

Entry file(const std::string& name)
{
	Entry e;
	e.m_kind = Entry::Kind::FILE;
	e.m_name = name;
	e.m_path = "/shots/" + name;
	return e;
}

Entry sequence(const std::string& name, int frames)
{
	Entry e;
	e.m_kind = Entry::Kind::SEQUENCE;
	e.m_name = name;
	for(int i = 1; i <= frames; ++i)
	{
		e.m_frames.push_back("/shots/" + name + "." + std::to_string(i) +
		                     ".exr");
		e.m_frameNumbers.push_back(i);
	}
	e.m_path = e.m_frames.front();
	return e;
}

// A header with one part (two attributes) and the given layers.
ImageInfoPtr info(const std::vector<std::string>& layers)
{
	auto i = std::make_shared<ImageInfo>();
	PartInfo part;
	part.m_displayWindow = {0, 0, 7, 3};
	part.m_dataWindow = part.m_displayWindow;
	part.m_attributes = {{"compression", "zip", "zip"},
	                     {"owner", "string", "me"}};
	i->m_parts.push_back(part);
	for(const auto& name : layers)
	{
		LayerInfo l;
		l.m_layer = name;
		l.m_channels = {name.empty() ? "R" : name + ".R"};
		i->m_layers.push_back(l);
	}
	return i;
}

struct Panes : ::testing::Test
{
	Context m_c;
	ViewerState m_state;
	Panes()
	{
		m_state.m_sources = {{file("a.exr")}, {file("b.exr")}, {file("c.exr")}};
	}
};

} // namespace

// --- inspector ---

TEST_F(Panes, InspectorCursorClampsAndGgGoesToTheTop)
{
	InspectorPane p(m_state, m_c.m_ctx);
	EXPECT_TRUE(p.event(key("j"))); // nothing picked: stays put
	EXPECT_EQ(p.cursor(), 0);

	Sample s;
	s.m_state = Sample::State::OK;
	s.m_x = 1;
	s.m_y = 2;
	s.m_values = {{"R", 0.5f}, {"G", 0.25f}};
	m_state.m_picked = s;
	const int n = static_cast<int>(sampleItems(s).size());
	for(int i = 0; i < n + 3; ++i)
	{
		EXPECT_TRUE(p.event(key("j")));
	}
	EXPECT_EQ(p.cursor(), n - 1);
	EXPECT_TRUE(p.event(key("g")));
	EXPECT_EQ(p.cursor(), n - 1); // one g waits for the second
	EXPECT_TRUE(p.event(key("g")));
	EXPECT_EQ(p.cursor(), 0);
	EXPECT_TRUE(p.event(key("G")));
	EXPECT_EQ(p.cursor(), n - 1);
	EXPECT_TRUE(p.event(key("k")));
	EXPECT_EQ(p.cursor(), n - 2);
}

TEST_F(Panes, InspectorCopiesTheLineOrValue)
{
	MuteCout mute;
	InspectorPane p(m_state, m_c.m_ctx);
	EXPECT_TRUE(p.event(key("y")));
	EXPECT_EQ(m_c.m_ctx.m_message,
	          "ctrl+click the image to pick a pixel first");
	Sample s;
	s.m_x = 1;
	s.m_y = 2;
	m_state.m_picked = s;
	EXPECT_TRUE(p.event(key("y")));
	EXPECT_EQ(m_c.m_ctx.m_message, "copied: pixel: [1, 2]");
	EXPECT_TRUE(p.event(key("Y")));
	EXPECT_EQ(m_c.m_ctx.m_message, "copied: [1, 2]");
	EXPECT_TRUE(p.event(key("x"))); // clears the pick
	EXPECT_FALSE(m_state.m_picked);
	EXPECT_EQ(m_c.m_ctx.m_message, "pick cleared");
}

TEST_F(Panes, InspectorCountsPixelsAsNukeDoes)
{
	// File row 136 of a 480x270 frame is Nuke's y 133 (from the bottom).
	Sample s;
	s.m_exact = true;
	s.m_x = 220;
	s.m_y = 136;
	s.m_frame = {0, 0, 479, 269};
	EXPECT_EQ(sampleCoord(s), "[220, 133]");
	EXPECT_EQ(sampleItems(s)[0].second, "[220, 133]");
	// Offset display window: counted from its own bottom-left corner.
	s.m_frame = {10, 20, 489, 289};
	EXPECT_EQ(sampleCoord(s), "[210, 153]");
	s.m_frame = {}; // unknown frame: file coordinates
	EXPECT_EQ(sampleCoord(s), "[220, 136]");
}

TEST_F(Panes, InspectorPassesOnViewerKeysAndClosesOnItsNumber)
{
	InspectorPane p(m_state, m_c.m_ctx);
	p.setOpen(true);
	m_state.m_focus = Focus::INSPECT;
	EXPECT_FALSE(p.event(key("e"))); // exposure: the viewer's
	EXPECT_FALSE(p.event(key("]")));
	EXPECT_TRUE(p.event(key("1"))); // focus back to the image, still open
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
	EXPECT_TRUE(p.isOpen());
	m_state.m_focus = Focus::INSPECT;
	EXPECT_TRUE(p.event(key("4")));
	EXPECT_FALSE(p.isOpen());
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
}

TEST_F(Panes, InspectorFlagsNanAndInf)
{
	Sample ok;
	ok.m_values = {{"R", 0.5f}};
	Sample nan = ok;
	nan.m_values.emplace_back("Z", std::nanf(""));
	Sample inf = ok;
	inf.m_rgba[1] = -INFINITY;
	EXPECT_EQ(nonFiniteLabel({&ok, nullptr}), "");
	EXPECT_EQ(nonFiniteLabel({&nan}), "NaN");
	EXPECT_EQ(nonFiniteLabel({&ok, &inf}), "inf");
	EXPECT_EQ(nonFiniteLabel({&nan, &inf}), "NaN inf");

	// The picked pixel's NaN shows as a badge in the [4] title.
	InspectorPane p(m_state, m_c.m_ctx);
	nan.m_state = Sample::State::OK;
	m_state.m_picked = nan;
	ftxui::Screen screen(60, 30);
	ftxui::Render(screen, p.render(Sample{}));
	std::string title;
	for(int x = 0; x < 21; ++x)
	{
		title += screen.at(x, 0);
	}
	EXPECT_EQ(title, "─[4]─Inspector─ NaN ─");

	// The whole layer's count, under the title.
	EXPECT_EQ(nonFiniteSummary(rega::LayerImage{}), "");
	rega::LayerImage layer;
	layer.m_nanPixels = 12;
	EXPECT_EQ(nonFiniteSummary(layer), "12 NaN px");
	layer.m_infPixels = 3;
	EXPECT_EQ(nonFiniteSummary(layer), "12 NaN · 3 inf px");
	m_state.m_picked.reset();
	ftxui::Render(screen, p.render(Sample{}, &layer));
	std::string row;
	for(int x = 0; x < 20; ++x)
	{
		row += screen.at(x, 1);
	}
	EXPECT_EQ(row, "  12 NaN · 3 inf px ");

	// Per-channel min / max / avg of the layer, at the bottom.
	layer.m_channelNames = {"R", "Z"};
	layer.m_stats = {{-0.5f, 2.0f, 0.25, 10}, {}};
	ftxui::Render(screen, p.render(Sample{}, &layer));
	auto line = [&](int y)
	{
		std::string l;
		for(int x = 0; x < 32; ++x)
		{
			l += screen.at(x, y);
		}
		return l;
	};
	int at = 0;
	while(at < 30 && !line(at).starts_with(" layer"))
	{
		++at;
	}
	EXPECT_EQ(line(at), " layer     min      max      avg");
	EXPECT_EQ(line(at + 1), " R        -0.5        2     0.25");
	EXPECT_EQ(line(at + 2), " Z           —        —        —");
}

// --- metadata ---

TEST_F(Panes, MetaItemsListPartsAttributesAndLayers)
{
	MetaPane p(m_state, m_c.m_ctx);
	const auto items = p.items(info({"", "diffuse"}));
	// title, path, part, 2 attributes, "layers", 2 layers
	ASSERT_EQ(items.size(), 8u);
	EXPECT_EQ(items[0].m_name, "a.exr");
	EXPECT_EQ(items[1].m_value, "/shots/a.exr");
	EXPECT_EQ(items[3].m_name, "compression");
	EXPECT_EQ(items[7].m_name, "diffuse");
	EXPECT_TRUE(p.items(nullptr).empty());
}

TEST_F(Panes, MetaCursorClampsAndGg)
{
	MetaPane p(m_state, m_c.m_ctx);
	const auto i = info({""});
	const int n = static_cast<int>(p.items(i).size());
	for(int k = 0; k < n + 2; ++k)
	{
		EXPECT_TRUE(p.event(key("j"), i));
	}
	EXPECT_EQ(p.cursor(), n - 1);
	EXPECT_TRUE(p.event(key("g"), i));
	EXPECT_TRUE(p.event(key("g"), i));
	EXPECT_EQ(p.cursor(), 0);
	EXPECT_TRUE(p.event(key("k"), i));
	EXPECT_EQ(p.cursor(), 0);
	EXPECT_TRUE(p.event(Event::CtrlD, i));
	EXPECT_GT(p.cursor(), 0);
}

TEST_F(Panes, MetaVisualSelectionCopiesLines)
{
	MuteCout mute;
	MetaPane p(m_state, m_c.m_ctx);
	const auto i = info({""});
	EXPECT_TRUE(p.event(key("j"), i));
	EXPECT_TRUE(p.event(key("j"), i));
	EXPECT_TRUE(p.event(key("j"), i)); // the "compression" attribute
	EXPECT_TRUE(p.event(key("y"), i));
	EXPECT_EQ(m_c.m_ctx.m_message, "copied: compression: zip");
	EXPECT_TRUE(p.event(key("v"), i));
	EXPECT_TRUE(p.event(key("j"), i));
	EXPECT_TRUE(p.event(key("Y"), i));
	EXPECT_EQ(m_c.m_ctx.m_message, "copied 2 lines");
}

TEST_F(Panes, MetaPassesOnViewerKeysAndClosesOnItsNumber)
{
	MetaPane p(m_state, m_c.m_ctx);
	p.setOpen(true);
	m_state.m_focus = Focus::META;
	EXPECT_FALSE(p.event(key("e"), nullptr));
	EXPECT_TRUE(p.event(key("2"), nullptr));
	EXPECT_FALSE(p.isOpen());
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
	p.scroll(-5); // never above the top
	p.scroll(3);
}

namespace
{

// A 160-column terminal (a 48-cell side pane) while it lives; ctest's output
// is a pipe, so FTXUI uses the fallback size.
struct WideTerminal
{
	WideTerminal()
	{
		ftxui::Terminal::SetFallbackSize({160, 40});
	}
	~WideTerminal()
	{
		ftxui::Terminal::SetFallbackSize({80, 24});
	}
};

// A header whose names and values do not fit the pane.
ImageInfoPtr longInfo(const std::string& path)
{
	auto i = std::make_shared<ImageInfo>();
	PartInfo part;
	part.m_displayWindow = {0, 0, 7, 3};
	part.m_dataWindow = part.m_displayWindow;
	part.m_attributes = {{"screenWindowCenter", "v2f", "[0, 0]"},
	                     {"shaders.camera.overscanLeft", "int", "0"},
	                     {"shaders.camera.overscanRight", "int", "1"},
	                     {"freaki.scenePath", "string", path},
	                     {"tiles", "tiledesc", "32x32 one level"}};
	i->m_parts.push_back(part);
	LayerInfo l;
	l.m_channels = {"R"};
	i->m_layers.push_back(l);
	return i;
}

// The pane drawn as many times as `draws` (its scroll uses the last layout),
// one string per screen row, trailing spaces off.
std::vector<std::string> drawMeta(MetaPane& p,
                                  const ImageInfoPtr& info,
                                  int height = 30)
{
	ftxui::Screen screen(ViewerState{}.sidePanelWidth(), height);
	ftxui::Render(screen, p.render(info));
	ftxui::Render(screen, p.render(info)); // with m_box from the first
	std::vector<std::string> rows;
	for(int y = 0; y < height; ++y)
	{
		std::string row;
		for(int x = 0; x < screen.dimx(); ++x)
		{
			const std::string& c = screen.PixelAt(x, y).character;
			row += c.empty() ? " " : c;
		}
		while(!row.empty() && row.back() == ' ')
		{
			row.pop_back();
		}
		rows.push_back(row);
	}
	return rows;
}

const std::string* rowStarting(const std::vector<std::string>& rows,
                               const std::string& prefix)
{
	for(const auto& r : rows)
	{
		if(r.starts_with(prefix))
		{
			return &r;
		}
	}
	return nullptr;
}

std::string longPath()
{
	std::string path = "/job/rendering/people/etitas/devboxes/g";
	while(path.size() < 150)
	{
		path += "/deeper";
	}
	return path + "/render.ass";
}

} // namespace

TEST_F(Panes, MetaFilterKeepsMatchesUnderTheirHeading)
{
	MetaPane p(m_state, m_c.m_ctx);
	const auto i = info({"", "diffuse"});
	EXPECT_TRUE(p.event(key("/"), i));
	EXPECT_TRUE(p.typing());
	for(const char* c : {"Z", "i", "p"}) // values: a substring, any case
	{
		EXPECT_TRUE(p.event(key(c), i));
	}
	EXPECT_TRUE(p.event(key("q"), i)); // typed, not "back"
	EXPECT_TRUE(p.event(Event::Backspace, i));
	auto rows = p.shown(i);
	ASSERT_EQ(rows.size(), 3u); // the title, part 0, compression
	EXPECT_EQ(rows[2].m_name, "compression");
	EXPECT_EQ(p.cursor(), 1); // the first row past the title
	EXPECT_TRUE(p.event(Event::Return, i));
	EXPECT_FALSE(p.typing());
	EXPECT_EQ(p.filter(), "Zip"); // kept

	// Names match fuzzily: "dfs" finds the diffuse layer.
	EXPECT_TRUE(p.event(key("/"), i));
	for(const char* c : {"d", "f", "s"})
	{
		EXPECT_TRUE(p.event(key(c), i));
	}
	rows = p.shown(i);
	ASSERT_EQ(rows.size(), 3u); // the title, "layers", diffuse
	EXPECT_EQ(rows[2].m_name, "diffuse");
	EXPECT_TRUE(p.event(Event::Escape, i)); // clears
	EXPECT_EQ(p.shown(i).size(), p.items(i).size());
}

TEST_F(Panes, MetaClickMovesTheCursorAndShiftClickSelects)
{
	MuteCout mute;
	MetaPane p(m_state, m_c.m_ctx);
	const auto i = info({""});
	const auto rows = drawMeta(p, i);
	int y = -1;
	for(int r = 0; r < static_cast<int>(rows.size()); ++r)
	{
		if(rows[r].starts_with("  compression"))
		{
			y = r;
		}
	}
	ASSERT_GE(y, 0);
	p.click(y, false);
	EXPECT_EQ(m_state.m_focus, Focus::META);
	EXPECT_EQ(p.cursor(), 3);
	p.click(y + 1, true); // owner, Shift / Ctrl: the two
	EXPECT_EQ(p.cursor(), 4);
	EXPECT_TRUE(p.event(key("y"), i));
	EXPECT_EQ(m_c.m_ctx.m_message, "copied 2 lines");

	// A drag from compression down past the last row: all of them.
	p.click(y, false);
	p.dragTo(y + 1);
	p.dragTo(100);
	EXPECT_EQ(p.cursor(), static_cast<int>(p.items(i).size()) - 1);
	EXPECT_TRUE(p.event(key("y"), i));
	EXPECT_EQ(m_c.m_ctx.m_message,
	          "copied " + std::to_string(p.items(i).size() - 3) + " lines");
}

TEST_F(Panes, MetaNamesAndValuesKeepTheirColumns)
{
	WideTerminal wide;
	MetaPane p(m_state, m_c.m_ctx);
	const auto rows = drawMeta(p, longInfo(longPath()));
	// A name that fits keeps a gap before its value.
	const std::string* centre = rowStarting(rows, "  screenWindowCenter ");
	ASSERT_NE(centre, nullptr);
	EXPECT_TRUE(centre->ends_with(" [0, 0]")) << *centre;
	// A long value no longer squeezes its name; it is cut at the end.
	const std::string* path = rowStarting(rows, "  freaki.scenePath ");
	ASSERT_NE(path, nullptr);
	EXPECT_TRUE(path->ends_with("…")) << *path;
	// Long names are cut in the middle, so they stay apart.
	int overscan = 0;
	for(const auto& r : rows)
	{
		overscan += r.starts_with("  shaders.") && r.find("…") != r.npos;
	}
	EXPECT_EQ(overscan, 2);
	for(const auto& r : rows)
	{
		EXPECT_LE(ftxui::string_width(r), ViewerState{}.sidePanelWidth()) << r;
	}
}

TEST_F(Panes, MetaCursorRowShowsTheWholeValue)
{
	WideTerminal wide;
	MetaPane p(m_state, m_c.m_ctx);
	p.setOpen(true);
	m_state.m_focus = Focus::META;
	const std::string path = longPath();
	const auto i = longInfo(path);
	// title, path, part, then the attributes: freaki.scenePath is the 4th.
	for(int k = 0; k < 6; ++k)
	{
		EXPECT_TRUE(p.event(key("j"), i));
	}
	const auto rows = drawMeta(p, i);
	auto at = std::ranges::find_if(rows,
	                               [](const std::string& r)
	                               { return r.starts_with("  freaki."); });
	ASSERT_NE(at, rows.end());
	// The value runs on under its column until it is all there.
	const size_t col = at->find("/job");
	ASSERT_NE(col, std::string::npos);
	// Continuation lines: blank up to the value column.
	std::string value = at->substr(col);
	for(auto r = at + 1;
	    r != rows.end() && r->size() > col && r->find_first_not_of(' ') == col;
	    ++r)
	{
		value += r->substr(col);
	}
	EXPECT_EQ(value, path);
	EXPECT_EQ(rowStarting(rows, "  tiles")->find("…"), std::string::npos);
}

TEST_F(Panes, MetaScrollsToShowTheWholeCursorRow)
{
	WideTerminal wide;
	MetaPane p(m_state, m_c.m_ctx);
	p.setOpen(true);
	m_state.m_focus = Focus::META;
	const auto i = longInfo(longPath());
	for(int k = 0; k < 6; ++k)
	{
		EXPECT_TRUE(p.event(key("j"), i));
	}
	// Ten rows: the title, the footer and eight lines for the items. The
	// path's last line must be on screen.
	const auto rows = drawMeta(p, i, 10);
	EXPECT_TRUE(std::ranges::any_of(rows,
	                                [](const std::string& r)
	                                { return r.ends_with("/render.ass"); }));
}

// --- colour ---

TEST_F(Panes, ColourWithoutAConfigOnlyOffersConfigs)
{
	ColourPane p(m_state, m_c.m_ctx);
	int current = -1;
	EXPECT_TRUE(p.values(ColourPane::Row::DISPLAY, "/shots/a.exr", "a", current)
	                .empty());
	const auto configs =
	    p.values(ColourPane::Row::CONFIG, "/shots/a.exr", "a", current);
	ASSERT_FALSE(configs.empty());
	EXPECT_EQ(configs[current], "none (sRGB)");
}

TEST_F(Panes, ColourStepsAndPicksViewsAndInputs)
{
	ASSERT_TRUE(m_c.m_colour.useConfig(ColourManager::STUDIO))
	    << m_c.m_colour.error();
	ColourPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_EQ(m_state.m_focus, Focus::COLOUR);
	const fs::path path = "/shots/a.exr";
	auto press = [&](const Event& e) { return p.event(e, path, "a"); };

	// h/l on the view row steps through the display's views.
	const auto views = m_c.m_colour.views();
	ASSERT_GT(views.size(), 1u);
	EXPECT_TRUE(press(key("j")));
	EXPECT_TRUE(press(key("j")));
	ASSERT_EQ(p.cursor(), static_cast<int>(ColourPane::Row::VIEW));
	const std::string before = m_c.m_colour.view();
	EXPECT_TRUE(press(key("l")));
	EXPECT_NE(m_c.m_colour.view(), before);
	EXPECT_TRUE(press(key("h")));
	EXPECT_EQ(m_c.m_colour.view(), before);

	// Enter lists them; typing filters; Enter picks the match.
	const std::string target =
	    views.back() == before ? views.front() : views.back();
	EXPECT_TRUE(press(Event::Return));
	EXPECT_TRUE(p.picking());
	for(char c : target)
	{
		EXPECT_TRUE(press(key(std::string(1, c))));
	}
	EXPECT_TRUE(press(Event::Return));
	EXPECT_FALSE(p.picking());
	EXPECT_EQ(m_c.m_colour.view(), target);

	// The input row: a colour space overrides the file rules; the first
	// entry goes back to them.
	EXPECT_TRUE(press(key("j")));
	EXPECT_TRUE(press(key("j")));
	ASSERT_EQ(p.cursor(), static_cast<int>(ColourPane::Row::INPUT));
	p.choose(ColourPane::Row::INPUT, 1, path, "a");
	EXPECT_TRUE(m_c.m_colour.overridden("a"));
	EXPECT_EQ(m_c.m_colour.inputFor(path, "a"),
	          m_c.m_colour.colourSpaces().front());
	p.choose(ColourPane::Row::INPUT, 0, path, "a");
	EXPECT_FALSE(m_c.m_colour.overridden("a"));

	// Esc leaves a list without picking; other keys reach the viewer; its
	// number closes it.
	EXPECT_TRUE(press(Event::Return));
	EXPECT_TRUE(press(Event::Escape));
	EXPECT_FALSE(p.picking());
	EXPECT_FALSE(press(key("e")));
	EXPECT_TRUE(press(key("6")));
	EXPECT_FALSE(p.isOpen());
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
}

// --- layers ---

TEST_F(Panes, LayersCursorShowsTheLayerItIsOn)
{
	LayersPane p(m_state);
	const auto i = info({"", "diffuse", "specular"});
	m_state.m_layerLabel = "diffuse";
	p.show(i);
	EXPECT_TRUE(p.isOpen());
	EXPECT_EQ(m_state.m_focus, Focus::LAYERS);
	EXPECT_EQ(p.cursor(), 1); // starts on the layer shown
	EXPECT_TRUE(p.event(key("j"), i));
	EXPECT_EQ(m_state.m_layerLabel, "specular");
	EXPECT_TRUE(p.event(key("j"), i)); // clamped at the last
	EXPECT_EQ(p.cursor(), 2);
	EXPECT_TRUE(p.event(key("g"), i));
	EXPECT_EQ(m_state.m_layerLabel, i->m_layers[0].label());
	EXPECT_TRUE(p.event(key("k"), i));
	EXPECT_EQ(p.cursor(), 0);
	EXPECT_TRUE(p.event(key("G"), i));
	EXPECT_EQ(m_state.m_layerLabel, "specular");
}

TEST_F(Panes, LayersClickPicksARowAndPassesOnOtherKeys)
{
	LayersPane p(m_state);
	const auto i = info({"", "diffuse"});
	p.click(2, i); // row 2 = the second layer (row 0 is the title)
	EXPECT_EQ(m_state.m_layerLabel, "diffuse");
	EXPECT_EQ(m_state.m_focus, Focus::LAYERS);
	p.click(9, i); // below the list: focus only
	EXPECT_EQ(m_state.m_layerLabel, "diffuse");
	EXPECT_FALSE(p.event(key("e"), i));
	EXPECT_TRUE(p.event(key("5"), i));
	EXPECT_FALSE(p.isOpen());
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
}

// --- files ---

TEST_F(Panes, FilesCursorRunsFromGlobalToTheLastFile)
{
	FilesPane p(m_state, m_c.m_ctx);
	m_state.m_current = 1;
	p.show();
	EXPECT_EQ(p.cursor(), 1);
	EXPECT_EQ(m_state.m_focus, Focus::FILES);
	for(int i = 0; i < 4; ++i)
	{
		EXPECT_TRUE(p.event(key("k")));
	}
	EXPECT_EQ(p.cursor(), -1); // the global row
	for(int i = 0; i < 5; ++i)
	{
		EXPECT_TRUE(p.event(key("j")));
	}
	EXPECT_EQ(p.cursor(), 2);
	EXPECT_TRUE(p.event(Event::Return));
	EXPECT_EQ(m_state.m_current, 2);
	EXPECT_FALSE(p.event(key("y"))); // not a files key
}

TEST_F(Panes, FilesLongNamesKeepTheirNumbersAndStayApart)
{
	// Names wider than the column used to squeeze the whole row, numbers
	// included (they came and went from row to row).
	m_state.m_sources.clear();
	for(const char* end : {"Batch.png", "Interactive.png", "Hydra.png"})
	{
		m_state.m_sources.push_back({file(
		    std::string("test_BasisCurve_ribbon_varying_normals_") + end)});
	}
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	const int w = m_state.leftPanelWidth();
	ftxui::Screen screen(w, 8);
	ftxui::Render(screen, p.render());
	std::vector<std::string> rows;
	for(int y = 2; y < 5; ++y) // below the title and the global row
	{
		std::string row;
		for(int x = 0; x < w; ++x)
		{
			const std::string& c = screen.PixelAt(x, y).character;
			row += c.empty() ? " " : c;
		}
		rows.push_back(row);
	}
	for(int i = 0; i < 3; ++i)
	{
		EXPECT_TRUE(rows[i].starts_with("  " + std::to_string(i + 1) + " "))
		    << rows[i];
		EXPECT_NE(rows[i].find("…"), std::string::npos) << rows[i];
		EXPECT_LE(ftxui::string_width(rows[i]), w) << rows[i];
	}
	// Cut in the middle: the ends that tell them apart are still there.
	EXPECT_NE(rows[2].find("Hydra.png"), std::string::npos) << rows[2];
	EXPECT_NE(rows[0], rows[1]);
	EXPECT_NE(rows[1], rows[2]);
}

TEST_F(Panes, FilesReorderKeepsShowingTheSameImage)
{
	FilesPane p(m_state, m_c.m_ctx);
	p.show(); // on a.exr, shown
	EXPECT_TRUE(p.event(key("J")));
	EXPECT_EQ(m_state.m_sources[1].m_entry.m_name, "a.exr");
	EXPECT_EQ(m_state.m_current, 1);
	EXPECT_EQ(p.cursor(), 1);
	EXPECT_TRUE(p.event(key("K")));
	EXPECT_EQ(m_state.m_sources[0].m_entry.m_name, "a.exr");
	EXPECT_EQ(m_state.m_current, 0);
}

TEST_F(Panes, FilesDropKeepsTheLastImage)
{
	FilesPane p(m_state, m_c.m_ctx);
	m_state.m_current = 2;
	p.show();
	EXPECT_TRUE(p.event(key("d")));
	ASSERT_EQ(m_state.m_sources.size(), 2u);
	EXPECT_EQ(m_state.m_current, 1);
	EXPECT_TRUE(p.event(key("d")));
	EXPECT_TRUE(p.event(key("d")));
	EXPECT_EQ(m_state.m_sources.size(), 1u);
	EXPECT_EQ(m_c.m_ctx.m_message, "the last image stays");
}

TEST_F(Panes, FilesExpandsASequenceAndFoldsItBack)
{
	m_state.m_sources = {{file("a.exr")}, {sequence("shot", 4)}};
	m_state.m_current = 1;
	m_state.m_frame = 2;
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("e")));
	ASSERT_EQ(m_state.m_sources.size(), 5u);
	EXPECT_EQ(m_state.m_current, 3); // the frame that was on screen
	EXPECT_EQ(p.cursor(), 3);
	EXPECT_FALSE(m_state.m_sources[2].m_entry.m_expandedFrom.empty());
	EXPECT_TRUE(p.event(key("e"))); // on a frame: fold
	ASSERT_EQ(m_state.m_sources.size(), 2u);
	EXPECT_EQ(m_state.m_sources[1].m_entry.m_kind, Entry::Kind::SEQUENCE);
	EXPECT_EQ(m_state.m_current, 1);
	EXPECT_EQ(m_state.m_frame, 2);
}

TEST_F(Panes, FilesSequenceTooLongToExpand)
{
	m_state.m_sources = {{sequence("long", FilesPane::MAX_EXPAND_FRAMES + 1)}};
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("e")));
	EXPECT_EQ(m_state.m_sources.size(), 1u);
	EXPECT_EQ(m_c.m_ctx.m_message, "501 frames: expands up to 500");
}

TEST_F(Panes, FilesIntoAnnotationsAndBack)
{
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("k"))); // global
	EXPECT_TRUE(p.event(key("l")));
	ASSERT_TRUE(p.annotations().isOpen());
	EXPECT_EQ(p.annotations().group(), 0);
	EXPECT_TRUE(p.event(key("h")));
	EXPECT_FALSE(p.annotations().isOpen());
	EXPECT_EQ(p.cursor(), -1);

	EXPECT_TRUE(p.event(key("j")));
	EXPECT_TRUE(p.event(key("j"))); // b.exr
	EXPECT_TRUE(p.event(key("l")));
	EXPECT_EQ(p.annotations().group(), 1);
	EXPECT_EQ(m_state.m_current, 1);
	EXPECT_TRUE(p.event(key("h")));
	EXPECT_EQ(p.cursor(), 1);
}

TEST_F(Panes, AnnotationLineIsTypedLiveAndCommitted)
{
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("l"))); // a.exr's set, cursor on the first slot
	EXPECT_TRUE(p.event(key("a")));
	ASSERT_TRUE(p.annotations().editing());
	// Looked up each time: the sets move as they are used (most recent first).
	const std::string srcKey = sourceKey(m_state.m_sources[0]);
	auto bottomLeft = [&]
	{ return m_c.m_ann.findSource(srcKey)->lines(Slot::BL); };
	for(const char* c : {"h", "i"})
	{
		EXPECT_TRUE(p.event(key(c)));
	}
	EXPECT_EQ(bottomLeft(), (std::vector<std::string>{"hi"})); // live
	EXPECT_TRUE(p.event(key("3"))); // typed, not "close the pane"
	EXPECT_TRUE(p.isOpen());
	EXPECT_TRUE(p.event(Event::Return));
	EXPECT_FALSE(p.annotations().editing());
	EXPECT_EQ(bottomLeft(), (std::vector<std::string>{"hi3"}));
	ASSERT_FALSE(m_c.m_ann.history().empty());
	EXPECT_EQ(m_c.m_ann.history().front(), "hi3");

	// Esc on a new line drops it.
	EXPECT_TRUE(p.event(key("a")));
	EXPECT_TRUE(p.event(key("x")));
	EXPECT_TRUE(p.event(Event::Escape));
	EXPECT_EQ(bottomLeft(), (std::vector<std::string>{"hi3"}));
}

TEST_F(Panes, FilesDTwiceClearsEveryAnnotation)
{
	m_c.m_ann.global().lines(Slot::TL).push_back("g");
	m_c.m_ann.source(sourceKey(m_state.m_sources[1]))
	    .lines(Slot::BR)
	    .push_back("b");
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("D")));
	EXPECT_EQ(m_c.m_ctx.m_message, "D again: clear all annotations");
	EXPECT_FALSE(m_c.m_ann.global().empty());
	EXPECT_TRUE(p.event(key("D")));
	EXPECT_TRUE(m_c.m_ann.global().empty());
	EXPECT_TRUE(m_state.sourceAnnotations(m_c.m_ann, 1)->empty());
}

TEST_F(Panes, FilesClosesOnItsNumberAndHandsBackFocus)
{
	FilesPane p(m_state, m_c.m_ctx);
	p.show();
	EXPECT_TRUE(p.event(key("1")));
	EXPECT_EQ(m_state.m_focus, Focus::IMAGE);
	EXPECT_TRUE(p.isOpen());
	p.show();
	EXPECT_TRUE(p.event(key("3")));
	EXPECT_FALSE(p.isOpen());
}

TEST_F(Panes, MetaOnADiffListsWhatDiffersAndDShowsBothWhole)
{
	using D = MetaPane::Item::Diff;
	const fs::path dir = freshDir("meta-diff");
	const fs::path a = writeExr(dir / "a.exr", {"R", "G", "B"}, 8, 4);
	const fs::path b = writeExr(dir / "b.exr", {"R", "G", "B"}, 16, 4);
	Source diff{entryForPath(b)};
	diff.m_diffA = entryForPath(a);
	m_state.m_sources = {diff};
	m_state.m_current = 0;
	ImageInfoPtr ib;
	ASSERT_TRUE(
	    waitFor([&] { return (ib = m_c.m_svc.info(b)) && m_c.m_svc.info(a); }));
	MetaPane p(m_state, m_c.m_ctx);
	auto find = [](const std::vector<MetaPane::Item>& items,
	               const std::string& name) -> const MetaPane::Item*
	{
		const auto it = std::ranges::find(items, name, &MetaPane::Item::m_name);
		return it == items.end() ? nullptr : &*it;
	};

	auto items = p.items(ib);
	EXPECT_EQ(items[1].m_name, "2 differences"); // data + display window
	const auto* dw = find(items, "dataWindow");
	ASSERT_TRUE(dw);
	EXPECT_EQ(dw->m_diff, D::CHANGED);
	EXPECT_NE(dw->m_value, dw->m_valueB);
	EXPECT_FALSE(find(items, "compression")); // the same: not listed

	EXPECT_TRUE(p.event(key("d"), ib)); // both headers whole
	items = p.items(ib);
	ASSERT_TRUE(find(items, "compression"));
	EXPECT_EQ(find(items, "compression")->m_diff, D::SAME);
	EXPECT_EQ(items[1].m_name, "2 differences");

	// The same file both sides.
	m_state.m_sources[0].m_diffA = entryForPath(b);
	EXPECT_EQ(p.items(ib)[1].m_name, "headers identical");
}
