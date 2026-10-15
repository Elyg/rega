#include "app/App.h"

#include "image/Loader.h"
#include "term/Kitty.h"
#include "term/Output.h"
#include "util/Log.h"
#include "util/Ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

namespace
{
// How often the disk is checked for changes.
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1000);
// How often tmux's attached clients are checked (--transfer auto).
constexpr auto CLIENT_CHECK_INTERVAL = std::chrono::seconds(3);
} // namespace

App::App(const AppOptions& opts, ScreenInteractive& screen)
    : m_opts(opts), m_screen(screen),
      m_caps(detectCaps(opts.m_graphics, opts.m_transfer)),
      m_kitty(m_caps.m_tmux, m_caps.m_transfer, &term::writeRaw),
      m_svc([this] { m_screen.PostEvent(Event::Custom); },
            static_cast<size_t>(opts.m_cacheBytes)),
      m_annPath(Annotations::defaultStatePath()),
      m_colour(ColourManager::defaultStatePath()),
      m_ctx{.m_svc = m_svc,
            .m_caps = m_caps,
            .m_kitty = m_kitty,
            .m_ann = m_ann,
            .m_colour = m_colour,
            .m_redraw = [this] { m_screen.PostEvent(Event::Custom); },
            .m_post = [this](std::function<void()> task)
            { m_screen.Post(std::move(task)); },
            .m_quit = [this] { m_screen.ExitLoopClosure()(); },
            .m_message = {}},
      m_browser(
          m_ctx,
          opts.m_chooserFile,
          [this](std::vector<Entry> entries, bool tile)
          {
	          m_viewer.open(std::move(entries), tile);
	          m_mode = Mode::VIEWER;
          },
          [this](std::vector<Entry> marked)
          {
	          m_viewer.openItems(pairItems(std::move(marked)));
	          m_mode = Mode::VIEWER;
          }),
      m_viewer(m_ctx, [this] { m_mode = Mode::BROWSER; }),
      m_watcher(POLL_INTERVAL,
                [this](std::vector<fs::path> changed)
                {
	                m_screen.Post([this, changed = std::move(changed)]
	                              { filesChanged(changed); });
                })
{
	m_kitty.setLinkRate(opts.m_linkRate);
	// Who watches a tmux session changes while rvtui runs: a laptop attaches
	// over ssh (it can't read our files), then leaves (files are faster).
	if(opts.m_transfer == "auto" && m_caps.m_tmux &&
	   m_caps.m_graphics == GraphicsMode::KITTY)
	{
		m_clientCheck = std::jthread(
		    [this, clients = tmuxClientPids()](std::stop_token stop) mutable
		    {
			    std::mutex mu;
			    std::condition_variable_any cv;
			    std::unique_lock lock(mu);
			    for(;;)
			    {
				    cv.wait_for(lock,
				                stop,
				                CLIENT_CHECK_INTERVAL,
				                [] { return false; });
				    if(stop.stop_requested())
				    {
					    return;
				    }
				    std::string now = tmuxClientPids();
				    if(now != clients)
				    {
					    clients = std::move(now);
					    const Transfer t =
					        autoTransfer(true,
					                     false,
					                     tmuxClientOverSsh(clients));
					    m_screen.Post([this, t] { clientsChanged(t); });
				    }
			    }
		    });
	}
	spdlog::info("terminal '{}', graphics {} ({}), cell {}x{}px, tmux {}",
	             m_caps.m_terminalName,
	             graphicsModeName(m_caps.m_graphics),
	             transferName(m_caps.m_transfer),
	             m_caps.m_cellW,
	             m_caps.m_cellH,
	             m_caps.m_tmux);
	if(!m_caps.m_note.empty())
	{
		spdlog::warn("{}", m_caps.m_note); // stays in the status bar
	}
	if(!m_annPath.empty() && m_ann.load(m_annPath))
	{
		spdlog::info("annotations loaded from {}", m_annPath.string());
	}
	m_colour.loadDefault(); // $OCIO, else the config picked last time
	if(!m_colour.error().empty())
	{
		m_ctx.m_message = "OCIO: " + m_colour.error();
	}

	std::error_code ec;
	const auto& paths = opts.m_paths;
	if(opts.m_args.empty() &&
	   (paths.empty() || (paths.size() == 1 && fs::is_directory(paths[0], ec))))
	{
		m_browser.openDirectory(
		    paths.empty() ? fs::current_path()
		                  : fs::absolute(paths[0], ec).lexically_normal());
	}
	else
	{
		std::vector<OpenArg> args = opts.m_args;
		if(args.empty())
		{
			for(const auto& p : paths)
			{
				args.push_back({p});
			}
		}
		std::vector<std::string> missing;
		std::vector<OpenItem> items = openItems(args, missing);
		const bool pairs = std::ranges::any_of(items,
		                                       [](const OpenItem& i)
		                                       { return i.m_b.has_value(); });
		std::vector<Entry> entries;
		for(const auto& i : items)
		{
			entries.push_back(i.m_a);
		}
		if(entries.empty())
		{
			m_browser.openDirectory(fs::current_path());
			m_ctx.m_message = missing.empty() ? "no images to open"
			                                  : "not found: " + missing.front();
		}
		else
		{
			// Browse the first image's directory with the cursor on it (or
			// on the sequence it belongs to), so `q` lands somewhere useful.
			const fs::path first = entries.front().m_path;
			m_browser.openDirectory(first.parent_path(),
			                        entryForPath(first).m_name);
			// Several: they are selected (marked) there too, so Enter in the
			// browser opens the same set again. One is just under the cursor.
			// Pairs: A sides are often copies elsewhere (git's other
			// version), so nothing is marked.
			if(entries.size() > 1 && !pairs)
			{
				m_browser.mark(entries);
			}
			m_viewer.openItems(std::move(items));
			m_mode = Mode::VIEWER;
			if(!missing.empty())
			{
				m_ctx.m_message = "not found: " + missing.front();
			}
		}
	}
	updateWatch();
	if(opts.m_tutorial)
	{
		m_tour.emplace();
	}
}

