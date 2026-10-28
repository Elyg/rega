#include "TestContext.h"
#include "app/Sheet.h"
#include "app/Viewer.h"
#include "image/Sequence.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

using namespace rega;
using namespace regatest;
using ftxui::Event;

namespace
{

// A multi-layer image and a 3-frame sequence (8x4 px each), viewed on an
// 80x24 half-block screen.
struct ViewerTest : ::testing::Test
{
	Context m_c;
	fs::path m_dir = freshDir("viewer");
	fs::path m_layers = writeExr(m_dir / "layers.exr",
	                             {"R",
	                              "G",
	                              "B",
	                              "diffuse.R",
	                              "diffuse.G",
	                              "diffuse.B",
	                              "spec.R",
	                              "spec.G",
	                              "spec.B"});
	int m_closed = 0;
	Viewer m_v{m_c.m_ctx, [this] { ++m_closed; }};

	ViewerTest()
	{
		for(int i = 1; i <= 3; ++i)
		{
			writeExr(m_dir / ("seq.000" + std::to_string(i) + ".exr"),
			         {"R", "G", "B"});
		}
	}
	Entry layersEntry() const
	{
		return entryForPath(m_layers);
	}
	Entry sequenceEntry() const
	{
		return entryForPath(m_dir / "seq.0001.exr");
	}
	void press(const Event& e)
	{
		EXPECT_TRUE(m_v.event(e));
	}
	const ViewerState& state() const
	{
		return m_v.state();
	}
	ImageInfoPtr info()
	{
		ImageInfoPtr i;
		waitFor(
		    [&]
		    {
			    return (i = m_c.m_svc.info(state().currentFramePath())) !=
			           nullptr;
		    });
		return i;
	}
	// Draw a tiled sheet until every tile's decode has landed (tiles of
	// layers ask for each layer, not the selected one draw() waits for).
	void drawSheet()
	{
		ASSERT_TRUE(info());
		ftxui::Screen screen(80, 24);
		ftxui::Render(screen, m_v.render());
		ASSERT_TRUE(waitFor([&] { return m_c.m_svc.pendingJobs() == 0; }));
		ftxui::Render(screen, m_v.render());
	}
	// Draw until the image itself is on screen (decoded and laid out).
	void draw()
	{
		ASSERT_TRUE(info());
		ftxui::Screen screen(80, 24);
		ftxui::Render(screen, m_v.render());
		const fs::path path = state().currentFramePath();
		ASSERT_TRUE(waitFor(
		    [&] { return m_c.m_svc.hasLayer(path, state().m_layerLabel, 1); }));
		ftxui::Render(screen, m_v.render());
	}
};

} // namespace

TEST_F(ViewerTest, OpenStartsFromTheFirstFrameFittedAndUntiled)
{
	m_v.open({sequenceEntry()});
	draw();
	press(key("."));
	press(key("+"));
	press(key("t"));
	EXPECT_EQ(state().m_frame, 1);
	EXPECT_TRUE(m_v.tiled());
	EXPECT_FALSE(state().m_view.m_fit);

	m_v.open({sequenceEntry(), layersEntry()});
	EXPECT_EQ(state().m_sources.size(), 2u);
	EXPECT_EQ(state().m_current, 0);
	EXPECT_EQ(state().m_frame, 0);
	EXPECT_FALSE(m_v.tiled());
	EXPECT_TRUE(state().m_view.m_fit);
	EXPECT_FALSE(state().m_picked);

	m_v.open({layersEntry()}, true);
	EXPECT_TRUE(m_v.tiled());
}

