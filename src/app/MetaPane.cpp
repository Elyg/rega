#include "app/MetaPane.h"

#include "util/Clipboard.h"
#include "util/Fuzzy.h"
#include "util/Ui.h"

#include <ftxui/component/event.hpp>
#include <ftxui/screen/string.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

MetaPane::MetaPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx)
{
}

bool MetaPane::onDiff() const
{
	return !m_state.m_sources.empty() &&
	       m_state.m_sources[m_state.m_current].m_diffA.has_value();
}

std::vector<MetaPane::Item> MetaPane::items(const ImageInfoPtr& info) const
{
	using K = Item::Kind;
	std::vector<Item> items;
	if(!info)
	{
		return items;
	}
	if(onDiff())
	{
		const Source& src = m_state.m_sources[m_state.m_current];
		return diffItems(m_ctx.m_svc.info(src.diffFrame(m_state.m_frame)),
		                 info);
	}
	items.push_back(
	    {K::TITLE, m_state.currentFramePath().filename().string(), ""});
	items.push_back({K::PATH, "path", m_state.currentFramePath().string()});
	for(size_t p = 0; p < info->m_parts.size(); ++p)
	{
		const auto& part = info->m_parts[p];
		items.push_back({K::PART, fmt::format("part {}", p), part.m_name});
		for(const auto& a : part.m_attributes)
		{
			items.push_back({K::ATTRIBUTE, a.m_name, a.m_value});
		}
	}
	items.push_back({K::LAYERS, "layers", ""});
	for(const auto& l : info->m_layers)
	{
		std::string chans;
		for(const auto& c : l.m_channels)
		{
			chans += (chans.empty() ? "" : " ") + LayerInfo::shortName(c);
		}
		items.push_back({K::LAYER, l.label(), chans});
	}
	return items;
}

std::vector<MetaPane::Item> MetaPane::diffItems(const ImageInfoPtr& a,
                                                const ImageInfoPtr& b) const
{
	using K = Item::Kind;
	using D = Item::Diff;
	const Source& src = m_state.m_sources[m_state.m_current];
	std::vector<Item> items;
	items.push_back({K::TITLE, "header diff", ""});
	const size_t summary = items.size();
	items.push_back({K::SUMMARY, "", ""});
	items.push_back({K::PATH, "A", src.diffFrame(m_state.m_frame).string()});
	items.push_back({K::PATH, "B", m_state.currentFramePath().string()});
	if(!a)
	{
		items[summary].m_name = "reading A…";
		return items;
	}
	int differ = 0;
	// (name, value) rows of both sides, matched by name: B's order, then
	// what only A has. A heading goes in once it has a row under it.
	using Rows = std::vector<std::pair<std::string, std::string>>;
	auto merge = [&](Item heading, const Rows& ra, const Rows& rb, K kind)
	{
		std::vector<Item> rows;
		auto find = [](const Rows& r, const std::string& n)
		{
			return std::ranges::find_if(r,
			                            [&](const auto& x)
			                            { return x.first == n; });
		};
		for(const auto& [name, vb] : rb)
		{
			const auto ia = find(ra, name);
			if(ia == ra.end())
			{
				rows.push_back({kind, name, "", D::ONLY_B, vb});
			}
			else if(ia->second != vb)
			{
				rows.push_back({kind, name, ia->second, D::CHANGED, vb});
			}
			else if(m_diffAll)
			{
				rows.push_back({kind, name, vb, D::SAME, ""});
			}
		}
		for(const auto& [name, va] : ra)
		{
			if(find(rb, name) == rb.end())
			{
				rows.push_back({kind, name, va, D::ONLY_A, ""});
			}
		}
		differ += static_cast<int>(
		    std::ranges::count_if(rows,
		                          [](const Item& it)
		                          { return it.m_diff != D::SAME; }));
		if(!rows.empty() || m_diffAll)
		{
			items.push_back(std::move(heading));
			std::ranges::move(rows, std::back_inserter(items));
		}
	};
	auto attributes = [](const ImageInfoPtr& info, size_t p)
	{
		Rows r;
		if(p < info->m_parts.size())
		{
			for(const auto& at : info->m_parts[p].m_attributes)
			{
				r.emplace_back(at.m_name, at.m_value);
			}
		}
		return r;
	};
	for(size_t p = 0; p < std::max(a->m_parts.size(), b->m_parts.size()); ++p)
	{
		const auto& named =
		    p < b->m_parts.size() ? b->m_parts[p] : a->m_parts[p];
		merge({K::PART, fmt::format("part {}", p), named.m_name},
		      attributes(a, p),
		      attributes(b, p),
		      K::ATTRIBUTE);
	}
	auto layers = [](const ImageInfoPtr& info)
	{
		Rows r;
		for(const auto& l : info->m_layers)
		{
			std::string chans;
			for(const auto& c : l.m_channels)
			{
				chans += (chans.empty() ? "" : " ") + LayerInfo::shortName(c);
			}
			r.emplace_back(l.label(), chans);
		}
		return r;
	};
	merge({K::LAYERS, "layers", ""}, layers(a), layers(b), K::LAYER);
	items[summary].m_name =
	    differ == 0
	        ? "headers identical"
	        : fmt::format("{} difference{}", differ, differ == 1 ? "" : "s");
	items[summary].m_diff = differ == 0 ? D::SAME : D::CHANGED;
	return items;
}