App::~App()
{
	m_watcher.stop();
	m_viewer.finishTyping();
	if(!m_annPath.empty() && m_ann.save(m_annPath))
	{
		spdlog::info("annotations saved to {}", m_annPath.string());
	}
	if(!m_opts.m_cwdFile.empty())
	{
		std::ofstream(m_opts.m_cwdFile) << m_browser.cwd().string();
	}
}

void App::updateWatch()
{
	std::vector<fs::path> dirs = m_browser.shownDirs();
	if(m_mode == Mode::VIEWER)
	{
		for(auto& d : m_viewer.shownDirs())
		{
			if(std::ranges::find(dirs, d) == dirs.end())
			{
				dirs.push_back(std::move(d));
			}
		}
	}
	if(dirs != m_watched)
	{
		m_watched = dirs;
		m_watcher.watch(std::move(dirs));
	}
}

TourView App::tourView() const
{
	TourView v;
	v.m_viewer = m_mode == Mode::VIEWER;
	const ViewerState& s = m_viewer.state();
	if(v.m_viewer && !s.m_sources.empty())
	{
		v.m_open = s.m_sources[s.m_current].m_entry.m_name;
		v.m_sources = static_cast<int>(s.m_sources.size());
		v.m_exposure = s.m_disp.m_exposure;
	}
	return v;
}

void App::clientsChanged(Transfer t)
{
	// Every picture again: the new mode, and a terminal that just attached
	// has none of them.
	++m_caps.m_resend;
	if(t != m_caps.m_transfer)
	{
		m_caps.m_transfer = t;
		m_kitty.setTransfer(t);
		spdlog::info("transfer now {}", transferName(t));
		m_ctx.m_message = t == Transfer::DIRECT
		                      ? "a terminal attached over ssh: images go inline"
		                      : "no terminal over ssh: images go through files";
	}
	m_screen.PostEvent(Event::Custom);
}

void App::filesChanged(const std::vector<fs::path>& changed)
{
	bool redraw = m_browser.refresh();
	if(m_mode == Mode::VIEWER)
	{
		redraw |= m_viewer.rescan();
	}
	redraw |= m_svc.checkForChanges(changed);
	if(redraw)
	{
		m_screen.PostEvent(Event::Custom);
	}
}