TEST_F(ViewerTest, TileSheetZoomsPansAndOpensWhereItWas)
{
	m_v.open({layersEntry()}, true); // a tile per layer: rgba, diffuse, spec
	drawSheet();
	ASSERT_EQ(m_v.sheet().m_count, 3);
	EXPECT_EQ(m_v.tilesShown(), 3);
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
	press(key("+"));
	EXPECT_GT(m_v.sheetView().m_zoom, 1.0);
	press(key("f"));
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
	// The wheel zooms the sheet too (it did nothing on tiles before).
	ftxui::Mouse wheel;
	wheel.button = ftxui::Mouse::WheelUp;
	wheel.motion = ftxui::Mouse::Pressed;
	wheel.x = 40;
	wheel.y = 12;
	press(Event::Mouse("", wheel));
	EXPECT_GT(m_v.sheetView().m_zoom, 1.0);
	press(key("f"));

	// Zoomed in on the selected tile, panning selects what is in the middle.
	const std::string first = state().m_layerLabel;
	for(int i = 0; i < 8; ++i)
	{
		press(key("+"));
	}
	drawSheet();
	EXPECT_EQ(state().m_layerLabel, first); // keys zoom about the selection
	EXPECT_LT(m_v.tilesShown(), 3);         // the others are off screen
	for(int i = 0; i < 20 && state().m_layerLabel == first; ++i)
	{
		press(key("L"));
	}
	EXPECT_NE(state().m_layerLabel, first);

	// Enter opens it at the zoom it had on the sheet.
	drawSheet();
	press(Event::Return);
	EXPECT_FALSE(m_v.tiled());
	EXPECT_FALSE(state().m_view.m_fit);
	EXPECT_GT(state().m_view.m_zoom, 0.0);

	// Back to tiles: a fresh, fitted sheet.
	press(key("t"));
	drawSheet();
	EXPECT_EQ(m_v.sheetView().m_zoom, 1.0);
}

TEST_F(ViewerTest, FoldingAnExpandedSequenceLaysTheSheetOutAfresh)
{
	// The sequence and an image, tiled beside the files pane.
	m_v.open({sequenceEntry(), layersEntry()}, true);
	press(key("3"));
	draw();
	const SheetLayout fresh = m_v.sheet();
	ASSERT_EQ(fresh.m_count, 2);
	press(key("e")); // the sequence's 3 frames: 4 tiles
	draw();
	EXPECT_EQ(m_v.sheet().m_count, 4);
	press(key("e")); // fold them back
	draw();
	EXPECT_EQ(m_v.sheet(), fresh);
	EXPECT_EQ(m_v.tilesShown(), 2);
}

TEST_F(ViewerTest, LayersCycleBothWaysAndWrap)
{
	m_v.open({layersEntry()});
	draw();
	const auto i = info();
	ASSERT_EQ(i->m_layers.size(), 3u);
	auto shown = [&] { return i->findLayer(state().m_layerLabel); };
	EXPECT_EQ(shown(), 0);
	press(key("]"));
	EXPECT_EQ(shown(), 1);
	press(key("["));
	press(key("["));
	EXPECT_EQ(shown(), 2);
	press(key("]"));
	EXPECT_EQ(shown(), 0);
}

TEST_F(ViewerTest, FrameSteppingWrapsRound)
{
	m_v.open({sequenceEntry()});
	EXPECT_EQ(state().frameCount(), 3);
	press(key(","));
	EXPECT_EQ(state().m_frame, 2);
	press(key("."));
	EXPECT_EQ(state().m_frame, 0);
	press(key(">"));
	EXPECT_EQ(state().m_frame, 2);
	press(key("<"));
	EXPECT_EQ(state().m_frame, 0);

	m_v.open({layersEntry()}); // one frame: stays
	press(key("."));
	EXPECT_EQ(state().m_frame, 0);
	press(key(" "));
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
	EXPECT_FALSE(m_v.playing());
}

TEST_F(ViewerTest, MetadataFilterDrawsAndEndsWhenFocusLeaves)
{
	m_v.open({layersEntry()});
	draw();
	press(key("2")); // open and focus the metadata
	press(key("/"));
	EXPECT_TRUE(m_v.typing());
	press(key("z"));
	// The status line is the viewer's, not the annotation input (none is
	// being edited).
	ftxui::Screen screen(80, 24);
	ftxui::Render(screen, m_v.render());

	// A click on the image (the wheel too) gets through the filter, and
	// takes the focus and the typing with it.
	ftxui::Mouse m{};
	m.button = ftxui::Mouse::Left;
	m.motion = ftxui::Mouse::Pressed;
	m.x = 5;
	m.y = 10;
	(void)m_v.event(Event::Mouse("", m));
	m.motion = ftxui::Mouse::Released;
	(void)m_v.event(Event::Mouse("", m));
	EXPECT_NE(state().m_focus, Focus::META);
	EXPECT_FALSE(m_v.typing());
	ftxui::Render(screen, m_v.render());
}

