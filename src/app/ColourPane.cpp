#include "app/ColourPane.h"

#include "util/Fuzzy.h"
#include "util/Ui.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rega
{

namespace
{

constexpr const char* ROW_NAMES[ColourPane::ROWS] = {"config",
                                                     "display",
                                                     "view",
                                                     "look",
                                                     "input"};
constexpr int NAME_W = 9;       ///< " display " and the like
constexpr int PICKER_ROWS = 12; ///< matches shown at once

// `~/x` → $HOME/x (typed config paths).
fs::path expandHome(const std::string& s)
{
	if(s.starts_with("~/"))
	{
		if(const char* home = std::getenv("HOME"))
		{
			return fs::path(home) / s.substr(2);
		}
	}
	return s;
}

} // namespace

ColourPane::ColourPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx)
{
}

void ColourPane::show()
{
	m_open = true;
	m_state.m_focus = Focus::COLOUR;
}

std::vector<std::string> ColourPane::values(Row r,
                                            const fs::path& path,
                                            const std::string& sourceKey,
                                            int& current) const
{
	const ColourManager& cm = m_ctx.m_colour;
	std::vector<std::string> out;
	current = -1;
	auto pick = [&](std::vector<std::string> all, const std::string& now)
	{
		const auto it = std::ranges::find(all, now);
		current = it == all.end() ? -1 : static_cast<int>(it - all.begin());
		return all;
	};
	if(r == Row::CONFIG)
	{
		const auto choices = cm.configChoices();
		for(size_t i = 0; i < choices.size(); ++i)
		{
			out.push_back(choices[i].m_label);
			if(choices[i].m_id == cm.configId())
			{
				current = static_cast<int>(i);
			}
		}
		return out;
	}
	if(!cm.active())
	{
		return out;
	}
	switch(r)
	{
		case Row::DISPLAY:
			return pick(cm.displays(), cm.display());
		case Row::VIEW:
			return pick(cm.views(), cm.view());
		case Row::LOOK:
			return pick(cm.looks(), cm.look());
		case Row::INPUT:
		{
			// The file rules' answer first, then every colour space.
			out = cm.colourSpaces();
			const auto it =
			    std::ranges::find(out, cm.inputFor(path, sourceKey));
			const int at =
			    it == out.end() ? -1 : static_cast<int>(it - out.begin()) + 1;
			out.insert(out.begin(), "file rule: " + cm.fileRuleFor(path));
			current = cm.overridden(sourceKey) ? at : 0;
			return out;
		}
		case Row::CONFIG:
			break;
	}
	return out;
}

void ColourPane::choose(Row r,
                        int index,
                        const fs::path& path,
                        const std::string& sourceKey)
{
	ColourManager& cm = m_ctx.m_colour;
	int current = -1;
	const auto all = values(r, path, sourceKey, current);
	if(index < 0 || index >= static_cast<int>(all.size()))
	{
		return;
	}
	switch(r)
	{
		case Row::CONFIG:
		{
			const auto choices = cm.configChoices();
			if(!cm.useConfig(choices[index].m_id))
			{
				m_ctx.m_message = "OCIO: " + cm.error();
			}
			break;
		}
		case Row::DISPLAY:
			cm.setDisplay(all[index]);
			break;
		case Row::VIEW:
			cm.setView(all[index]);
			break;
		case Row::LOOK:
			cm.setLook(all[index]);
			break;
		case Row::INPUT:
			cm.setInput(sourceKey,
			            index == 0 ? std::nullopt : std::optional(all[index]));
			break;
	}
}

std::vector<int> ColourPane::matches(const std::vector<std::string>& all) const
{
	std::vector<int> out;
	for(size_t i = 0; i < all.size(); ++i)
	{
		if(fuzzyMatch(all[i], m_picker->m_filter))
		{
			out.push_back(static_cast<int>(i));
		}
	}
	return out;
}