Element App::render()
{
	if(refreshCellSize(m_caps))
	{
		spdlog::info("cell size now {}x{}px", m_caps.m_cellW, m_caps.m_cellH);
	}
	Element body =
	    m_mode == Mode::BROWSER ? m_browser.render() : m_viewer.render();
	if(m_tour && !m_tour->hidden())
	{
		// Bottom-right, clear of the playbar and the status bar.
		body = dbox({body,
		             vbox({filler(),
		                   hbox({filler(), m_tour->render(), text("  ")}),
		                   text(""),
		                   text(""),
		                   text(""),
		                   text("")})});
	}
	if(m_help)
	{
		body = dbox({body, renderHelp() | clear_under | center});
	}
	updateWatch();
	if(m_caps.m_graphics == GraphicsMode::KITTY)
	{
		body = drawAfter(body, compactPlaceholders);
	}
	return body;
}

Element App::renderHelp()
{
	// A key and what it does; `kpos` / `dpos` (matches) highlighted.
	auto line = [](const std::string& k,
	               const std::string& d,
	               const std::vector<size_t>& kpos = {},
	               const std::vector<size_t>& dpos = {})
	{
		return hbox({ui::highlighted(k, kpos, bold | color(Color::Yellow)) |
		                 size(WIDTH, EQUAL, 16),
		             ui::highlighted(d, dpos, nothing)});
	};
	auto heading = [](const std::string& name)
	{ return text(name) | bold | color(Color::Cyan); };
	// Every row is also kept, under its section, for the search.
	struct Entry
	{
		std::string m_section, m_key, m_text;
	};
	std::vector<Entry> entries;
	std::string current;
	auto row = [&](const std::string& k, const std::string& d)
	{
		entries.push_back({current, k, d});
		return line(k, d);
	};
	auto section = [&](const std::string& name)
	{
		current = name;
		return heading(name);
	};
	// Only the keys of the mode you are in.
	std::string title;
	Element body;
	if(m_mode == Mode::BROWSER)
	{
		title = " rvtui browser keys ";
		body = vbox({
		    row("j/k ↑/↓", "move"),
		    row("h/l ←/→", "parent / enter dir or view image"),
		    row("gg / G", "top / bottom"),
		    row("/", "fuzzy filter (Enter keep, Esc clear)"),
		    row("e", "expand sequence into frames / collapse"),
		    row(".", "toggle hidden files"),
		    row("space", "mark (view marked together)"),
		    row("a", "mark every image here (again: unmark)"),
		    row("d", "diff the marked in pairs (one marked: with this)"),
		    row("click", "select (double: open) / parent dir"),
		    row("o", "choose → --chooser-file, quit"),
		    row("q / Q", "quit"),
		});
	}
	else
	{
		title = " rvtui viewer keys ";
		Element left = vbox({
		    section("Image"),
		    row("[ / ]", "previous / next layer (AOV)"),
		    row("n / p N", "next / previous image (marked files)"),
		    row("", "diff: next / previous pair (A, B, |A - B|)"),
		    row("+ / - / wheel", "zoom"),
		    row("f / z", "fit / 1:1"),
		    row("hjkl ←↓↑→ HJKL", "pan (shift = faster)"),
		    row("right drag", "pan (middle button too)"),
		    row("t", "tile: all layers, or all marked images"),
		    row("  +- wheel hjkl", "tile: zoom / pan the sheet (f fit, z 1:1)"),
		    row("click", "select tile / focus pane"),
		    row("drag", "column edges: width; files title: height"),
		    row("Enter (tile)", "open the selected tile"),
		    row("T", "layer name on the image (tiles: each tile's)"),
		    row("Ctrl+click", "pick colour"),
		    row("", "[x, y] as in Nuke: from the bottom-left"),
		    text(""),
		    section("Sequence"),
		    row("space", "play / pause"),
		    row(", .  < >", "step frame / first, last"),
		    row(":", "go to frame (file number, nearest)"),
		    row("G", "missing frames: hold the nearest (timing) / skip"),
		    row("I / O", "in / out point here (again: clear)"),
		    row("", "playback loops in to out; , . < > stay inside"),
		    row("left drag", "scrub (on the image or the playbar)"),
		    row("playbar", "click: go to frame, wheel: step"),
		    row("F", "cycle playback fps"),
		    row("P", "playback: capped resolution (keeps up) / full res"),
		});
		Element right = vbox({
		    section("Display"),
		    row("c r g b a u", "colour / R / G / B / alpha / luma"),
		    row("e / E", "exposure +/- 0.5 stop"),
		    row("y / Y", "gamma +/- 0.1"),
		    row("s", "view transform (sRGB / OCIO view) / raw"),
		    row("w", "outlines (off): frame + data (dashed) / frame / off"),
		    row("w (tile)", "selection / frame + selection / frame / off"),
		    row("!", "paint NaN (magenta) / inf (cyan) pixels"),
		    row("0", "reset exposure, gamma, channel"),
		    row("A", "show / hide annotations"),
		    text(""),
		    section("Panes"),
		    row("2 3 4 5 6", "metadata / files / inspector / layers / colour:"),
		    row("m o/x i /", "the same, as letters"),
		    row("", "open + focus; again while focused closes"),
		    row("1", "focus the image"),
		    row("Tab", "hide all panes / bring them back"),
		    row("  j k  y  Y  x",
		        "inspector: move, copy line / value, clear pick"),
		    row("  wheel { }", "metadata: scroll"),
		    row("  /  click", "metadata: filter (Esc clear) / move there"),
		    row("  d", "metadata, diff tile: differences / both whole"),
		    row("  drag C-click", "metadata: select rows (then y / Y)"),
		    row("  J / K  d", "files: move down / up, drop"),
		    row("  e", "files: sequence ⇄ its frames (up to 500)"),
		    row("  l / h", "files: into a row's annotations / back"),
		    row("  D D", "files: clear all annotations (in a row: its own)"),
		    row("  a A Enter d",
		        "annotations: add below / above, edit, delete"),
		    row("  J / K  g / s", "annotations: reorder, to global / source"),
		    row("  Tab ↑↓ C-n", "typing: next slot, history, [#key] / [#@key]"),
		    row("  j k gg G", "metadata: move (Ctrl-d/u half page)"),
		    row("  h/l  Enter", "colour: change the row / pick from a list"),
		    row("  v  y  Y", "select lines / copy line / copy value"),
		    text(""),
		    row("q / Esc", "back to browser"),
		    row("Q", "quit rvtui"),
		});
		// Two columns when they fit side by side, else one (scrollable).
		constexpr int TWO_COLUMN_WIDTH = 118;
		body = Terminal::Size().dimx >= TWO_COLUMN_WIDTH
		           ? hbox({left, text("   "), right})
		           : vbox({left, text(""), right});
	}
	if(!m_helpQuery.empty())
	{
		// The rows whose key or text holds what was typed, one column,
		// under their sections; a row's continuation ("" key) comes along.
		// In a box the size of the full list, so typing doesn't resize it.
		body->ComputeRequirement();
		const int fullW = body->requirement().min_x;
		const int fullH = body->requirement().min_y;
		Elements found;
		std::string shownSection;
		for(size_t i = 0; i < entries.size(); ++i)
		{
			const Entry& e = entries[i];
			auto kpos = ui::findIgnoringCase(e.m_key, m_helpQuery);
			auto dpos = ui::findIgnoringCase(e.m_text, m_helpQuery);
			if(kpos.empty() && dpos.empty())
			{
				continue;
			}
			if(!e.m_section.empty() && e.m_section != shownSection)
			{
				if(!found.empty())
				{
					found.push_back(text(""));
				}
				found.push_back(heading(e.m_section));
				shownSection = e.m_section;
			}
			found.push_back(line(e.m_key, e.m_text, kpos, dpos));
			for(size_t j = i + 1;
			    j < entries.size() && entries[j].m_key.empty() &&
			    !entries[j].m_text.empty();
			    ++j)
			{
				found.push_back(line("", entries[j].m_text));
			}
		}
		body = (found.empty() ? text("no keys match") | dim
		                      : vbox(std::move(found))) |
		       size(WIDTH, EQUAL, fullW) | size(HEIGHT, GREATER_THAN, fullH);
	}
	// Scrollable when the terminal is still too short: ↑/↓ or wheel.
	const int maxH = std::max(5, Terminal::Size().dimy - 6);
	m_helpScroll = std::clamp(m_helpScroll, 0, 100);
	const Element footer =
	    m_helpQuery.empty()
	        ? text(" type to search   ↑/↓ scroll   ? / Esc close") | dim
	        : hbox({text(" search › ") | color(Color::Yellow) | bold,
	                text(m_helpQuery),
	                text(" ") | inverted,
	                filler(),
	                text("  Esc clear ") | dim});
	return vbox({
	           text(title) | bold | hcenter,
	           separator(),
	           body | focusPositionRelative(0, m_helpScroll / 100.0f) |
	               vscroll_indicator | yframe | size(HEIGHT, LESS_THAN, maxH),
	           separator(),
	           footer,
	       }) |
	       border | bgcolor(Color::Black);
}