TEST_F(ViewerTest, ColonGoesToAFrameNumberOrTheNearest)
{
	for(int n : {1001, 1002, 1005})
	{
		writeExr(m_dir / ("gap." + std::to_string(n) + ".exr"), {"R"});
	}
	m_v.open({entryForPath(m_dir / "gap.1001.exr")});
	auto go = [&](const std::string& digits)
	{
		press(key(":"));
		EXPECT_TRUE(m_v.typing());
		for(char c : digits)
		{
			press(key(std::string(1, c)));
		}
		press(Event::Return);
		EXPECT_FALSE(m_v.typing());
	};
	go("1005");
	EXPECT_EQ(state().m_frame, 2);
	EXPECT_EQ(m_c.m_ctx.m_message, "");
	go("1004"); // a gap: the nearest
	EXPECT_EQ(state().m_frame, 2);
	EXPECT_EQ(m_c.m_ctx.m_message, "no frame 1004: showing 1005");
	go("1003"); // a tie: the earlier
	EXPECT_EQ(state().m_frame, 1);
	go("9999"); // past the end: the last
	EXPECT_EQ(state().m_frame, 2);
	go("1"); // before the start: the first
	EXPECT_EQ(state().m_frame, 0);

	// Letters are not typed (nor run as commands); Esc leaves it as it was.
	press(key(":"));
	press(key("q"));
	press(key("1"));
	press(Event::Escape);
	EXPECT_FALSE(m_v.typing());
	EXPECT_EQ(state().m_frame, 0);
	EXPECT_EQ(m_closed, 0);
	go(""); // nothing typed: nothing happens
	EXPECT_EQ(state().m_frame, 0);

	m_v.open({layersEntry()});
	press(key(":"));
	EXPECT_FALSE(m_v.typing());
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
}

TEST_F(ViewerTest, PLiftsThePlaybackResolutionCap)
{
	m_v.open({sequenceEntry()});
	ASSERT_EQ(m_c.m_caps.m_transfer, Transfer::DIRECT); // as over ssh
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET);
	press(key(" "));
	ASSERT_TRUE(m_v.playing());
	// Half for playing, half again for the inline transfer.
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET / 4);
	press(key("P"));
	EXPECT_FALSE(m_v.playbackCapped());
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET);
	EXPECT_EQ(m_c.m_ctx.m_message, "playback: full res");
	press(key("P"));
	EXPECT_EQ(m_v.pixelCap(), TOTAL_PIXEL_BUDGET / 4);
	EXPECT_EQ(m_c.m_ctx.m_message, "playback: capped at 1 Mpx (full 4)");
	press(key(" "));
}

TEST_F(ViewerTest, ZoomStepsBy125PercentAndPanMovesByCells)
{
	m_v.open({layersEntry()});
	press(key("+")); // not drawn yet: fit has no zoom to start from
	EXPECT_TRUE(state().m_view.m_fit);
	draw();
	press(key("+"));
	EXPECT_FALSE(state().m_view.m_fit);
	const double z1 = state().m_view.m_zoom;
	press(key("+"));
	EXPECT_NEAR(state().m_view.m_zoom, z1 * 1.25, 1e-9);
	press(key("-"));
	EXPECT_NEAR(state().m_view.m_zoom, z1, 1e-9);

	press(key("z")); // 1:1
	EXPECT_EQ(state().m_view.m_zoom, 1.0);
	const double cx = state().m_view.m_centerX;
	const double cy = state().m_view.m_centerY;
	// Half-block cells are 1x2 terminal px; at 1:1 that many image px.
	press(key("l"));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 4, 1e-9);
	press(key("j"));
	EXPECT_NEAR(state().m_view.m_centerY, cy + 4, 1e-9);
	press(key("H"));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 4 - 16, 1e-9);

	press(key("f"));
	EXPECT_TRUE(state().m_view.m_fit);
}

TEST_F(ViewerTest, RightOrMiddleDragPansLikeHjkl)
{
	m_v.open({layersEntry()});
	draw();
	press(key("z")); // 1:1: a cell is 1x2 image px
	auto mouse =
	    [&](ftxui::Mouse::Button b, int x, int y, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = b;
		m.motion = motion;
		m.x = x;
		m.y = y;
		press(Event::Mouse("", m));
	};
	const double cx = state().m_view.m_centerX;
	const double cy = state().m_view.m_centerY;
	// The image follows the mouse: dragging right looks further left.
	mouse(ftxui::Mouse::Right, 40, 12, ftxui::Mouse::Pressed);
	mouse(ftxui::Mouse::Right, 44, 13, ftxui::Mouse::Moved);
	mouse(ftxui::Mouse::Right, 44, 13, ftxui::Mouse::Released);
	EXPECT_NEAR(state().m_view.m_centerX, cx - 4, 1e-9);
	EXPECT_NEAR(state().m_view.m_centerY, cy - 2, 1e-9);
	mouse(ftxui::Mouse::Middle, 40, 12, ftxui::Mouse::Pressed);
	mouse(ftxui::Mouse::Middle, 30, 12, ftxui::Mouse::Moved);
	mouse(ftxui::Mouse::Middle, 30, 12, ftxui::Mouse::Released);
	EXPECT_NEAR(state().m_view.m_centerX, cx - 4 + 10, 1e-9);
	// Released: moving leaves the view be.
	ftxui::Mouse move;
	move.button = ftxui::Mouse::None;
	move.motion = ftxui::Mouse::Moved;
	(void)m_v.event(Event::Mouse("", move));
	EXPECT_NEAR(state().m_view.m_centerX, cx + 6, 1e-9);
}

