// rega --gui: keys and mouse as a terminal sends them, the remembered
// settings, the built-in font.

#include "gui/Font.h"
#include "gui/Input.h"
#include "gui/Settings.h"

#include <SDL3/SDL_mouse.h>
#include <ftxui/component/event.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace rega::assets
{
extern const std::uint8_t FONT_REGULAR[];
extern const std::size_t FONT_REGULAR_SIZE;
extern const std::uint8_t FONT_BOLD[];
extern const std::size_t FONT_BOLD_SIZE;
} // namespace rega::assets

using namespace rega::gui;
using ftxui::Event;
using ftxui::Mouse;

namespace fs = std::filesystem;

TEST(GuiInput, SpecialKeysAreWhatATerminalSends)
{
	EXPECT_EQ(keyEvent(SDLK_ESCAPE, SDL_KMOD_NONE), Event::Escape);
	EXPECT_EQ(keyEvent(SDLK_RETURN, SDL_KMOD_NONE), Event::Return);
	EXPECT_EQ(keyEvent(SDLK_KP_ENTER, SDL_KMOD_NONE), Event::Return);
	EXPECT_EQ(keyEvent(SDLK_BACKSPACE, SDL_KMOD_NONE), Event::Backspace);
	EXPECT_EQ(keyEvent(SDLK_TAB, SDL_KMOD_NONE), Event::Tab);
	EXPECT_EQ(keyEvent(SDLK_TAB, SDL_KMOD_LSHIFT), Event::TabReverse);
	EXPECT_EQ(keyEvent(SDLK_UP, SDL_KMOD_NONE), Event::ArrowUp);
	EXPECT_EQ(keyEvent(SDLK_PAGEDOWN, SDL_KMOD_NONE), Event::PageDown);
	EXPECT_EQ(keyEvent(SDLK_HOME, SDL_KMOD_NONE), Event::Home);
	EXPECT_EQ(keyEvent(SDLK_DELETE, SDL_KMOD_NONE), Event::Delete);
	EXPECT_EQ(keyEvent(SDLK_F1, SDL_KMOD_NONE), Event::F1);
	EXPECT_EQ(keyEvent(SDLK_F3, SDL_KMOD_NONE), Event::F3);
}

TEST(GuiInput, CtrlLetterIsItsControlCharacter)
{
	EXPECT_EQ(keyEvent(SDLK_D, SDL_KMOD_LCTRL), Event::CtrlD);
	EXPECT_EQ(keyEvent(SDLK_U, SDL_KMOD_RCTRL), Event::CtrlU);
	EXPECT_EQ(keyEvent(SDLK_N, SDL_KMOD_LCTRL), Event::CtrlN);
	EXPECT_EQ(keyEvent(SDLK_P, SDL_KMOD_LCTRL), Event::CtrlP);
	EXPECT_EQ(keyEvent(SDLK_C, SDL_KMOD_LCTRL), Event::CtrlC); // copies
	EXPECT_EQ(command(SDLK_C, SDL_KMOD_LCTRL), Command::NONE);
	EXPECT_EQ(command(SDLK_V, SDL_KMOD_LCTRL), Command::PASTE);
}

TEST(GuiInput, TypingKeysComeAsText)
{
	// Their SDL_EVENT_TEXT_INPUT brings them, in the keyboard's layout.
	EXPECT_FALSE(keyEvent(SDLK_A, SDL_KMOD_NONE));
	EXPECT_FALSE(keyEvent(SDLK_A, SDL_KMOD_LSHIFT));
	EXPECT_FALSE(keyEvent(SDLK_SPACE, SDL_KMOD_NONE));
	const auto e = textEvents("Q? é");
	ASSERT_EQ(e.size(), 4u);
	EXPECT_EQ(e[0], Event::Character('Q'));
	EXPECT_EQ(e[1], Event::Character('?'));
	EXPECT_EQ(e[2], Event::Character(' '));
	EXPECT_EQ(e[3], Event::Character("é"));
	EXPECT_TRUE(textEvents("\x01\x7f").empty()); // no control characters
}

