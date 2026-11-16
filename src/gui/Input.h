#pragma once

#include <SDL3/SDL_keycode.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace rega::gui
{

/// What a key press is to the app, as a terminal would send it: Escape, the
/// arrows, Ctrl+letter... nullopt for a key that types text (its
/// SDL_EVENT_TEXT_INPUT brings it, in the keyboard's layout) or means
/// nothing to the app.
[[nodiscard]] std::optional<ftxui::Event> keyEvent(SDL_Keycode key,
                                                   SDL_Keymod mod);

/// Typed text (SDL_EVENT_TEXT_INPUT, IME commits too): one Event::Character
/// a code point.
[[nodiscard]] std::vector<ftxui::Event> textEvents(std::string_view utf8);

/// The window's own keys, which the terminal doesn't have.
enum class Command
{
	NONE,
	FONT_BIGGER,  ///< Ctrl+= (Cmd on macOS)
	FONT_SMALLER, ///< Ctrl+-
	FONT_RESET,   ///< Ctrl+0
	OPEN_FILES,   ///< Ctrl+O: the system's Open dialog
	OPEN_FOLDER,  ///< Ctrl+Shift+O
	PASTE,        ///< Ctrl+V: the clipboard's text (App::pasteText)
	FULLSCREEN,   ///< F11
};
[[nodiscard]] Command command(SDL_Keycode key, SDL_Keymod mod);

/// Mouse events in cells, as a terminal reports them: a motion only when the
/// pointer enters another cell, the held button with it while dragging, the
/// wheel a notch at a time.
class MouseCells
{
public:
	/// Screen pixels per cell, and screen pixels per window coordinate
	/// (SDL_GetWindowPixelDensity).
	void setGeometry(int cellW, int cellH, float density) noexcept;

	/// Where (`x`, `y`), window coordinates, is in cells, the fractions
	/// kept: what a drag pans by, finer than a cell.
	[[nodiscard]] std::pair<double, double> cells(float x, float y) const;
	/// The pointer moved to (`x`, `y`), window coordinates: hovering, when
	/// it enters another cell; dragging, every move (the cell may be the
	/// same, cells() is not).
	[[nodiscard]] std::optional<ftxui::Mouse> moved(float x,
	                                                float y,
	                                                SDL_Keymod mod);
	/// SDL button `button` (SDL_BUTTON_LEFT...) went down / up.
	[[nodiscard]] std::optional<ftxui::Mouse>
	button(int button, bool down, float x, float y, SDL_Keymod mod);
	/// The wheel turned `dy` notches (up positive; fractions add up).
	[[nodiscard]] std::vector<ftxui::Mouse>
	wheel(float dy, float x, float y, SDL_Keymod mod);

private:
	[[nodiscard]] ftxui::Mouse at(float x, float y, SDL_Keymod mod) const;

	int m_cellW = 10, m_cellH = 20;
	float m_density = 1.0f;
	int m_lastX = -1, m_lastY = -1;
	ftxui::Mouse::Button m_held = ftxui::Mouse::None;
	float m_wheel = 0; ///< turned, not yet a notch
};

} // namespace rega::gui