TEST_F(ViewerTest, CtrlClickPicksAPixelUntilReopened)
{
	m_v.open({layersEntry()});
	draw();
	ftxui::Mouse m;
	m.button = ftxui::Mouse::Left;
	m.control = true;
	m.motion = ftxui::Mouse::Pressed;
	m.x = 40;
	m.y = 12;
	(void)m_v.event(Event::Mouse("", m));
	ASSERT_TRUE(waitFor(
	    [&]
	    {
		    // The exact readout needs the full-resolution layer.
		    return m_v.event(Event::Mouse("", m)) && state().m_picked &&
		           state().m_picked->m_exact;
	    }));
	EXPECT_EQ(state().m_picked->m_state, Sample::State::OK);
	EXPECT_NEAR(state().m_picked->m_rgba[0], 0.5f, 1e-6);
	EXPECT_TRUE(m_c.m_ctx.m_message.starts_with("picked ["))
	    << m_c.m_ctx.m_message;
	m_v.open({layersEntry()});
	EXPECT_FALSE(state().m_picked);
}

TEST_F(ViewerTest, OcioViewChangesWhatIsShownAndSGoesRaw)
{
	m_v.open({layersEntry()}); // every channel 0.5
	draw();
	auto pickRed = [&]
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.control = true;
		m.motion = ftxui::Mouse::Pressed;
		m.x = 40;
		m.y = 12;
		EXPECT_TRUE(waitFor(
		    [&]
		    {
			    return m_v.event(Event::Mouse("", m)) && state().m_picked &&
			           state().m_picked->m_exact;
		    }));
		return state().m_picked ? state().m_picked->m_r : -1;
	};
	EXPECT_EQ(pickRed(), 188); // sRGB of 0.5
	ASSERT_TRUE(m_c.m_colour.useConfig(ColourManager::STUDIO))
	    << m_c.m_colour.error();
	const int ocio = pickRed();
	EXPECT_NE(ocio, 188); // the config's view, tone mapped
	EXPECT_GT(ocio, 0);
	press(key("s"));
	EXPECT_EQ(pickRed(), 128); // raw
	press(key("6"));
	EXPECT_EQ(state().m_focus, Focus::COLOUR);
}

TEST_F(ViewerTest, LowerCaseRaisesExposureAndGamma)
{
	m_v.open({layersEntry()});
	press(key("e"));
	EXPECT_FLOAT_EQ(state().m_disp.m_exposure, 0.5f);
	press(key("E"));
	press(key("E"));
	EXPECT_FLOAT_EQ(state().m_disp.m_exposure, -0.5f);
	press(key("y"));
	EXPECT_FLOAT_EQ(state().m_disp.m_gamma, 1.1f);
	press(key("Y"));
	press(key("Y"));
	EXPECT_FLOAT_EQ(state().m_disp.m_gamma, 0.9f);
}

TEST_F(ViewerTest, NextAndPreviousImageWrap)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("n"));
	EXPECT_EQ(state().m_current, 1);
	press(key("n"));
	EXPECT_EQ(state().m_current, 0);
	press(key("p"));
	EXPECT_EQ(state().m_current, 1);
	press(key("N")); // previous too
	EXPECT_EQ(state().m_current, 0);
}

TEST_F(ViewerTest, QUntilesFirstThenCloses)
{
	m_v.open({layersEntry()}, true);
	press(key("q"));
	EXPECT_FALSE(m_v.tiled());
	EXPECT_EQ(m_closed, 0);
	press(key("q"));
	EXPECT_EQ(m_closed, 1);
}