TEST(GuiInput, WindowCommands)
{
	EXPECT_EQ(command(SDLK_EQUALS, SDL_KMOD_LCTRL), Command::FONT_BIGGER);
	EXPECT_EQ(command(SDLK_MINUS, SDL_KMOD_LCTRL), Command::FONT_SMALLER);
	EXPECT_EQ(command(SDLK_0, SDL_KMOD_LCTRL), Command::FONT_RESET);
	EXPECT_EQ(command(SDLK_O, SDL_KMOD_LCTRL), Command::OPEN_FILES);
	EXPECT_EQ(command(SDLK_O, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT),
	          Command::OPEN_FOLDER);
	EXPECT_EQ(command(SDLK_F11, SDL_KMOD_NONE), Command::FULLSCREEN);
	// Plain keys are the app's.
	EXPECT_EQ(command(SDLK_O, SDL_KMOD_NONE), Command::NONE);
	EXPECT_EQ(command(SDLK_0, SDL_KMOD_NONE), Command::NONE);
	EXPECT_EQ(command(SDLK_EQUALS, SDL_KMOD_NONE), Command::NONE);
	// The app's own Ctrl keys stay its own.
	EXPECT_EQ(command(SDLK_D, SDL_KMOD_LCTRL), Command::NONE);
}

TEST(GuiInput, MouseInCellsAtTheWindowsPixelDensity)
{
	MouseCells m;
	m.setGeometry(10, 20, 2.0f); // a 2x display: 5 x 10 window units a cell
	auto press = m.button(SDL_BUTTON_LEFT, true, 12.0f, 25.0f, SDL_KMOD_LCTRL);
	ASSERT_TRUE(press);
	EXPECT_EQ(press->x, 2);
	EXPECT_EQ(press->y, 2);
	EXPECT_EQ(press->button, Mouse::Left);
	EXPECT_EQ(press->motion, Mouse::Pressed);
	EXPECT_TRUE(press->control);

	// Dragging, every move counts, within the cell too (a pan follows the
	// pointer finer than a cell); cells() has the fraction.
	auto within = m.moved(13.0f, 26.0f, SDL_KMOD_NONE);
	ASSERT_TRUE(within);
	EXPECT_EQ(within->x, 2);
	EXPECT_EQ(within->button, Mouse::Left);
	EXPECT_DOUBLE_EQ(m.cells(13.0f, 26.0f).first, 2.6);
	EXPECT_DOUBLE_EQ(m.cells(13.0f, 26.0f).second, 2.6);
	auto drag = m.moved(15.0f, 26.0f, SDL_KMOD_NONE);
	ASSERT_TRUE(drag);
	EXPECT_EQ(drag->x, 3);
	EXPECT_EQ(drag->button, Mouse::Left);
	EXPECT_EQ(drag->motion, Mouse::Moved);

	auto release =
	    m.button(SDL_BUTTON_LEFT, false, 15.0f, 26.0f, SDL_KMOD_NONE);
	ASSERT_TRUE(release);
	EXPECT_EQ(release->motion, Mouse::Released);
	auto hover = m.moved(30.0f, 26.0f, SDL_KMOD_NONE);
	ASSERT_TRUE(hover);
	EXPECT_EQ(hover->button, Mouse::None);
	// Hovering, only into another cell.
	EXPECT_FALSE(m.moved(31.0f, 26.0f, SDL_KMOD_NONE));
	EXPECT_FALSE(m.button(SDL_BUTTON_X1, true, 0, 0, SDL_KMOD_NONE));
}

TEST(GuiInput, WheelNotchesAddUpFromATouchpad)
{
	MouseCells m;
	m.setGeometry(10, 20, 1.0f);
	EXPECT_TRUE(m.wheel(0.4f, 0, 0, SDL_KMOD_NONE).empty());
	EXPECT_TRUE(m.wheel(0.4f, 0, 0, SDL_KMOD_NONE).empty());
	auto up = m.wheel(0.4f, 0, 0, SDL_KMOD_NONE);
	ASSERT_EQ(up.size(), 1u);
	EXPECT_EQ(up[0].button, Mouse::WheelUp);
	auto down = m.wheel(-3.0f, 0, 0, SDL_KMOD_NONE);
	ASSERT_EQ(down.size(),
	          2u); // 0.2 left over, then -3: two whole notches down
	EXPECT_EQ(down[0].button, Mouse::WheelDown);
}

