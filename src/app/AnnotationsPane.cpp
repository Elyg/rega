#include "app/AnnotationsPane.h"

#include "util/Ui.h"

#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>

using namespace ftxui;

namespace rega
{

namespace
{

// `[#@key]` placeholders, always available.
const std::string BUILTINS[] = {"file", "frame", "layer", "res", "fps", "date"};

// Pane and Tab order: the bottom row first (where `T` starts), then the top.
constexpr Slot SLOT_ORDER[SLOT_COUNT] =
    {Slot::BL, Slot::BC, Slot::BR, Slot::TL, Slot::TC, Slot::TR};

Slot nextSlot(Slot s, int delta)
{
	const int at = static_cast<int>(
	    std::find(std::begin(SLOT_ORDER), std::end(SLOT_ORDER), s) -
	    std::begin(SLOT_ORDER));
	return SLOT_ORDER[(at + delta + SLOT_COUNT) % SLOT_COUNT];
}

} // namespace

AnnotationsPane::AnnotationsPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx)
{
}

AnnotationSet& AnnotationsPane::set(int group)
{
	return group == 0 ? m_ctx.m_ann.global()
	                  : m_ctx.m_ann.source(
	                        sourceKey(m_state.m_sources[m_state.m_current]));
}

// --- the tree ---

std::vector<AnnotationsPane::Row> AnnotationsPane::rows() const
{
	using K = Row::Kind;
	std::vector<Row> rows;
	const int g = std::max(0, m_group);
	const AnnotationSet* set =
	    g == 0 ? &m_ctx.m_ann.global()
	           : m_state.sourceAnnotations(m_ctx.m_ann, m_state.m_current);
	for(const Slot slot : SLOT_ORDER)
	{
		rows.push_back({K::SLOT, g, slot});
		const int n = set ? static_cast<int>(set->lines(slot).size()) : 0;
		for(int i = 0; i < n; ++i)
		{
			rows.push_back({K::LINE, g, slot, i});
		}
	}
	return rows;
}

void AnnotationsPane::select(int group, Slot slot, int line)
{
	m_group = group;
	const auto all = rows();
	for(size_t i = 0; i < all.size(); ++i)
	{
		const Row& r = all[i];
		const bool want = line >= 0 ? r.m_kind == Row::Kind::LINE
		                            : r.m_kind == Row::Kind::SLOT;
		if(want && r.m_slot == slot && (line < 0 || r.m_line == line))
		{
			m_cursor = static_cast<int>(i);
			return;
		}
	}
}

int AnnotationsPane::lineCount(const AnnotationSet* set)
{
	int n = 0;
	for(int s = 0; set && s < SLOT_COUNT; ++s)
	{
		n += static_cast<int>(set->m_slots[s].size());
	}
	return n;
}

Element AnnotationsPane::render(bool focused)
{
	using K = Row::Kind;
	const auto all = rows();
	m_cursor =
	    std::clamp(m_cursor, 0, std::max(0, static_cast<int>(all.size()) - 1));
	// Breadcrumb title, like a directory: "[3]─Files › Desk.exr".
	const std::string where =
	    m_group == 0 ? "global"
	                 : m_state.m_sources[m_state.m_current].m_entry.m_name;
	std::string title = "─[3]─Files › ";
	int used = 13 + ftxui::string_width(where);
	std::string fill;
	for(int i = used; i < m_state.leftPanelWidth(); ++i)
	{
		fill += "─";
	}
	const AnnotationSet* set =
	    m_group == 0
	        ? &m_ctx.m_ann.global()
	        : m_state.sourceAnnotations(m_ctx.m_ann, m_state.m_current);
	Elements lines;
	for(int i = 0; i < static_cast<int>(all.size()); ++i)
	{
		const Row& r = all[i];
		Element row;
		if(r.m_kind == K::SLOT)
		{
			const bool any = set && !set->lines(r.m_slot).empty();
			row = hbox(
			    {text(" "), text(slotName(r.m_slot)) | (any ? bold : dim)});
		}
		else if(m_edit && m_edit->m_slot == r.m_slot &&
		        m_edit->m_line == r.m_line)
		{
			// Being typed: the text is edited in the bottom bar, the pane
			// just marks the line.
			const std::string typed = m_edit->m_editor.text();
			Elements parts{text("  » "), text(typed.empty() ? "…" : typed)};
			row = hbox(std::move(parts)) | color(Color::Yellow);
		}
		else
		{
			const std::string& line = set->lines(r.m_slot)[r.m_line];
			row = hbox({text("   "),
			            text(line.empty() ? "(empty)" : line) |
			                (line.empty() ? dim : nothing)});
		}
		if(focused && i == m_cursor)
		{
			row = (m_edit ? row : row | inverted) | focus;
		}
		lines.push_back(row);
	}
	Element hints;
	if(m_edit)
	{
		hints = ui::paneHints("", {{"Enter", "done"}, {"Esc", "cancel"}});
	}
	else if(focused)
	{
		hints = ui::paneHints("", {{"h", "back"}, {"a", "add"}, {"d", "del"}});
	}
	else
	{
		hints = ui::paneHints("", {{"3", "focus"}});
	}
	constexpr int MAX_ROWS = 20;
	const Decorator titleStyle = focused ? color(Color::Green) | bold : dim;
	return vbox({
	    hbox({text(title) | titleStyle,
	          text(where) | titleStyle |
	              (m_group == 1 ? color(Color::Cyan) : nothing),
	          text(fill) | titleStyle}),
	    vbox(std::move(lines)) | vscroll_indicator | yframe |
	        size(HEIGHT, LESS_THAN, MAX_ROWS),
	    hints,
	});
}

