#include "app/Tour.h"

#include <ftxui/component/mouse.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

using namespace ftxui;

namespace rega
{

namespace
{

struct Step
{
	const char* m_title;
	/// Keys in braces ("{space} plays"); every one must be pressed, in any
	/// order, so each is one that's safe to try where the step is.
	const char* m_text;
	/// Where the user must be as well, or null for anywhere.
	bool (*m_where)(const TourView& now);
	bool m_last = false; ///< the closing card: never done
};

const std::array<Step, 15> STEPS = {{
    {"The browser",
     "{j} / {k} (or the arrows) move through the folder; the preview on "
     "the right follows the cursor.",
     nullptr},
    {"Open the sequence",
     "shot.####.exr is 100 frames, 1001-1100, in one row. Move onto it and "
     "press {Enter}.",
     [](const TourView& n)
     { return n.m_viewer && n.m_open.starts_with("shot."); }},
    {"Play",
     "{space} plays the sequence. The bar at the bottom turns green as "
     "frames are cached; the fps on its right is what's kept up.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Step and scrub",
     "{space} again pauses. {,} and {.} step a frame, {<} {>} jump to the "
     "ends, {:} goes to a frame number. {Left-drag} on the image or the bar "
     "scrubs.",
     [](const TourView& n) { return n.m_viewer; }},
    {"In and out",
     "{Shift+i} sets the in point at this frame, {Shift+o} the out point: "
     "playback loops between them. Pressed again on the same frame, each "
     "clears its point.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Layers",
     "This render has layers (AOVs). {[} and {]} step through them: "
     "diffuse, specular, albedo, N, Z, mask. {5} lists them all (again: "
     "closes the list).",
     [](const TourView& n) { return n.m_viewer; }},
    {"Every layer at once",
     "{t} tiles every layer side by side. Click one to select it; {Enter} "
     "opens it.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Channels",
     "{c} {r} {g} {b} {a} {u}: colour, red, green, blue, alpha and luma.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Exposure",
     "{e} / {E}: exposure up / down half a stop. {y} / {Y}: gamma +/- 0.1. "
     "{0} resets them and the channel.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Read a pixel",
     "{Ctrl+click} a pixel, and {4} opens the inspector: its values in this "
     "layer, at coordinates counted as Nuke does.",
     [](const TourView& n) { return n.m_viewer; }},
    {"Back to the browser",
     "{q} goes back to the browser (from the tiles, twice). The sequence "
     "keeps its place. {Shift+q} quits rega from anywhere.",
     [](const TourView& n) { return !n.m_viewer; }},
    {"Brighter than white",
     "Open hdr-sky.exr: its sun is about 50 times brighter than white. {E} "
     "stops down: press it until the sun's disc shows.",
     [](const TourView& n)
     {
	     return n.m_viewer && n.m_open == "hdr-sky.exr" &&
	            n.m_exposure <= -3.0f;
     }},
    {"Broken pixels",
     "{q}, then open nan-inf.exr. The status bar counts its NaN and inf "
     "pixels; {!} paints them (NaN magenta, inf cyan). {[} {]} find the "
     "layer they came from.",
     [](const TourView& n) { return n.m_viewer && n.m_open == "nan-inf.exr"; }},
    {"Several at once",
     "{q}, then in the browser mark a few stills with {space} and press "
     "{Enter}: {n} / {p} step through them, {t} tiles them.",
     [](const TourView& n) { return n.m_viewer && n.m_sources >= 2; }},
    {"That's the tour",
     "{?} lists every key and searches them as you type. Each tutorial "
     "image's metadata ({2}) says what it shows. {F1} hides this card.",
     nullptr,
     true},
}};

// The keys a step's text names, in order, each once.
std::vector<std::string> keysOf(std::string_view text)
{
	std::vector<std::string> keys;
	for(size_t open = text.find('{'); open != std::string_view::npos;
	    open = text.find('{', open + 1))
	{
		const size_t close = text.find('}', open);
		if(close == std::string_view::npos)
		{
			break;
		}
		std::string key(text.substr(open + 1, close - open - 1));
		if(std::ranges::find(keys, key) == keys.end())
		{
			keys.push_back(std::move(key));
		}
	}
	return keys;
}

// Whether `e` is the key (or mouse action) a step calls `key`.
bool matches(const std::string& key, Event e)
{
	if(e.is_mouse())
	{
		const Mouse& m = e.mouse();
		if(key == "Ctrl+click")
		{
			return m.button == Mouse::Left && m.motion == Mouse::Pressed &&
			       m.control;
		}
		if(key == "Left-drag")
		{
			return m.button == Mouse::Left && m.motion == Mouse::Moved;
		}
		return false;
	}
	if(key == "space")
	{
		return e == Event::Character(' ');
	}
	if(key == "Enter")
	{
		return e == Event::Return;
	}
	if(key.size() == 7 && key.starts_with("Shift+"))
	{
		return e == Event::Character(static_cast<char>(
		                std::toupper(static_cast<unsigned char>(key[6]))));
	}
	if(key == "F1")
	{
		return e == Event::F1;
	}
	if(key == "j" && e == Event::ArrowDown)
	{
		return true;
	}
	if(key == "k" && e == Event::ArrowUp)
	{
		return true;
	}
	return e == Event::Character(key);
}

// A key press or a click: what moves a done step on (not mouse moves, and
// not F1, which only hides the card; App handles F2 / F3 itself).
bool movesOn(Event e)
{
	if(e.is_mouse())
	{
		return e.mouse().motion == Mouse::Pressed;
	}
	return e != Event::F1 && e != Event::Custom;
}

// `text` wrapped like paragraph(), with the keys in braces bold: green once
// `isPressed`, else yellow (as in the help). A key never breaks across lines.
template <class IsPressed>
Element keyedParagraph(std::string_view text, IsPressed&& isPressed)
{
	Elements words, word;
	std::string run;
	bool key = false;
	auto endRun = [&]
	{
		if(!run.empty())
		{
			Element e = ftxui::text(run);
			if(key)
			{
				e = e | bold |
				    color(isPressed(run) ? Color::Green : Color::Yellow);
			}
			word.push_back(e);
			run.clear();
		}
	};
	auto endWord = [&]
	{
		endRun();
		if(!word.empty())
		{
			word.push_back(ftxui::text(" "));
			words.push_back(hbox(std::move(word)));
			word.clear();
		}
	};
	for(char c : text)
	{
		if(c == '{' || c == '}')
		{
			endRun();
			key = c == '{';
		}
		else if(c == ' ' && !key)
		{
			endWord();
		}
		else
		{
			run += c;
		}
	}
	endWord();
	return hflow(std::move(words));
}

} // namespace

Tour::Tour()
{
	enter(0);
}

int Tour::stepCount()
{
	return static_cast<int>(STEPS.size());
}

bool Tour::finished() const
{
	return STEPS[m_step].m_last;
}

std::string Tour::title() const
{
	return STEPS[m_step].m_title;
}

bool Tour::pressed(const std::string& key) const
{
	const auto it = std::ranges::find(m_keys, key);
	return it != m_keys.end() && m_pressed[it - m_keys.begin()];
}

void Tour::enter(int step)
{
	m_step = step;
	m_done = false;
	m_keys = keysOf(STEPS[step].m_text);
	m_pressed.assign(m_keys.size(), false);
}

void Tour::press(Event e)
{
	if(m_done && movesOn(e))
	{
		enter(m_step + 1); // and the key counts there
	}
	for(size_t i = 0; i < m_keys.size(); ++i)
	{
		if(matches(m_keys[i], e))
		{
			m_pressed[i] = true;
		}
	}
}

bool Tour::update(const TourView& now)
{
	const Step& s = STEPS[m_step];
	if(m_done || s.m_last ||
	   !std::ranges::all_of(m_pressed, [](bool p) { return p; }) ||
	   (s.m_where && !s.m_where(now)))
	{
		return false;
	}
	m_done = true;
	return true;
}

void Tour::skip()
{
	if(!finished())
	{
		enter(m_step + 1);
	}
}

void Tour::back()
{
	enter(std::max(0, m_step - 1));
}

Element Tour::render() const
{
	constexpr int CARD_WIDTH = 44;
	const Step& s = STEPS[m_step];
	auto isPressed = [this](const std::string& k) { return pressed(k); };
	auto none = [](const std::string&) { return false; };
	Element title = m_done ? text(std::string("✓ ") + s.m_title) | bold |
	                             color(Color::Green)
	                       : text(s.m_title) | bold;
	// F3 only once there's a step to go back to.
	std::string footer = m_done     ? "any key: next · {F1} hide"
	                     : s.m_last ? "{F1} hide"
	                                : "{F1} hide · {F2} skip";
	if(m_step > 0)
	{
		footer += " · {F3} back";
	}
	return window(text(fmt::format(" tutorial {}/{} ",
	                               m_step + 1,
	                               stepCount())) |
	                  color(Color::Cyan),
	              vbox({
	                  title,
	                  keyedParagraph(s.m_text, isPressed),
	                  text(""),
	                  keyedParagraph(footer, none) | dim,
	              }) | size(WIDTH, EQUAL, CARD_WIDTH)) |
	       clear_under;
}

} // namespace rega