TEST_F(ViewerTest, PaneKeysOpenFocusAndClose)
{
	m_v.open({layersEntry()});
	press(key("m")); // = 2: metadata
	EXPECT_EQ(state().m_focus, Focus::META);
	press(key("m")); // focused: closes
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
	press(key("2"));
	press(key("5"));
	EXPECT_EQ(state().m_focus, Focus::LAYERS);
	press(Event::Tab); // hide all
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
	press(Event::Tab); // and back, with the focus they had
	EXPECT_EQ(state().m_focus, Focus::LAYERS);
	press(key("1"));
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
}

TEST_F(ViewerTest, DraggingTheLeftDividerResizesTheColumn)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("3")); // files
	draw();
	const int before = state().leftPanelWidth();
	auto mouse = [&](int x, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = x;
		m.y = 3;
		press(Event::Mouse("", m));
	};
	mouse(before, ftxui::Mouse::Pressed); // on the separator
	mouse(before + 4, ftxui::Mouse::Moved);
	mouse(before + 4, ftxui::Mouse::Released);
	EXPECT_EQ(state().leftPanelWidth(), before + 4);
	// Narrower than a short name, or over the image: no further.
	mouse(before + 4, ftxui::Mouse::Pressed);
	mouse(1, ftxui::Mouse::Moved);
	EXPECT_EQ(state().leftPanelWidth(), 16);
	mouse(1000, ftxui::Mouse::Moved);
	mouse(1000, ftxui::Mouse::Released);
	EXPECT_LT(state().leftPanelWidth(),
	          ftxui::Terminal::Size().dimx - state().sidePanelWidth());
	// Released: moving the mouse leaves it be.
	const int after = state().leftPanelWidth();
	ftxui::Mouse move;
	move.motion = ftxui::Mouse::Moved;
	move.x = 5;
	move.y = 3;
	(void)m_v.event(Event::Mouse("", move));
	EXPECT_EQ(state().leftPanelWidth(), after);
}

namespace
{

// The screen row and column where `title` is drawn ({-1, -1}: nowhere).
std::pair<int, int> titleAt(Viewer& v, const std::string& title)
{
	ftxui::Screen screen(80, 24);
	ftxui::Render(screen, v.render());
	for(int y = 0; y < screen.dimy(); ++y)
	{
		std::string row;
		std::vector<int> cols; // screen column of each byte of `row`
		for(int x = 0; x < screen.dimx(); ++x)
		{
			const std::string& c = screen.PixelAt(x, y).character;
			row += c.empty() ? " " : c;
			cols.resize(row.size(), x);
		}
		if(const auto at = row.find(title); at != std::string::npos)
		{
			return {y, cols[at]};
		}
	}
	return {-1, -1};
}

} // namespace

TEST_F(ViewerTest, DraggingTheFilesTitleShowsMoreOrFewerRows)
{
	m_v.open({layersEntry(), sequenceEntry()});
	press(key("3"));
	press(key("j"));
	press(key("e")); // the sequence's frames: global + 4 rows
	draw();
	const int y0 = titleAt(m_v, "[3]─Files").first;
	ASSERT_GT(y0, 0);
	auto mouse = [&](int y, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = 4;
		m.y = y;
		press(Event::Mouse("", m));
	};
	// Down two: two rows fewer, the title where the mouse let go.
	mouse(y0, ftxui::Mouse::Pressed);
	mouse(y0 + 2, ftxui::Mouse::Moved);
	mouse(y0 + 2, ftxui::Mouse::Released);
	EXPECT_EQ(titleAt(m_v, "[3]─Files").first, y0 + 2);
	// Up past the list: it shows every row, no more.
	mouse(y0 + 2, ftxui::Mouse::Pressed);
	mouse(1, ftxui::Mouse::Moved);
	mouse(1, ftxui::Mouse::Released);
	EXPECT_EQ(titleAt(m_v, "[3]─Files").first, y0);
}

TEST_F(ViewerTest, DraggingTheRightDividerResizesTheColumn)
{
	m_v.open({layersEntry()});
	press(key("2")); // metadata, on the right
	draw();
	const int w = ftxui::Terminal::Size().dimx;
	const int before = state().sidePanelWidth();
	auto mouse = [&](int x, ftxui::Mouse::Motion motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = x;
		m.y = 3;
		press(Event::Mouse("", m));
	};
	// The separator just left of the column's title.
	const int divider = titleAt(m_v, "[2]─Metadata").second - 2;
	ASSERT_GE(divider, 0);
	mouse(divider, ftxui::Mouse::Pressed);
	mouse(divider - 4, ftxui::Mouse::Moved); // leftwards: wider
	mouse(divider - 4, ftxui::Mouse::Released);
	EXPECT_EQ(state().sidePanelWidth(), before + 4);
	EXPECT_EQ(state().m_leftFrac, 0); // the left one untouched
	// Past the left column and some image: no further.
	mouse(divider - 4, ftxui::Mouse::Pressed);
	mouse(0, ftxui::Mouse::Moved);
	mouse(0, ftxui::Mouse::Released);
	EXPECT_LE(state().sidePanelWidth(), w - state().leftPanelWidth() - 2 - 10);
}