TEST(GuiSettings, RoundTripAndDefaults)
{
	const fs::path dir =
	    fs::temp_directory_path() / "rega-tests" / "gui-settings";
	fs::remove_all(dir);
	const fs::path file = dir / "window";

	const Settings none = Settings::load(file); // missing: defaults
	EXPECT_EQ(none.m_fontSize, Settings::DEFAULT_FONT_SIZE);
	EXPECT_TRUE(none.m_lastDir.empty());

	Settings s;
	s.m_fontSize = 18;
	s.m_width = 1000;
	s.m_height = 700;
	s.m_maximized = true;
	s.m_lastDir = fs::path(u8"/shots/sq010/ünïcode dir");
	ASSERT_TRUE(s.save(file));
	const Settings back = Settings::load(file);
	EXPECT_EQ(back.m_fontSize, 18);
	EXPECT_EQ(back.m_width, 1000);
	EXPECT_EQ(back.m_height, 700);
	EXPECT_TRUE(back.m_maximized);
	EXPECT_EQ(back.m_lastDir, s.m_lastDir);

	// Out of range or garbage: clamped, or the default kept.
	std::ofstream(file) << "font_size = 400\nwidth = abc\nheight=10\n";
	const Settings odd = Settings::load(file);
	EXPECT_EQ(odd.m_fontSize, Settings::MAX_FONT_SIZE);
	EXPECT_EQ(odd.m_width, Settings{}.m_width);
	EXPECT_EQ(odd.m_height, 240);
}

TEST(GuiFont, CellSizeFollowsThePixelHeight)
{
	Font f({rega::assets::FONT_REGULAR, rega::assets::FONT_REGULAR_SIZE},
	       {rega::assets::FONT_BOLD, rega::assets::FONT_BOLD_SIZE});
	f.setPixelHeight(15);
	const int w15 = f.cellW(), h15 = f.cellH();
	EXPECT_GE(w15, 8);
	EXPECT_LE(w15, 10);
	EXPECT_GT(h15, w15);
	f.setPixelHeight(30);
	EXPECT_NEAR(f.cellW(), 2 * w15, 1);
	EXPECT_NEAR(f.cellH(), 2 * h15, 1);
}

TEST(GuiFont, GlyphsAndShapes)
{
	Font f({rega::assets::FONT_REGULAR, rega::assets::FONT_REGULAR_SIZE},
	       {rega::assets::FONT_BOLD, rega::assets::FONT_BOLD_SIZE});
	f.setPixelHeight(16);
	EXPECT_EQ(f.glyph(U' ', false), nullptr);
	const Font::Glyph* a = f.glyph(U'A', false);
	ASSERT_NE(a, nullptr);
	EXPECT_GT(a->m_w, 0);
	EXPECT_GE(a->m_offY, 0);
	EXPECT_NE(f.glyph(U'A', true), nullptr);
	// The same glyph twice: the same place in the atlas.
	EXPECT_EQ(f.glyph(U'A', false), a);

	// Drawn shapes fill the cell exactly, so they join their neighbours.
	for(char32_t cp : {U'─', U'│', U'┼', U'█', U'▀', U'╭', U'═', U'⣿'})
	{
		const Font::Glyph* g = f.glyph(cp, false);
		ASSERT_NE(g, nullptr) << static_cast<uint32_t>(cp);
		EXPECT_EQ(g->m_w, f.cellW());
		EXPECT_EQ(g->m_h, f.cellH());
		EXPECT_EQ(g->m_offX, 0);
		EXPECT_EQ(g->m_offY, 0);
	}
	auto at = [&](const Font::Glyph* g, int x, int y)
	{
		return f.atlas()[static_cast<size_t>(g->m_y + y) * f.atlasSize() +
		                 g->m_x + x];
	};
	const Font::Glyph* h = f.glyph(U'─', false);
	EXPECT_EQ(at(h, 0, f.cellH() / 2), 255); // reaches the left edge
	EXPECT_EQ(at(h, f.cellW() - 1, f.cellH() / 2), 255); // and the right
	EXPECT_EQ(at(h, 0, 0), 0);
	const Font::Glyph* full = f.glyph(U'█', false);
	EXPECT_EQ(at(full, 0, 0), 255);
	EXPECT_EQ(at(full, f.cellW() - 1, f.cellH() - 1), 255);
	const Font::Glyph* upper = f.glyph(U'▀', false);
	EXPECT_EQ(at(upper, 0, 0), 255);
	EXPECT_EQ(at(upper, 0, f.cellH() - 1), 0);

	// A glyph the font lacks draws as "?" rather than nothing.
	EXPECT_NE(f.glyph(0x10FFF0, false), nullptr);
}

TEST(GuiFont, DecodesUtf8)
{
	EXPECT_EQ(decodeUtf8("a▀é"), U"a▀é");
	EXPECT_EQ(decodeUtf8("\xF4\x80\x80\x80"), std::u32string(1, 0x100000));
	EXPECT_EQ(decodeUtf8("\xff"), std::u32string(1, 0xFFFD));
	EXPECT_EQ(decodeUtf8("\xE2\x96"), U"��"); // cut short
}