namespace
{

bool containsNoCase(const std::string& s, const std::string& q)
{
	return std::ranges::search(s,
	                           q,
	                           [](unsigned char a, unsigned char b)
	                           { return std::tolower(a) == std::tolower(b); })
	               .begin() != s.end() ||
	       q.empty();
}

} // namespace

std::vector<MetaPane::Item> MetaPane::shown(const ImageInfoPtr& info) const
{
	using K = Item::Kind;
	std::vector<Item> all = items(info);
	if(m_filter.empty())
	{
		return all;
	}
	auto hit = [&](const Item& it)
	{
		return fuzzyMatch(it.m_name, m_filter).has_value() ||
		       containsNoCase(it.m_value, m_filter) ||
		       (!it.m_valueB.empty() && containsNoCase(it.m_valueB, m_filter));
	};
	// The title always; a heading once a row under it matches (or itself).
	std::vector<Item> out;
	std::optional<Item> heading;
	for(Item& it : all)
	{
		if(it.m_kind == K::TITLE || it.m_kind == K::SUMMARY)
		{
			out.push_back(std::move(it));
		}
		else if(it.m_kind == K::PART || it.m_kind == K::LAYERS)
		{
			heading.reset();
			if(hit(it))
			{
				out.push_back(std::move(it));
			}
			else
			{
				heading = std::move(it);
			}
		}
		else if(hit(it))
		{
			if(heading)
			{
				out.push_back(std::move(*heading));
				heading.reset();
			}
			out.push_back(std::move(it));
		}
	}
	return out;
}

int MetaPane::nameColumn(const std::vector<Item>& all) const
{
	// The longest name plus a one-cell gap, but never more than ~45% of the
	// pane: values need the room more.
	int longest = 0;
	for(const Item& it : all)
	{
		if(it.m_kind == Item::Kind::ATTRIBUTE || it.m_kind == Item::Kind::LAYER)
		{
			longest = std::max(longest, string_width(it.m_name));
		}
	}
	const int cap =
	    std::max(8, m_state.sidePanelWidth() * 45 / 100 - NAME_INDENT);
	return std::min(longest + 1, cap);
}