TEST_F(ViewerTest, ColourPaneOpensOnTheLeft)
{
	m_v.open({layersEntry()});
	press(key("6"));
	EXPECT_EQ(state().m_focus, Focus::COLOUR);
	draw();
	const auto [y, x] = titleAt(m_v, "[6]─Colour");
	ASSERT_GE(y, 0);
	EXPECT_LT(x, state().leftPanelWidth());
}

TEST_F(ViewerTest, TabWithNothingOpenSaysSo)
{
	m_v.open({layersEntry()});
	press(Event::Tab);
	EXPECT_EQ(m_c.m_ctx.m_message, "no panes to show");
}

TEST_F(ViewerTest, TShowsLayerNames)
{
	m_v.open({layersEntry()});
	press(key("T")); // the single view too
	EXPECT_EQ(m_c.m_ctx.m_message, "names: on");
	EXPECT_FALSE(m_v.typing());
	press(key("t"));
	press(key("T"));
	EXPECT_EQ(m_c.m_ctx.m_message, "names: off");
}

TEST_F(ViewerTest, FpsCyclesUpFromTheFileRate)
{
	m_v.open({sequenceEntry()});
	EXPECT_EQ(m_v.player().fps(), 0.0); // from the file (none: 24)
	press(key("F"));
	EXPECT_EQ(m_v.player().fps(), 25.0);
	press(key("F"));
	EXPECT_EQ(m_v.player().fps(), 30.0);
	press(key(" "));
	EXPECT_TRUE(m_v.playing());
	press(key(" "));
	EXPECT_FALSE(m_v.playing());
}

TEST_F(ViewerTest, RescanFindsFramesAddedToASequence)
{
	m_v.open({sequenceEntry(), layersEntry()});
	EXPECT_FALSE(m_v.rescan());
	writeExr(m_dir / "seq.0004.exr", {"R", "G", "B"});
	EXPECT_TRUE(m_v.rescan());
	EXPECT_EQ(state().frameCount(), 4);
	EXPECT_FALSE(m_v.rescan());
	EXPECT_EQ(m_v.shownDirs(), (std::vector<fs::path>{m_dir}));
}

TEST(AppContext, ReduceForAcceptsSlackWhilePlaying)
{
	// A half-block slot drawn 100x50 cells = 100x100 px.
	Context c;
	auto slot = newSlot(c.m_ctx);
	ftxui::Screen screen(100, 50);
	LayerImage img;
	img.m_width = img.m_height = 2;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 1, 1};
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, Plane(std::vector<float>(4, 0.5f)));
	slot->draw(screen,
	           {0, 99, 0, 49},
	           std::make_shared<const LayerImage>(std::move(img)),
	           {},
	           {});
	// 190 px into 100: 1.9x too many; floor keeps full resolution...
	const Box2i b{0, 0, 189, 189};
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, b, std::nullopt, 4'000'000), 1);
	// ...but up to 25% upscaling halves it.
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, b, std::nullopt, 4'000'000, 1.25), 2);
	// Small enough already: slack never goes below 1.
	const Box2i small{0, 0, 79, 79};
	EXPECT_EQ(c.m_ctx.reduceFor(*slot, small, std::nullopt, 4'000'000, 1.25),
	          1);
}

TEST_F(ViewerTest, InAndOutPointsBoundSteppingAndClearAgain)
{
	m_v.open({sequenceEntry()});
	press(key("."));
	press(key("I")); // in at the second frame
	EXPECT_EQ(state().m_in, 1);
	EXPECT_EQ(m_c.m_ctx.m_message, "in point: 2");
	press(key("."));
	press(key("O"));
	EXPECT_EQ(state().m_out, 2);
	press(key("."));
	EXPECT_EQ(state().m_frame, 1); // wrapped round to the in point
	press(key(","));
	EXPECT_EQ(state().m_frame, 2);
	press(key("<"));
	EXPECT_EQ(state().m_frame, 1);
	press(key("I")); // again on it: cleared
	EXPECT_FALSE(state().m_in);
	EXPECT_EQ(state().playRange().m_first, 0);
	// An in point past the out point drops the out point.
	press(key("O")); // out moves to the second frame
	EXPECT_EQ(state().m_out, 1);
	press(key(":"));
	press(key("3"));
	press(Event::Return);
	EXPECT_EQ(state().m_frame, 2);
	press(key("I"));
	EXPECT_EQ(state().m_in, 2);
	EXPECT_FALSE(state().m_out);

	m_v.open({sequenceEntry()}); // a fresh open forgets them
	EXPECT_FALSE(state().m_in);
	EXPECT_FALSE(state().m_out);
}