bool App::helpEvent(Event e)
{
	// Typing searches; ↑/↓ / PgUp / PgDn / wheel scroll (in 10% steps);
	// Esc clears the search, then closes; `?` (nothing typed) closes, as
	// does any other key.
	int step = 0;
	if(e.is_mouse())
	{
		auto b = e.mouse().button;
		step = b == Mouse::WheelDown ? 1 : b == Mouse::WheelUp ? -1 : 0;
		if(step == 0)
		{
			return false;
		}
	}
	else if(e == Event::ArrowDown)
	{
		step = 1;
	}
	else if(e == Event::ArrowUp)
	{
		step = -1;
	}
	else if(e == Event::PageDown || e == Event::PageUp)
	{
		step = e == Event::PageDown ? 5 : -5;
	}
	if(step != 0)
	{
		m_helpScroll = std::clamp(m_helpScroll + step * 10, 0, 100);
		return true;
	}
	if(e == Event::Escape && !m_helpQuery.empty())
	{
		m_helpQuery.clear();
		m_helpScroll = 0;
		return true;
	}
	if(e == Event::Backspace)
	{
		// One glyph: drop UTF-8 continuation bytes with it.
		while(!m_helpQuery.empty() &&
		      (static_cast<unsigned char>(m_helpQuery.back()) & 0xC0) == 0x80)
		{
			m_helpQuery.pop_back();
		}
		if(!m_helpQuery.empty())
		{
			m_helpQuery.pop_back();
		}
		return true;
	}
	if(e.is_character() && !(e == Event::Character('?') && m_helpQuery.empty()))
	{
		m_helpQuery += e.character();
		m_helpScroll = 0;
		return true;
	}
	m_help = false;
	m_helpQuery.clear();
	return true;
}