Elements MetaPane::itemLines(const Item& it, int nameCol, bool expand) const
{
	using K = Item::Kind;
	const int width = m_state.sidePanelWidth();
	// Whole lines, cut to the pane with "…", or (expand: the cursor row)
	// wrapped onto as many lines as they take.
	auto plain = [&](const std::string& s, int indent, Decorator style)
	{
		Elements out;
		const int room = std::max(1, width - indent);
		const std::string pad(static_cast<size_t>(indent), ' ');
		if(!expand)
		{
			out.push_back(text(pad + ui::ellipsizeEnd(s, room)) | style);
			return out;
		}
		for(const auto& l : ui::wrapWidth(s, room))
		{
			out.push_back(text(pad + l) | style);
		}
		return out;
	};
	switch(it.m_kind)
	{
		case K::TITLE:
			return plain(it.m_name, 1, bold);
		case K::SUMMARY:
			return plain(it.m_name,
			             1,
			             color(it.m_diff == Item::Diff::SAME ? Color::Green
			                                                 : Color::Yellow));
		case K::PATH:
		{
			// A diff's two: "A /…", "B /…".
			const std::string side = it.m_name == "path" ? "" : it.m_name + " ";
			if(expand)
			{
				return plain(side + it.m_value, 1, dim);
			}
			// Keep the tail (the interesting end) when it does not fit.
			return {text(" " + side +
			             ui::ellipsizeStart(it.m_value,
			                                std::max(8,
			                                         width - 2 -
			                                             static_cast<int>(
			                                                 side.size())))) |
			        dim};
		}
		case K::PART:
			return plain(it.m_name +
			                 (it.m_value.empty() ? "" : " · " + it.m_value),
			             1,
			             bold | color(Color::Cyan));
		case K::LAYERS:
			return {text(" layers") | bold | color(Color::Cyan)};
		case K::ATTRIBUTE:
		case K::LAYER:
			break;
	}
	// name  value, in two columns.
	using D = Item::Diff;
	const bool attr = it.m_kind == K::ATTRIBUTE;
	const bool differs = it.m_diff != D::NONE && it.m_diff != D::SAME;
	const Decorator nameStyle =
	    it.m_diff == D::SAME ? dim
	    : attr               ? color(Color::Yellow)
	           : (it.m_name == m_state.m_layerLabel ? bold : nothing);
	const Decorator valueStyle = attr ? nothing : dim;
	const int valueCol = NAME_INDENT + nameCol;
	const int valueRoom = std::max(1, width - valueCol);
	const std::string indent(NAME_INDENT, ' ');
	auto nameCell = [&](const std::string& name)
	{ return text(indent + name) | nameStyle | size(WIDTH, EQUAL, valueCol); };
	if(differs)
	{
		// A's value → B's, red → green; "—" for the side without it.
		const std::string va = it.m_diff == D::ONLY_B ? "—" : it.m_value;
		const std::string vb = it.m_diff == D::ONLY_A ? "—" : it.m_valueB;
		const Decorator red = color(Color::Red), green = color(Color::Green);
		if(!expand)
		{
			const int half = std::max(1, (valueRoom - 3) / 2);
			return {hbox({
			    nameCell(ui::ellipsizeMiddle(it.m_name, nameCol - 1)),
			    text(ui::ellipsizeEnd(va, half)) | red,
			    text(" → ") | dim,
			    text(ui::ellipsizeEnd(vb, valueRoom - 3 - half)) | green,
			})};
		}
		// Expanded: the whole name, then each side wrapped on its own.
		Elements out;
		for(const auto& l : ui::wrapWidth(it.m_name, width - NAME_INDENT))
		{
			out.push_back(text(indent + l) | nameStyle);
		}
		auto side = [&](const char* tag, const std::string& v, Decorator style)
		{
			const auto lines = ui::wrapWidth(v, std::max(1, width - 6));
			for(size_t i = 0; i < lines.size(); ++i)
			{
				out.push_back(hbox({text(indent + "  "),
				                    text(i == 0 ? tag : "  ") | dim,
				                    text(lines[i]) | style}));
			}
		};
		side("A ", va, red);
		side("B ", vb, green);
		return out;
	}
	if(!expand)
	{
		return {hbox({
		    nameCell(ui::ellipsizeMiddle(it.m_name, nameCol - 1)),
		    text(ui::ellipsizeEnd(it.m_value, valueRoom)) | valueStyle,
		})};
	}
	Elements out;
	const bool nameFits = string_width(it.m_name) <= nameCol - 1;
	if(!nameFits)
	{
		// The whole name first, on lines of its own.
		for(const auto& l : ui::wrapWidth(it.m_name, width - NAME_INDENT))
		{
			out.push_back(text(indent + l) | nameStyle);
		}
	}
	const auto values = ui::wrapWidth(it.m_value, valueRoom);
	for(size_t v = 0; v < values.size(); ++v)
	{
		out.push_back(hbox({
		    nameCell(v == 0 && nameFits ? it.m_name : ""),
		    text(values[v]) | valueStyle,
		}));
	}
	return out;
}