TEST_F(ViewerTest, ClickingThePlaybarScrubsAndPauses)
{
	m_v.open({sequenceEntry()});
	draw();
	auto mouse = [&](ftxui::Mouse::Button b, int x, int y, auto motion)
	{
		ftxui::Mouse m;
		m.button = b;
		m.motion = motion;
		m.x = x;
		m.y = y;
		press(Event::Mouse("", m));
	};
	press(key(" "));
	ASSERT_TRUE(m_v.playing());
	// The playbar's track: the row above the status bar.
	mouse(ftxui::Mouse::Left, 79, 22, ftxui::Mouse::Pressed);
	EXPECT_FALSE(m_v.playing());
	EXPECT_EQ(state().m_frame, 2);
	mouse(ftxui::Mouse::Left, 0, 22, ftxui::Mouse::Moved);
	EXPECT_EQ(state().m_frame, 0);
	mouse(ftxui::Mouse::Left, 0, 22, ftxui::Mouse::Released);
	mouse(ftxui::Mouse::WheelDown, 40, 22, ftxui::Mouse::Pressed);
	EXPECT_EQ(state().m_frame, 1);
	mouse(ftxui::Mouse::WheelUp, 40, 21, ftxui::Mouse::Pressed);
	EXPECT_EQ(state().m_frame, 0);
}

TEST_F(ViewerTest, XOpensTheFilesPaneButClearsThePickInTheInspector)
{
	m_v.open({layersEntry()});
	press(key("x"));
	EXPECT_EQ(state().m_focus, Focus::FILES);
	press(key("x")); // focused: closes
	EXPECT_EQ(state().m_focus, Focus::IMAGE);
	press(key("4"));
	press(key("x"));
	EXPECT_EQ(state().m_focus, Focus::INSPECT);
}

TEST_F(ViewerTest, LeftDragOnTheImageScrubsAndStopsAtTheEnds)
{
	m_v.open({sequenceEntry()});
	draw();
	auto mouse = [&](int x, auto motion)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = motion;
		m.x = x;
		m.y = 10;
		press(Event::Mouse("", m));
	};
	press(key(" "));
	const double cx = state().m_view.m_centerX;
	// 3 frames on the playbar's width: a third of it is a frame.
	const int third = (m_v.playbarWidth() + 2) / 3;
	mouse(20, ftxui::Mouse::Pressed);
	EXPECT_TRUE(m_v.playing()); // a click alone leaves playback be
	mouse(20 + third, ftxui::Mouse::Moved);
	EXPECT_FALSE(m_v.playing());
	EXPECT_EQ(state().m_frame, 1);
	mouse(20 + 5 * third, ftxui::Mouse::Moved);
	EXPECT_EQ(state().m_frame, 2); // the end: no wrap
	mouse(0, ftxui::Mouse::Moved);
	EXPECT_EQ(state().m_frame, 0);
	mouse(0, ftxui::Mouse::Released);
	EXPECT_EQ(state().m_view.m_centerX, cx); // no pan
}

TEST(Source, FilledGapsHoldTheNearestFrame)
{
	Source s;
	s.m_entry.m_kind = Entry::Kind::SEQUENCE;
	for(int n : {1001, 1050, 1075, 1100})
	{
		s.m_entry.m_frames.push_back("a." + std::to_string(n) + ".exr");
		s.m_entry.m_frameNumbers.push_back(n);
	}
	EXPECT_EQ(s.frameCount(), 4); // skipped: 4 frames at 24 fps
	EXPECT_EQ(s.frameLabel(1), "1050");

	s.m_fillGaps = true; // a frame per number
	EXPECT_EQ(s.frameCount(), 100);
	EXPECT_EQ(s.frameLabel(24), "1025");
	EXPECT_EQ(s.frame(24), "a.1001.exr"); // 24 from 1001, 25 from 1050
	EXPECT_EQ(s.frame(25), "a.1050.exr");
	EXPECT_EQ(s.frame(99), "a.1100.exr");
	EXPECT_TRUE(s.held(24));
	EXPECT_FALSE(s.held(49)); // 1050 is a file
	EXPECT_EQ(s.indexForFrameNumber(1060), 59);
	EXPECT_EQ(s.indexForFrameNumber(2000), 99);
}