void AnnotationsPane::clear(int group)
{
	if(group == 0)
	{
		m_ctx.m_ann.global() = {};
	}
	else if(group == 1)
	{
		set(1) = {};
	}
	else
	{
		m_ctx.m_ann.global() = {};
		for(const auto& s : m_state.m_sources)
		{
			m_ctx.m_ann.source(sourceKey(s)) = {};
		}
	}
	m_cursor = 0;
}

// --- editing ---

Element AnnotationsPane::renderInput() const
{
	// Full-width input line in place of the status bar: the slot, then the
	// text with a block cursor, scrolled to keep the cursor in view; key hints
	// on the right while they fit.
	const Edit& ed = *m_edit;
	const int width = Terminal::Size().dimx;
	const std::string where = fmt::format(" {} {} › ",
	                                      ed.m_group == 0 ? "global" : "file",
	                                      slotName(ed.m_slot));
	const std::string hints =
	    "  Enter done  Esc cancel  Tab slot  ↑↓ history  C-n key ";
	int room = width - string_width(where) - 1;
	const int textW = static_cast<int>(ed.m_editor.glyphs().size()) + 1;
	const bool withHints = textW + string_width(hints) <= room;
	if(withHints)
	{
		room -= string_width(hints);
	}
	const auto& g = ed.m_editor.glyphs();
	const int cur = ed.m_editor.cursor();
	const int first = std::max(0, cur - std::max(1, room - 1) + 1);
	std::string before, after;
	for(int k = first; k < cur; ++k)
	{
		before += g[k];
	}
	for(int k = cur + 1; k < static_cast<int>(g.size()); ++k)
	{
		after += g[k];
	}
	Elements parts{
	    text(where) | color(Color::Yellow) | bold,
	    text(first > 0 ? "…" : ""),
	    text(before),
	    text(cur < static_cast<int>(g.size()) ? g[cur] : " ") | inverted,
	    text(after),
	    filler(),
	};
	if(!m_ctx.m_message.empty())
	{
		parts.push_back(text(" " + m_ctx.m_message + " ") | dim);
	}
	else if(withHints)
	{
		parts.push_back(text(hints) | dim);
	}
	return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
}

void AnnotationsPane::click(int row)
{
	if(!m_edit && row >= 0 && row < static_cast<int>(rows().size()))
	{
		m_cursor = row;
	}
}

void AnnotationsPane::startEdit(int group, Slot slot, int line, int insertAt)
{
	auto& lines = set(group).lines(slot);
	Edit ed;
	ed.m_group = group;
	ed.m_slot = slot;
	if(line < 0)
	{
		const int at = insertAt < 0 || insertAt > static_cast<int>(lines.size())
		                   ? static_cast<int>(lines.size())
		                   : insertAt;
		lines.insert(lines.begin() + at, "");
		ed.m_line = at;
		ed.m_new = true;
	}
	else
	{
		ed.m_line = line;
		ed.m_editor = LineEditor(lines[line]);
	}
	m_edit = std::move(ed);
	m_ctx.m_ann.m_visible = true; // see what is typed
	select(group, slot, m_edit->m_line);
}

void AnnotationsPane::finishEdit(bool commit)
{
	if(!m_edit)
	{
		return;
	}
	Edit ed = std::move(*m_edit);
	m_edit.reset();
	auto& lines = set(ed.m_group).lines(ed.m_slot);
	if(ed.m_line < 0 || ed.m_line >= static_cast<int>(lines.size()))
	{
		return;
	}
	const std::string text = ed.m_editor.text();
	if(!commit)
	{
		if(ed.m_new)
		{
			lines.erase(lines.begin() + ed.m_line);
		}
		else
		{
			lines[ed.m_line] = ed.m_editor.original();
		}
	}
	else if(text.empty())
	{
		lines.erase(lines.begin() + ed.m_line); // an emptied line goes
	}
	else
	{
		lines[ed.m_line] = text;
		m_ctx.m_ann.addHistory(text);
	}
	const int n = static_cast<int>(lines.size());
	select(ed.m_group, ed.m_slot, n ? std::min(ed.m_line, n - 1) : -1);
}