Element MetaPane::render(const ImageInfoPtr& info)
{
	const auto all = shown(info);
	const int total = static_cast<int>(all.size());
	const bool focus = focused();
	m_cursor = std::clamp(m_cursor, 0, std::max(0, total - 1));
	const int nameCol = nameColumn(all);
	// Focused, the cursor row shows its whole name and value.
	Elements cursorLines;
	if(focus && total > 0)
	{
		cursorLines = itemLines(all[m_cursor], nameCol, true);
	}
	const int extra = std::max(0, static_cast<int>(cursorLines.size()) - 1);

	// Keep the cursor (all of its lines) on screen: rows from the last
	// layout, minus the title and footer.
	const int rows = std::max(1, m_box.y_max - m_box.y_min - 1);
	if(focus)
	{
		if(m_cursor < m_scroll)
		{
			m_scroll = m_cursor;
		}
		else if(m_cursor + extra >= m_scroll + rows)
		{
			m_scroll = std::min(m_cursor, m_cursor + extra - rows + 1);
		}
	}
	m_scroll = std::clamp(m_scroll, 0, std::max(0, total - 1));

	const int selLo = m_anchor < 0 ? m_cursor : std::min(m_anchor, m_cursor);
	const int selHi = m_anchor < 0 ? m_cursor : std::max(m_anchor, m_cursor);
	Elements lines;
	m_lineItems.clear();
	for(int i = m_scroll; i < total; ++i)
	{
		Elements item = focus && i == m_cursor
		                    ? cursorLines
		                    : itemLines(all[i], nameCol, false);
		for(auto& line : item)
		{
			lines.push_back(focus && i >= selLo && i <= selHi ? line | inverted
			                                                  : line);
			m_lineItems.push_back(i);
		}
	}
	const std::string at =
	    fmt::format("{}/{}", (focus ? m_cursor : m_scroll) + 1, total);
	Element footer =
	    !focus     ? ui::paneHints(at, {{"2", "focus"}})
	    : onDiff() ? ui::paneHints(at,
	                               {{"d", m_diffAll ? "differences" : "all"},
	                                {"/", "filter"},
	                                {"y/Y", "copy"},
	                                {"2", "close"}})
	               : ui::paneHints(at,
	                               {{"/", "filter"},
	                                {"v", "select"},
	                                {"y/Y", "copy"},
	                                {"2", "close"}});
	if(m_filtering || !m_filter.empty())
	{
		// As the browser's: the filter, how many rows it lets through.
		const int matches = static_cast<int>(
		    std::ranges::count_if(all,
		                          [](const Item& it)
		                          {
			                          return it.m_kind ==
			                                     Item::Kind::ATTRIBUTE ||
			                                 it.m_kind == Item::Kind::LAYER ||
			                                 it.m_kind == Item::Kind::PATH;
		                          }));
		footer = hbox({
		    text(" / ") | bgcolor(Color::Yellow) | color(Color::Black) | bold,
		    text(" " + m_filter + (m_filtering ? "▏" : "") + " ") |
		        color(Color::Yellow) | bold,
		    text(fmt::format("{} match{}  {}",
		                     matches,
		                     matches == 1 ? "" : "es",
		                     m_filtering ? "Enter keep · Esc clear"
		                                 : "Esc clear")) |
		        dim,
		});
	}
	// lazygit-style title: `2` focuses this pane.
	const std::string title =
	    ui::paneTitle("─[2]─Metadata", m_state.sidePanelWidth());
	return vbox({
	           text(title) | (focus ? color(Color::Green) | bold : dim),
	           vbox(std::move(lines)) | flex,
	           footer,
	       }) |
	       reflect(m_box);
}

void MetaPane::copy(const ImageInfoPtr& info, bool valuesOnly)
{
	using K = Item::Kind;
	const auto all = shown(info);
	if(all.empty())
	{
		return;
	}
	int lo = m_anchor < 0 ? m_cursor : std::min(m_anchor, m_cursor);
	int hi = m_anchor < 0 ? m_cursor : std::max(m_anchor, m_cursor);
	hi = std::min(hi, static_cast<int>(all.size()) - 1);
	std::string out;
	for(int i = lo; i <= hi; ++i)
	{
		const Item& it = all[i];
		std::string line;
		if(valuesOnly)
		{
			line = it.m_value.empty() ? it.m_name : it.m_value;
		}
		else if(it.m_diff == Item::Diff::CHANGED ||
		        it.m_diff == Item::Diff::ONLY_A ||
		        it.m_diff == Item::Diff::ONLY_B)
		{
			line =
			    fmt::format("{}: {} → {}",
			                it.m_name,
			                it.m_diff == Item::Diff::ONLY_B ? "—" : it.m_value,
			                it.m_diff == Item::Diff::ONLY_A ? "—"
			                                                : it.m_valueB);
		}
		else if(it.m_kind == K::ATTRIBUTE || it.m_kind == K::LAYER)
		{
			line = it.m_name + ": " + it.m_value;
		}
		else
		{
			line = it.m_name + (it.m_value.empty() ? "" : " " + it.m_value);
		}
		out += line + "\n";
	}
	if(hi == lo && !out.empty())
	{
		out.pop_back(); // a single line copys without the newline
	}
	int n = hi - lo + 1;
	if(!copyToClipboard(out, m_ctx.m_caps.m_tmux))
	{
		m_ctx.m_message = "copy failed";
	}
	else if(n == 1)
	{
		constexpr size_t MAX_SHOWN = 60;
		m_ctx.m_message =
		    "copied: " +
		    (out.size() > MAX_SHOWN ? out.substr(0, MAX_SHOWN - 1) + "…" : out);
	}
	else
	{
		m_ctx.m_message = fmt::format("copied {} lines", n);
	}
	m_anchor = -1;
}