bool App::onEvent(Event e)
{
	if(e == Event::Custom)
	{
		if(m_mode == Mode::VIEWER)
		{
			m_viewer.tick();
		}
		return true;
	}
	if(e.is_mouse())
	{
		m_viewer.setMouse(e.mouse().x, e.mouse().y);
	}
	if(m_help)
	{
		return helpEvent(e);
	}
	// Shift+Q quits from anywhere (except while typing a filter or text).
	const bool typing = m_browser.typing() || m_viewer.typing();
	if(e == Event::Character('Q') && !typing)
	{
		m_ctx.m_quit();
		return true;
	}
	if(e == Event::Character('?') && !typing)
	{
		m_help = true;
		m_helpScroll = 0;
		return true;
	}
	if(!e.is_mouse())
	{
		m_ctx.m_message.clear(); // messages stay until the next key, not mouse
	}
	if(m_tour && (e == Event::F1 || e == Event::F2 || e == Event::F3))
	{
		if(e == Event::F1)
		{
			m_tour->press(e); // the last step names it
			m_tour->toggleHidden();
		}
		else if(e == Event::F2)
		{
			m_tour->skip();
		}
		else
		{
			m_tour->back();
		}
		return true;
	}
	if(m_tour && !typing)
	{
		m_tour->press(e);
	}
	const bool used =
	    m_mode == Mode::BROWSER ? m_browser.event(e) : m_viewer.event(e);
	if(m_tour)
	{
		m_tour->update(tourView());
	}
	return used;
}

int runApp(const AppOptions& opts)
{
	log::redirectToFile();
	auto screen = ScreenInteractive::Fullscreen();
	// Placeholder cells encode the kitty image id in a 24-bit foreground
	// colour, so FTXUI must never downgrade colours. Ghostty/kitty are
	// truecolour anyway.
	if(detectCaps(opts.m_graphics).m_graphics == GraphicsMode::KITTY)
	{
		Terminal::SetColorSupport(Terminal::Color::TrueColor);
	}

	{
		App app(opts, screen);
		auto root = Renderer([&] { return app.render(); });
		root |= CatchEvent([&](Event e) { return app.onEvent(e); });
		screen.Loop(root);
	}
	spdlog::info("exit");
	return 0;
}

} // namespace rv