std::vector<std::string> AnnotationsPane::headerKeys() const
{
	std::vector<std::string> names;
	if(ImageInfoPtr info = m_ctx.m_svc.info(m_state.currentFramePath()))
	{
		for(const auto& part : info->m_parts)
		{
			for(const auto& a : part.m_attributes)
			{
				names.push_back(a.m_name);
			}
		}
	}
	std::ranges::sort(names);
	const auto dups = std::ranges::unique(names);
	names.erase(dups.begin(), dups.end());
	return names;
}

bool AnnotationsPane::editEvent(const Event& e)
{
	if(e.is_mouse())
	{
		return true; // the line keeps the keyboard until Enter / Esc
	}
	Edit& ed = *m_edit;
	auto& lines = set(ed.m_group).lines(ed.m_slot);
	if(e == Event::CtrlN)
	{
		m_ctx.m_message = ed.m_editor.complete(BUILTINS, headerKeys());
	}
	else
	{
		switch(ed.m_editor.event(e, m_ctx.m_ann.history()))
		{
			case LineEditor::Result::COMMIT:
				finishEdit(true);
				return true;
			case LineEditor::Result::CANCEL:
				finishEdit(false);
				return true;
			case LineEditor::Result::EDITED:
				break;
			case LineEditor::Result::IGNORED:
				if(e == Event::Tab || e == Event::TabReverse)
				{
					// Same group, next / previous slot (appended there).
					const std::string text = ed.m_editor.text();
					lines.erase(lines.begin() + ed.m_line);
					ed.m_slot = nextSlot(ed.m_slot, e == Event::Tab ? 1 : -1);
					auto& to = set(ed.m_group).lines(ed.m_slot);
					to.push_back(text);
					ed.m_line = static_cast<int>(to.size()) - 1;
					select(ed.m_group, ed.m_slot, ed.m_line);
					m_ctx.m_message = std::string("→ ") + slotName(ed.m_slot);
					return true;
				}
				break;
		}
	}
	// Live: the overlay shows the line as typed.
	lines[ed.m_line] = ed.m_editor.text();
	return true;
}

bool AnnotationsPane::event(const Event& e)
{
	using K = Row::Kind;
	if(m_edit)
	{
		return editEvent(e);
	}
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	const bool clearAsked = m_clearPending;
	m_clearPending = false;
	const auto all = rows();
	const int n = static_cast<int>(all.size());
	m_cursor = std::clamp(m_cursor, 0, std::max(0, n - 1));
	const Row r = all[m_cursor];
	const int g = m_group;
	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(n - 1, m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(ch("G"))
	{
		m_cursor = n - 1;
	}
	else if(e == Event::Return || ch("l") || e == Event::ArrowRight)
	{
		if(r.m_kind == K::LINE)
		{
			startEdit(g, r.m_slot, r.m_line);
		}
		else
		{
			startEdit(g, r.m_slot, -1); // a slot row: add to it
		}
	}
	else if(ch("a") || ch("A"))
	{
		// New line below (`a`) / above (`A`) a line, else at the slot's end.
		const int at = r.m_kind == K::LINE ? r.m_line + (ch("a") ? 1 : 0) : -1;
		startEdit(g, r.m_slot, -1, at);
	}
	else if(r.m_kind == K::LINE && ch("d"))
	{
		auto& lines = set(g).lines(r.m_slot);
		lines.erase(lines.begin() + r.m_line);
		const int left = static_cast<int>(lines.size());
		select(g, r.m_slot, left ? std::min(r.m_line, left - 1) : -1);
	}
	else if(r.m_kind == K::LINE && (ch("J") || ch("K")))
	{
		auto& lines = set(g).lines(r.m_slot);
		const int to = r.m_line + (ch("J") ? 1 : -1);
		if(to >= 0 && to < static_cast<int>(lines.size()))
		{
			std::swap(lines[r.m_line], lines[to]);
			select(g, r.m_slot, to);
		}
	}
	else if(r.m_kind == K::LINE && (ch("g") || ch("s")))
	{
		// Move the line to the global / current-source set, same slot. The
		// pane stays where it is.
		const int toGroup = ch("g") ? 0 : 1;
		if(toGroup != g)
		{
			auto& from = set(g).lines(r.m_slot);
			const std::string line = from[r.m_line];
			from.erase(from.begin() + r.m_line);
			set(toGroup).lines(r.m_slot).push_back(line);
			const int left = static_cast<int>(from.size());
			select(g, r.m_slot, left ? std::min(r.m_line, left - 1) : -1);
			m_ctx.m_message =
			    toGroup == 0
			        ? "moved to global"
			        : "moved to " +
			              m_state.m_sources[m_state.m_current].m_entry.m_name;
		}
	}
	else if(ch("D"))
	{
		const std::string what =
		    g == 0 ? "global"
		           : m_state.m_sources[m_state.m_current].m_entry.m_name;
		if(clearAsked)
		{
			clear(g);
			m_ctx.m_message = "cleared " + what;
		}
		else
		{
			m_clearPending = true;
			m_ctx.m_message = "D again: clear " + what;
		}
	}
	else if(ch("1") || ch("q") || e == Event::Escape)
	{
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false;
	}
	return true;
}

} // namespace rega
