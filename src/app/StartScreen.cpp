#include "app/StartScreen.h"

#include "image/Sequence.h"
#include "util/Paths.h"
#include "util/Ui.h"

#include <ftxui/screen/string.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <span>
#include <string>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rega
{

namespace
{

constexpr int MAX_WIDTH = 76;

std::int64_t nowSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
	           std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

/// `s` padded with spaces to `width` cells (cut with … if longer).
std::string fit(std::string_view s, int width)
{
	std::string out = ui::ellipsizeEnd(s, width);
	const int w = string_width(out);
	if(w < width)
	{
		out.append(static_cast<size_t>(width - w), ' ');
	}
	return out;
}

} // namespace

std::string pastedPath(std::string_view text, const fs::path& home)
{
	auto trim = [](std::string_view v)
	{
		const auto b = v.find_first_not_of(" \t\r\n");
		if(b == std::string_view::npos)
		{
			return std::string_view{};
		}
		return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
	};
	std::string_view v = trim(text);
	if(v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
	   v.back() == v.front())
	{
		v = trim(v.substr(1, v.size() - 2));
	}
	std::string out;
	if(v.starts_with("file://"))
	{
		// A file manager's copy: file:///path or file://localhost/path,
		// %-escaped.
		v.remove_prefix(7);
		if(v.starts_with("localhost/"))
		{
			v.remove_prefix(9);
		}
		for(size_t i = 0; i < v.size(); ++i)
		{
			auto hex = [](char c) -> int
			{
				return c >= '0' && c <= '9'   ? c - '0'
				       : c >= 'a' && c <= 'f' ? c - 'a' + 10
				       : c >= 'A' && c <= 'F' ? c - 'A' + 10
				                              : -1;
			};
			if(v[i] == '%' && i + 2 < v.size() && hex(v[i + 1]) >= 0 &&
			   hex(v[i + 2]) >= 0)
			{
				out += static_cast<char>(hex(v[i + 1]) * 16 + hex(v[i + 2]));
				i += 2;
			}
			else
			{
				out += v[i];
			}
		}
		// file:///C:/x on Windows
		if(out.size() >= 3 && out[0] == '/' &&
		   std::isalpha(static_cast<unsigned char>(out[1])) && out[2] == ':')
		{
			out.erase(0, 1);
		}
	}
	else
	{
		out = v;
	}
	if(!home.empty() && (out == "~" || out.starts_with("~/")))
	{
		const std::u8string h = home.u8string();
		out = std::string(h.begin(), h.end()) + out.substr(1);
	}
	return out;
}

std::vector<std::string> pastedPaths(std::string_view text,
                                     const fs::path& home)
{
	std::vector<std::string> out;
	while(!text.empty())
	{
		const size_t nl = text.find('\n');
		std::string p = pastedPath(text.substr(0, nl), home);
		if(!p.empty())
		{
			out.push_back(std::move(p));
		}
		text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
	}
	return out;
}

StartScreen::StartScreen(AppContext& ctx, Recent& recent, Actions actions)
    : m_ctx(ctx), m_recent(recent), m_actions(std::move(actions))
{
}

void StartScreen::show()
{
	m_items.clear();
	for(const RecentItem& item : m_recent.items())
	{
		if(std::ranges::any_of(item.m_paths, recentPathExists))
		{
			m_items.push_back(item);
		}
	}
	m_field.reset();
	m_sel = 0;
}

bool StartScreen::add(const std::string& path)
{
	if(path.empty() ||
	   std::ranges::any_of(m_pasted,
	                       [&](const Pasted& p) { return p.m_path == path; }))
	{
		return false;
	}
	std::error_code ec;
	std::vector<std::string> missing;
	const fs::path p(std::u8string(path.begin(), path.end()));
	const bool found =
	    fs::is_directory(p, ec) || !entriesForArgs({path}, missing).empty();
	m_pasted.push_back({path, found});
	return true;
}

void StartScreen::removePasted(int row)
{
	if(!onPasted(row))
	{
		return;
	}
	m_pasted.erase(m_pasted.begin() + (row - firstPasted()));
	m_sel = std::clamp(m_sel, 0, rows() - 1);
}

void StartScreen::paste(std::string_view text)
{
	const std::vector<std::string> paths = pastedPaths(text, paths::home());
	if(m_field && paths.size() <= 1 &&
	   text.find('\n') == std::string_view::npos)
	{
		// Into the field, to edit before Enter adds it.
		m_field->setText(m_field->text() + (paths.empty() ? "" : paths[0]));
		return;
	}
	int added = 0;
	for(const std::string& p : paths)
	{
		added += add(p) ? 1 : 0;
	}
	if(added > 0)
	{
		m_sel = openPastedRow(); // Enter opens them
	}
	m_ctx.m_message = added == 0   ? "nothing new to add"
	                  : added == 1 ? "added 1 path"
	                               : fmt::format("added {} paths", added);
}

void StartScreen::activate(int row)
{
	if(row == OPEN_FILES || row == OPEN_FOLDER)
	{
		if(m_actions.m_dialog)
		{
			m_actions.m_dialog(row == OPEN_FOLDER);
		}
	}
	else if(row == BROWSE)
	{
		if(m_actions.m_browse)
		{
			m_actions.m_browse();
		}
	}
	else if(row == TUTORIAL)
	{
		if(m_actions.m_tutorial)
		{
			m_actions.m_tutorial();
		}
	}
	else if(row == PASTE)
	{
		m_field.emplace();
		m_sel = PASTE;
	}
	else if(onPasted(row))
	{
		m_field.emplace(
		    m_pasted[static_cast<size_t>(row - firstPasted())].m_path);
		removePasted(row); // edited: Enter adds it back
		m_sel = PASTE;
	}
	else if(row == openPastedRow() && !m_pasted.empty())
	{
		std::vector<std::string> paths;
		for(const Pasted& p : m_pasted)
		{
			if(p.m_found)
			{
				paths.push_back(p.m_path);
			}
		}
		if(paths.empty())
		{
			m_ctx.m_message = "none of them was found";
		}
		else if(m_actions.m_open)
		{
			m_actions.m_open(paths);
		}
	}
	else if(row >= firstRecent() && row < rows() && m_actions.m_open)
	{
		// A copy: opening it puts it first in the list.
		const RecentItem item =
		    m_items[static_cast<size_t>(row - firstRecent())];
		m_actions.m_open(item.m_paths);
	}
}

int StartScreen::rowAt(int x, int y) const
{
	for(size_t i = 0; i < m_boxes.size(); ++i)
	{
		const Box& b = m_boxes[i];
		if(x >= b.x_min && x <= b.x_max && y >= b.y_min && y <= b.y_max)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool StartScreen::fieldEvent(const Event& e)
{
	switch(m_field->event(e, std::span<const std::string>{}))
	{
		case LineEditor::Result::COMMIT:
			// Added, and the field stays for the next one.
			if(add(pastedPath(m_field->text(), paths::home())))
			{
				m_ctx.m_message.clear();
			}
			m_field->setText("");
			return true;
		case LineEditor::Result::CANCEL:
			m_field.reset();
			return true;
		case LineEditor::Result::EDITED:
			return true;
		case LineEditor::Result::IGNORED:
			break;
	}
	// Down / Tab: out of the field, onto the list.
	if(e == Event::ArrowDown || e == Event::Tab)
	{
		m_field.reset();
		m_sel = std::min(rows() - 1, PASTE + 1);
	}
	return true; // typing: nothing else gets the keys
}

bool StartScreen::event(Event e)
{
	if(e.is_mouse())
	{
		const Mouse& m = e.mouse();
		const bool press =
		    m.button == Mouse::Left && m.motion == Mouse::Pressed;
		// The × of a pasted row.
		for(size_t i = 0; i < m_remove.size(); ++i)
		{
			const Box& b = m_remove[i];
			if(press && m.x >= b.x_min && m.x <= b.x_max && m.y >= b.y_min &&
			   m.y <= b.y_max)
			{
				removePasted(firstPasted() + static_cast<int>(i));
				return true;
			}
		}
		const int r = rowAt(m.x, m.y);
		if(r < 0)
		{
			if(press)
			{
				m_field.reset(); // a click elsewhere leaves the field
			}
			return false;
		}
		if(m_field && r != PASTE && !press)
		{
			return true; // typing: hovering doesn't move off the field
		}
		m_sel = r;
		if(press)
		{
			if(r != PASTE)
			{
				m_field.reset();
			}
			if(r != PASTE || !m_field)
			{
				activate(r);
			}
		}
		return true;
	}
	if(m_field)
	{
		return fieldEvent(e);
	}
	auto ch = [&e](char c) { return e == Event::Character(c); };
	if(ch('o'))
	{
		activate(OPEN_FILES);
	}
	else if(ch('f') || ch('O'))
	{
		activate(OPEN_FOLDER);
	}
	else if(ch('b'))
	{
		activate(BROWSE);
	}
	else if(ch('t'))
	{
		activate(TUTORIAL);
	}
	else if(ch('a') || ch('p'))
	{
		activate(PASTE);
	}
	else if(e.is_character() && e.character().size() == 1 &&
	        e.character()[0] >= '1' && e.character()[0] <= '9')
	{
		const int i = e.character()[0] - '1';
		if(i < static_cast<int>(m_items.size()))
		{
			activate(firstRecent() + i);
		}
	}
	else if(ch('j') || e == Event::ArrowDown || e == Event::Tab)
	{
		m_sel = (m_sel + 1) % rows();
	}
	else if(ch('k') || e == Event::ArrowUp || e == Event::TabReverse)
	{
		m_sel = (m_sel + rows() - 1) % rows();
	}
	else if(ch('g') || e == Event::Home)
	{
		m_sel = 0;
	}
	else if(ch('G') || e == Event::End)
	{
		m_sel = rows() - 1;
	}
	else if(e == Event::Return || ch('l') || e == Event::ArrowRight)
	{
		activate(m_sel);
	}
	else if((ch('x') || e == Event::Delete) && onPasted(m_sel))
	{
		removePasted(m_sel);
	}
	else if((ch('x') || e == Event::Delete) && m_sel >= firstRecent())
	{
		// Forget it.
		const RecentItem& item =
		    m_items[static_cast<size_t>(m_sel - firstRecent())];
		const auto& all = m_recent.items();
		if(const auto it = std::ranges::find(all, item); it != all.end())
		{
			m_recent.remove(static_cast<size_t>(it - all.begin()));
		}
		const int sel = m_sel;
		std::vector<Pasted> keep = std::move(m_pasted);
		show();
		m_pasted = std::move(keep);
		m_sel = std::min(sel, rows() - 1);
	}
	else if(ch('X') && !m_pasted.empty())
	{
		m_pasted.clear();
		m_sel = std::min(m_sel, rows() - 1);
	}
	else if(ch('q'))
	{
		m_ctx.m_quit();
	}
	else
	{
		return false;
	}
	return true;
}

Element StartScreen::render(const fs::path& browseDir)
{
	const Dimensions screen = ui::screenSize();
	const int width = std::clamp(screen.dimx - 4, 20, MAX_WIDTH);
	const fs::path home = paths::home();
	m_boxes.assign(static_cast<size_t>(rows()), Box{});
	m_remove.assign(m_pasted.size(), Box{});
	const bool hilite = !m_field; // typing: only the field stands out

	// A row: its cells, and on the selected one its keys at the right
	// (dim); the others show none, to keep the screen quiet.
	auto row = [&](int i, Elements cells, std::string_view keys = {})
	{
		const bool sel = i == m_sel && (hilite || i == PASTE);
		if(sel && !keys.empty())
		{
			cells.push_back(filler());
			cells.push_back(text(std::string(keys)) | dim);
			cells.push_back(text("  "));
		}
		Element r = hbox(std::move(cells));
		if(sel)
		{
			r = r | bgcolor(Color::GrayDark);
		}
		return r | reflect(m_boxes[static_cast<size_t>(i)]);
	};
	auto button = [&](int i, std::string_view label, std::string_view keys)
	{ return row(i, {text("  "), text(std::string(label)) | bold}, keys); };
	// A path's name and folder in columns, as the lists show them.
	constexpr int TAIL_W = 12;
	const int nameW = std::clamp((width - 8 - TAIL_W) / 2, 8, 40);
	const int whereW = std::max(4, width - 8 - TAIL_W - nameW - 2);
	// `indent` more cells before the name come off its width: the folders
	// line up whatever the list.
	auto pathCells =
	    [&](const RecentLabel& label, Decorator nameStyle, int indent = 0)
	{
		const int w = std::max(4, nameW - indent);
		return Elements{text(std::string(2 + static_cast<size_t>(indent), ' ')),
		                text(fit(ui::ellipsizeMiddle(label.m_name, w), w)) |
		                    nameStyle,
		                text("  "),
		                text(fit(ui::ellipsizeStart(label.m_where, whereW),
		                         whereW)) |
		                    dim};
	};
	auto rightAligned = [](const std::string& s, int w)
	{
		return std::string(static_cast<size_t>(
		                       std::max(0, w - string_width(s))),
		                   ' ') +
		       s;
	};
	auto nameStyle = [](const RecentLabel& l)
	{
		return l.m_name.ends_with('/') ? color(Color::Blue)
		                               : color(Color::Green);
	};

	Elements lines;
	lines.push_back(button(OPEN_FILES, "Open files…", "o  Ctrl+O"));
	lines.push_back(button(OPEN_FOLDER, "Open a folder…", "f  Ctrl+Shift+O"));
	lines.push_back(button(BROWSE,
	                       browseDir.empty()
	                           ? std::string("Browse")
	                           : "Browse " + tildePath(browseDir, home),
	                       "b"));
	lines.push_back(button(TUTORIAL, "Tutorial", "t"));
	lines.push_back(text(""));

	// Paste: a row like the others, a box to type in only while typing.
	if(m_field)
	{
		const int fieldW = std::max(8, width - 2 - 7 - 2);
		const auto& g = m_field->glyphs();
		const int cur = m_field->cursor();
		std::string before, at = " ", after;
		// A long path scrolls: the cursor stays in the box.
		const int from = std::max(0, cur - (fieldW - 2));
		for(int k = from; k < static_cast<int>(g.size()); ++k)
		{
			(k < cur ? before : k == cur ? at : after) += g[k];
		}
		lines.push_back(
		    row(PASTE,
		        {text("  "),
		         text("Path  ") | bold,
		         hbox({text(before), text(at) | inverted, text(after)}) |
		             size(WIDTH, EQUAL, fieldW)}));
	}
	else
	{
		lines.push_back(button(PASTE, "Paste paths…", "Ctrl+V  a type"));
	}
	for(size_t n = 0; n < m_pasted.size(); ++n)
	{
		const Pasted& p = m_pasted[n];
		const RecentLabel label = recentLabel({{p.m_path}, 0}, home);
		Elements cells =
		    pathCells(label,
		              p.m_found ? nameStyle(label) : color(Color::Red),
		              2); // under "Paste paths…"
		cells.push_back(
		    text(rightAligned(p.m_found ? "" : "not found", TAIL_W - 2)) |
		    color(Color::Red));
		cells.push_back(text("  "));
		cells.push_back(text("×") | dim | reflect(m_remove[n]));
		lines.push_back(
		    row(firstPasted() + static_cast<int>(n), std::move(cells), ""));
	}
	if(!m_pasted.empty())
	{
		const int found = static_cast<int>(
		    std::ranges::count_if(m_pasted,
		                          [](const Pasted& p) { return p.m_found; }));
		lines.push_back(
		    row(openPastedRow(),
		        {text(std::string(2 + static_cast<size_t>(nameW) + 2, ' ')),
		         text(fmt::format("Open ({})", found)) | bold |
		             color(Color::Cyan)},
		        "Enter  X clear"));
	}

	if(!m_items.empty())
	{
		lines.push_back(text(""));
		lines.push_back(text("  Recent") | dim);
	}
	const std::int64_t now = nowSeconds();
	for(size_t n = 0; n < m_items.size(); ++n)
	{
		const RecentItem& item = m_items[n];
		const RecentLabel label = recentLabel(item, home);
		Elements cells = pathCells(label, nameStyle(label));
		cells.push_back(
		    text(rightAligned(agoText(
		                          std::max<std::int64_t>(0, now - item.m_when)),
		                      TAIL_W)) |
		    dim);
		cells.push_back(text("  "));
		lines.push_back(
		    row(firstRecent() + static_cast<int>(n), std::move(cells)));
	}

	Element body =
	    vbox(std::move(lines)) | size(WIDTH, EQUAL, width) | center | flex;
	return vbox(
	    {body,
	     m_ctx.statusLine(m_field ? "enter add  esc done"
	                              : "drop files anywhere  ? keys  q quit")});
}

} // namespace rega