TEST_F(ViewerTest, GFillsGapsAndKeepsTheFrameNumber)
{
	const fs::path dir = m_dir / "gaps";
	fs::create_directories(dir);
	for(int n : {1001, 1050, 1100})
	{
		writeExr(dir / ("g." + std::to_string(n) + ".exr"), {"R", "G", "B"});
	}
	m_v.open({entryForPath(dir / "g.1001.exr")});
	ASSERT_EQ(state().frameCount(), 3);
	press(key(">")); // the last: 1100
	press(key("G"));
	EXPECT_EQ(state().frameCount(), 100);
	EXPECT_EQ(state().m_frame, 99);
	press(key("G"));
	EXPECT_EQ(state().frameCount(), 3);
	EXPECT_EQ(state().m_frame, 2);
}

TEST_F(ViewerTest, HudShowsTheResolution)
{
	m_v.open(
	    {entryForPath(writeExr(m_dir / "res.exr", {"R", "G", "B"}, 40, 20))});
	draw();
	EXPECT_GE(titleAt(m_v, "40×20").first, 0);
}

TEST_F(ViewerTest, ADiffPairShowsLabelledTilesAndNGoesPairByPair)
{
	// B is wider: its extra pixels differ from A's nothing by 0.5.
	std::vector<OpenItem> items(2);
	items[0].m_a = entryForPath(writeExr(m_dir / "old.exr", {"R", "G", "B"}));
	items[0].m_labelsA.lines(Slot::TC).push_back("main");
	items[0].m_b =
	    entryForPath(writeExr(m_dir / "new.exr", {"R", "G", "B"}, 16, 4));
	items[0].m_labelsB.lines(Slot::TC).push_back("current");
	items[1].m_a = layersEntry();
	m_v.openItems(std::move(items));
	EXPECT_TRUE(m_v.tiled());
	drawSheet();
	EXPECT_EQ(m_v.tilesShown(), 3);
	// Each draw asks for what is missing (A and B, then their diff).
	for(const char* s : {"main", "current", "diff", "max Δ 0.5", "[1/2]"})
	{
		EXPECT_TRUE(waitFor([&] { return titleAt(m_v, s).first >= 0; })) << s;
	}
	// `A` hides them with the annotations.
	press(key("A"));
	EXPECT_LT(titleAt(m_v, "current").first, 0);
	EXPECT_LT(titleAt(m_v, "max Δ").first, 0);
	press(key("A"));

	press(key("n")); // the lone image: shown alone
	EXPECT_FALSE(m_v.tiled());
	EXPECT_EQ(m_v.state().m_current, 3);
	EXPECT_GE(titleAt(m_v, "[2/2]").first, 0);
	press(key("n")); // round to the pair, tiled again
	EXPECT_TRUE(m_v.tiled());
	EXPECT_EQ(m_v.state().m_current, 0);
}

TEST(OpenItems, LabelsFollowTheirFileIntoItsSequenceAndPairsStayPairs)
{
	const fs::path dir = freshDir("open-items");
	for(const char* f : {"s.0001.exr", "s.0002.exr", "x.exr", "y.exr"})
	{
		writeExr(dir / f, {"R"});
	}
	const std::string d = dir.string() + "/";
	std::vector<std::string> missing;
	const auto items = openItems({{d + "s.0001.exr:label=top:seq"},
	                              {d + "s.0002.exr"},
	                              {d + "x.exr:label=bl:old", d + "y.exr"},
	                              {d + "gone.exr", d + "y.exr"}},
	                             missing);
	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0].m_a.m_kind, Entry::Kind::SEQUENCE);
	EXPECT_EQ(items[0].m_labelsA.lines(Slot::TC),
	          std::vector<std::string>{"seq"});
	ASSERT_TRUE(items[1].m_b);
	EXPECT_EQ(items[1].m_labelsA.lines(Slot::BL),
	          std::vector<std::string>{"old"});
	EXPECT_FALSE(items[2].m_b); // A missing: B alone
	EXPECT_EQ(missing, std::vector<std::string>{d + "gone.exr"});
}