void MetaPane::toFirstMatch(const ImageInfoPtr& info)
{
	const auto all = shown(info);
	const auto it =
	    std::ranges::find_if(all,
	                         [](const Item& r)
	                         { return r.m_kind != Item::Kind::TITLE; });
	m_cursor = it == all.end() ? 0 : static_cast<int>(it - all.begin());
	m_scroll = 0;
	m_anchor = -1;
}

bool MetaPane::filterEvent(const Event& e, const ImageInfoPtr& info)
{
	if(e.is_mouse())
	{
		return false; // the wheel, a click elsewhere: the viewer's
	}
	const int total = static_cast<int>(shown(info).size());
	if(e == Event::Escape)
	{
		m_filtering = false;
		m_filter.clear();
	}
	else if(e == Event::Return)
	{
		m_filtering = false;
		return true;
	}
	else if(e == Event::ArrowDown)
	{
		m_cursor = std::min(total - 1, m_cursor + 1);
		return true;
	}
	else if(e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
		return true;
	}
	else if(e == Event::Backspace)
	{
		// A whole UTF-8 character.
		while(!m_filter.empty() &&
		      (static_cast<unsigned char>(m_filter.back()) & 0xC0) == 0x80)
		{
			m_filter.pop_back();
		}
		if(!m_filter.empty())
		{
			m_filter.pop_back();
		}
	}
	else if(e.is_character())
	{
		m_filter += e.character();
	}
	else
	{
		return true; // typing: nothing else reaches the viewer
	}
	toFirstMatch(info);
	return true;
}

void MetaPane::click(int y, bool extend)
{
	const int line = y - m_box.y_min - 1; // under the title
	m_state.m_focus = Focus::META;
	if(line < 0 || line >= static_cast<int>(m_lineItems.size()))
	{
		return;
	}
	const int row = m_lineItems[line];
	if(extend)
	{
		if(m_anchor < 0)
		{
			m_anchor = m_cursor;
		}
	}
	else
	{
		m_anchor = -1;
	}
	m_cursor = row;
}

void MetaPane::dragTo(int y)
{
	if(m_lineItems.empty())
	{
		return;
	}
	const int line = std::clamp(y - m_box.y_min - 1,
	                            0,
	                            static_cast<int>(m_lineItems.size()) - 1);
	const int row = m_lineItems[line];
	if(m_anchor < 0 && row != m_cursor)
	{
		m_anchor = m_cursor; // the row the drag started on
	}
	m_cursor = row;
}

bool MetaPane::event(const Event& e, const ImageInfoPtr& info)
{
	if(m_filtering)
	{
		return filterEvent(e, info);
	}
	const int total = static_cast<int>(shown(info).size());
	const int half = std::max(1, (m_box.y_max - m_box.y_min) / 2);
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	bool wasG = m_pendingG;
	m_pendingG = false;

	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(total - 1, m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(e == Event::CtrlD || e == Event::PageDown)
	{
		m_cursor = std::min(total - 1, m_cursor + half);
	}
	else if(e == Event::CtrlU || e == Event::PageUp)
	{
		m_cursor = std::max(0, m_cursor - half);
	}
	else if(ch("g"))
	{
		if(wasG)
		{
			m_cursor = 0;
		}
		else
		{
			m_pendingG = true;
		}
	}
	else if(ch("G"))
	{
		m_cursor = std::max(0, total - 1);
	}
	else if(ch("/"))
	{
		m_filtering = true;
		m_filter.clear();
		toFirstMatch(info);
	}
	else if(ch("v") || ch("V"))
	{
		m_anchor = m_anchor < 0 ? m_cursor : -1;
	}
	else if(ch("d") && onDiff())
	{
		m_diffAll = !m_diffAll; // the differences ⇄ both headers whole
		m_anchor = -1;
	}
	else if(ch("y"))
	{
		copy(info, false);
	}
	else if(ch("Y"))
	{
		copy(info, true);
	}
	else if(e == Event::Escape)
	{
		if(m_anchor >= 0)
		{
			m_anchor = -1;
		}
		else if(!m_filter.empty())
		{
			m_filter.clear();
		}
		else
		{
			m_state.m_focus = Focus::IMAGE;
		}
	}
	else if(e == Event::CtrlH || ch("q") || ch("1"))
	{
		m_anchor = -1;
		m_state.m_focus = Focus::IMAGE;
	}
	else if(e == Event::CtrlL)
	{
		m_ctx.tmuxSelectPane('R');
	}
	else if(ch("2"))
	{
		m_anchor = -1; // focused: its number closes it
		m_open = false;
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false; // e.g. exposure keys still reach the viewer
	}
	return true;
}

} // namespace rv
