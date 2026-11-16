#include "gui/Input.h"

#include "gui/Font.h"

#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>
#include <string>

using ftxui::Event;
using ftxui::Mouse;

namespace rega::gui
{

namespace
{

// UTF-8 for one code point.
std::string utf8(char32_t cp)
{
	std::string s;
	if(cp < 0x80)
	{
		s += static_cast<char>(cp);
	}
	else if(cp < 0x800)
	{
		s += static_cast<char>(0xC0 | (cp >> 6));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else if(cp < 0x10000)
	{
		s += static_cast<char>(0xE0 | (cp >> 12));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else
	{
		s += static_cast<char>(0xF0 | (cp >> 18));
		s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return s;
}

// The shortcut modifier: Cmd on macOS, Ctrl elsewhere (Ctrl works on macOS
// too, as in a terminal there).
bool shortcut(SDL_Keymod mod)
{
#ifdef __APPLE__
	if(mod & SDL_KMOD_GUI)
	{
		return true;
	}
#endif
	return (mod & SDL_KMOD_CTRL) != 0;
}

} // namespace

std::optional<Event> keyEvent(SDL_Keycode key, SDL_Keymod mod)
{
	const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
	const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
	const bool alt = (mod & SDL_KMOD_ALT) != 0;
	switch(key)
	{
		case SDLK_ESCAPE:
			return Event::Escape;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			return Event::Return;
		case SDLK_BACKSPACE:
			return Event::Backspace;
		case SDLK_TAB:
			return shift ? Event::TabReverse : Event::Tab;
		case SDLK_DELETE:
			return Event::Delete;
		case SDLK_INSERT:
			return Event::Insert;
		case SDLK_HOME:
			return Event::Home;
		case SDLK_END:
			return Event::End;
		case SDLK_PAGEUP:
			return Event::PageUp;
		case SDLK_PAGEDOWN:
			return Event::PageDown;
		case SDLK_UP:
			return Event::ArrowUp;
		case SDLK_DOWN:
			return Event::ArrowDown;
		case SDLK_LEFT:
			return Event::ArrowLeft;
		case SDLK_RIGHT:
			return Event::ArrowRight;
		case SDLK_F1:
			return Event::F1;
		case SDLK_F2:
			return Event::F2;
		case SDLK_F3:
			return Event::F3;
		case SDLK_F4:
			return Event::F4;
		case SDLK_F5:
			return Event::F5;
		case SDLK_F6:
			return Event::F6;
		case SDLK_F7:
			return Event::F7;
		case SDLK_F8:
			return Event::F8;
		case SDLK_F9:
			return Event::F9;
		case SDLK_F10:
			return Event::F10;
		case SDLK_F12:
			return Event::F12;
		default:
			break;
	}
	// Ctrl+letter: the control character a terminal sends (Ctrl+A = 0x01).
	if(ctrl && !alt && key >= SDLK_A && key <= SDLK_Z)
	{
		return Event::Special(
		    std::string(1, static_cast<char>(key - SDLK_A + 1)));
	}
	return std::nullopt;
}

std::vector<Event> textEvents(std::string_view text)
{
	std::vector<Event> out;
	for(char32_t cp : decodeUtf8(text))
	{
		if(cp >= 0x20 && cp != 0x7F)
		{
			out.push_back(Event::Character(utf8(cp)));
		}
	}
	return out;
}

Command command(SDL_Keycode key, SDL_Keymod mod)
{
	if(key == SDLK_F11)
	{
		return Command::FULLSCREEN;
	}
	if(!shortcut(mod) || (mod & SDL_KMOD_ALT))
	{
		return Command::NONE;
	}
	switch(key)
	{
		case SDLK_EQUALS:
		case SDLK_PLUS:
		case SDLK_KP_PLUS:
			return Command::FONT_BIGGER;
		case SDLK_MINUS:
		case SDLK_KP_MINUS:
			return Command::FONT_SMALLER;
		case SDLK_0:
		case SDLK_KP_0:
			return Command::FONT_RESET;
		case SDLK_O:
			return (mod & SDL_KMOD_SHIFT) ? Command::OPEN_FOLDER
			                              : Command::OPEN_FILES;
		default:
			return Command::NONE;
	}
}

void MouseCells::setGeometry(int cellW, int cellH, float density) noexcept
{
	m_cellW = std::max(1, cellW);
	m_cellH = std::max(1, cellH);
	m_density = density > 0 ? density : 1.0f;
}

Mouse MouseCells::at(float x, float y, SDL_Keymod mod) const
{
	Mouse m;
	m.x = static_cast<int>(std::floor(x * m_density / m_cellW));
	m.y = static_cast<int>(std::floor(y * m_density / m_cellH));
	m.shift = (mod & SDL_KMOD_SHIFT) != 0;
	m.control = (mod & SDL_KMOD_CTRL) != 0;
	m.meta = (mod & SDL_KMOD_ALT) != 0;
	return m;
}

std::optional<Mouse> MouseCells::moved(float x, float y, SDL_Keymod mod)
{
	Mouse m = at(x, y, mod);
	if(m.x == m_lastX && m.y == m_lastY)
	{
		return std::nullopt;
	}
	m_lastX = m.x;
	m_lastY = m.y;
	m.button = m_held;
	m.motion = Mouse::Moved;
	return m;
}

std::optional<Mouse>
MouseCells::button(int button, bool down, float x, float y, SDL_Keymod mod)
{
	Mouse m = at(x, y, mod);
	switch(button)
	{
		case SDL_BUTTON_LEFT:
			m.button = Mouse::Left;
			break;
		case SDL_BUTTON_MIDDLE:
			m.button = Mouse::Middle;
			break;
		case SDL_BUTTON_RIGHT:
			m.button = Mouse::Right;
			break;
		default:
			return std::nullopt;
	}
	m.motion = down ? Mouse::Pressed : Mouse::Released;
	m_held = down ? m.button : Mouse::None;
	m_lastX = m.x;
	m_lastY = m.y;
	return m;
}

std::vector<Mouse> MouseCells::wheel(float dy, float x, float y, SDL_Keymod mod)
{
	std::vector<Mouse> out;
	m_wheel += dy;
	// A touchpad sends fractions: a notch once they add up to one.
	while(std::abs(m_wheel) >= 1.0f)
	{
		Mouse m = at(x, y, mod);
		m.button = m_wheel > 0 ? Mouse::WheelUp : Mouse::WheelDown;
		m.motion = Mouse::Pressed;
		out.push_back(m);
		m_wheel += m_wheel > 0 ? -1.0f : 1.0f;
	}
	return out;
}

} // namespace rega::gui