Element ColourPane::render(const fs::path& path, const std::string& sourceKey)
{
	const ColourManager& cm = m_ctx.m_colour;
	const bool focus = focused();
	const int width = m_state.leftPanelWidth();
	const int room = std::max(1, width - NAME_W);
	Elements lines;
	for(int r = 0; r < ROWS; ++r)
	{
		const Row row = static_cast<Row>(r);
		std::string value;
		bool off = false;
		switch(row)
		{
			case Row::CONFIG:
				value = cm.configLabel();
				break;
			case Row::DISPLAY:
				value = cm.display();
				break;
			case Row::VIEW:
				value = cm.view();
				break;
			case Row::LOOK:
				value = cm.look();
				break;
			case Row::INPUT:
				value = cm.inputFor(path, sourceKey) +
				        (cm.overridden(sourceKey) ? "" : " (file rule)");
				break;
		}
		if(row != Row::CONFIG && !cm.active())
		{
			value = "—";
			off = true;
		}
		Element line = hbox({
		    text(fmt::format(" {:<{}}", ROW_NAMES[r], NAME_W - 1)) |
		        color(Color::Yellow),
		    text(ui::ellipsizeEnd(value, room)) | (off ? dim : nothing),
		});
		if(focus && r == m_cursor)
		{
			line = line | inverted;
		}
		lines.push_back(line);
	}
	if(m_picker)
	{
		// The list: the filter as typed, then the matches around the
		// selection.
		int current = -1;
		const auto all = values(m_picker->m_row, path, sourceKey, current);
		const auto hits = matches(all);
		m_picker->m_selected =
		    std::clamp(m_picker->m_selected,
		               0,
		               std::max(0, static_cast<int>(hits.size()) - 1));
		lines.push_back(separatorLight());
		lines.push_back(hbox({text(" › ") | color(Color::Yellow),
		                      text(m_picker->m_filter),
		                      text(" ") | inverted}));
		if(m_picker->m_row == Row::CONFIG && !m_picker->m_filter.empty())
		{
			std::error_code ec;
			if(fs::is_regular_file(expandHome(m_picker->m_filter), ec))
			{
				lines.push_back(text(" ↵ load this file") | dim);
			}
		}
		const int first = std::max(0, m_picker->m_selected - PICKER_ROWS + 1);
		for(int k = first;
		    k < static_cast<int>(hits.size()) && k < first + PICKER_ROWS;
		    ++k)
		{
			const std::string& name = all[hits[k]];
			const auto pos = fuzzyMatch(name, m_picker->m_filter);
			Element el = hbox({
			    text(hits[k] == current ? " • " : "   "),
			    ui::highlighted(ui::ellipsizeEnd(name, width - 3),
			                    pos ? *pos : std::vector<size_t>{},
			                    nothing),
			});
			lines.push_back(k == m_picker->m_selected ? el | inverted : el);
		}
		if(hits.empty())
		{
			lines.push_back(text("   no match") | dim);
		}
	}
	Element footer =
	    m_picker
	        ? ui::paneHints("",
	                        {{"↑↓", "move"}, {"Enter", "pick"}, {"Esc", ""}})
	    : focus ? ui::paneHints("",
	                            {{"h/l", "change"},
	                             {"Enter", "list"},
	                             {"s", "raw"},
	                             {"6", "close"}})
	            : ui::paneHints("", {{"6", "focus"}});
	const std::string title = ui::paneTitle("─[6]─Colour", width);
	return vbox({
	           text(title) | (focus ? color(Color::Green) | bold : dim),
	           vbox(std::move(lines)),
	           footer,
	       }) |
	       reflect(m_box);
}

bool ColourPane::pickerEvent(const Event& e,
                             const fs::path& path,
                             const std::string& sourceKey)
{
	Picker& p = *m_picker;
	if(e == Event::Escape)
	{
		m_picker.reset();
	}
	else if(e == Event::Return)
	{
		int current = -1;
		const auto all = values(p.m_row, path, sourceKey, current);
		const auto hits = matches(all);
		std::error_code ec;
		const fs::path typed = expandHome(p.m_filter);
		if(p.m_row == Row::CONFIG && !p.m_filter.empty() &&
		   fs::is_regular_file(typed, ec))
		{
			if(!m_ctx.m_colour.useConfig(typed.string()))
			{
				m_ctx.m_message = "OCIO: " + m_ctx.m_colour.error();
			}
		}
		else if(!hits.empty())
		{
			choose(p.m_row,
			       hits[std::clamp(
			           p.m_selected, 0, static_cast<int>(hits.size()) - 1)],
			       path,
			       sourceKey);
		}
		m_picker.reset();
	}
	else if(e == Event::ArrowDown || e == Event::CtrlN || e == Event::Tab)
	{
		++p.m_selected; // clamped when drawn
	}
	else if(e == Event::ArrowUp || e == Event::CtrlP || e == Event::TabReverse)
	{
		p.m_selected = std::max(0, p.m_selected - 1);
	}
	else if(e == Event::Backspace)
	{
		// One UTF-8 character back.
		while(!p.m_filter.empty() &&
		      (static_cast<unsigned char>(p.m_filter.back()) & 0xC0) == 0x80)
		{
			p.m_filter.pop_back();
		}
		if(!p.m_filter.empty())
		{
			p.m_filter.pop_back();
		}
		p.m_selected = 0;
	}
	else if(e.is_character())
	{
		p.m_filter += e.character();
		p.m_selected = 0;
	}
	return true; // the list has the keys while open
}

bool ColourPane::event(const Event& e,
                       const fs::path& path,
                       const std::string& sourceKey)
{
	if(m_picker)
	{
		return pickerEvent(e, path, sourceKey);
	}
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	const Row row = static_cast<Row>(m_cursor);
	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(ROWS - 1, m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(ch("h") || ch("l") || e == Event::ArrowLeft ||
	        e == Event::ArrowRight)
	{
		int current = -1;
		const auto all = values(row, path, sourceKey, current);
		if(!all.empty())
		{
			const int n = static_cast<int>(all.size());
			const int step = ch("l") || e == Event::ArrowRight ? 1 : -1;
			choose(row,
			       ((std::max(0, current) + step) % n + n) % n,
			       path,
			       sourceKey);
		}
	}
	else if(e == Event::Return)
	{
		int current = -1;
		if(!values(row, path, sourceKey, current).empty())
		{
			m_picker = Picker{row, "", std::max(0, current)};
		}
	}
	else if(e == Event::Escape || ch("q") || ch("1") || e == Event::CtrlH)
	{
		m_state.m_focus = Focus::IMAGE;
	}
	else if(ch("6"))
	{
		m_open = false; // focused: its number closes it
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false; // s, exposure… still reach the viewer
	}
	return true;
}

} // namespace rega
