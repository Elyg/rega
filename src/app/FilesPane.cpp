#include "app/FilesPane.h"

#include "util/Ui.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <vector>

using namespace ftxui;

namespace rega
{

FilesPane::FilesPane(ViewerState& state, AppContext& ctx)
    : m_state(state), m_ctx(ctx), m_ann(state, ctx)
{
}

void FilesPane::show()
{
	m_open = true;
	m_cursor = m_state.m_current;
	m_state.m_focus = Focus::FILES;
}

void FilesPane::click(int y)
{
	// A row below the title: a file to show, or an annotation row.
	m_state.m_focus = Focus::FILES;
	const int row = y - m_box.y_min - 1;
	if(m_ann.isOpen())
	{
		m_ann.click(row);
	}
	else if(row == 0)
	{
		m_cursor = -1; // the global row
	}
	else if(row > 0 && row <= static_cast<int>(m_state.m_sources.size()))
	{
		m_cursor = m_state.m_current = row - 1;
	}
}

Element FilesPane::render()
{
	const bool focus = focused();
	if(m_ann.isOpen())
	{
		return m_ann.render(focus) | reflect(m_box);
	}
	const auto& sources = m_state.m_sources;
	const auto& ann = m_ctx.m_ann;
	const int n = static_cast<int>(sources.size());
	m_cursor = std::clamp(m_cursor, -1, std::max(0, n - 1));
	// Annotation line counts on the right; `l` goes into a row's lines.
	auto count = [](int lines)
	{ return lines ? fmt::format(" {} ", lines) : std::string(); };
	// What a row has for its name: the column less the scroll indicator.
	// Names are cut to fit it, as a row too wide makes ftxui squeeze all of
	// its cells (the numbers vanished).
	const int width = m_state.leftPanelWidth() - 1;
	Elements rows;
	{
		Element row = hbox(
		    {text("      global") | (ann.global().empty() ? dim : nothing),
		     filler(),
		     text(count(AnnotationsPane::lineCount(&ann.global()))) | dim});
		if(focus && m_cursor == -1)
		{
			row = row | inverted | ftxui::focus;
		}
		rows.push_back(row);
	}
	for(int i = 0; i < n; ++i)
	{
		const bool shown = i == m_state.m_current;
		const std::string num = fmt::format(" {:>2} ", i + 1);
		const std::string lines = count(
		    AnnotationsPane::lineCount(m_state.sourceAnnotations(ann, i)));
		// The middle goes: these often share a long prefix.
		const int room =
		    std::max(1,
		             width - static_cast<int>(num.size() + 2 + lines.size()));
		Element row = hbox({
		    text(num) | dim,
		    text(shown ? "▶ " : "  ") | color(Color::Cyan),
		    // Frames of an expanded sequence: magenta, as in the browser.
		    text(ui::ellipsizeMiddle(sources[i].m_entry.m_name, room)) |
		        (shown ? color(Color::Cyan) | bold
		         : !sources[i].m_entry.m_expandedFrom.empty()
		             ? color(Color::Magenta)
		             : nothing),
		    filler(),
		    text(lines) | dim,
		});
		if(focus && i == m_cursor)
		{
			row = row | inverted | ftxui::focus;
		}
		rows.push_back(row);
	}
	return vbox({
	           text(ui::paneTitle("─[3]─Files", m_state.leftPanelWidth())) |
	               (focus ? color(Color::Green) | bold : dim),
	           vbox(std::move(rows)) | vscroll_indicator | yframe |
	               size(HEIGHT, LESS_THAN, m_rows),
	           focus ? ui::paneHints("", {{"l", "text"}, {"D", "clear all"}})
	                 : ui::paneHints("", {{"3", "focus"}}),
	       }) |
	       reflect(m_box);
}

void FilesPane::moveSource(int from, int to)
{
	auto& sources = m_state.m_sources;
	int& current = m_state.m_current;
	const int n = static_cast<int>(sources.size());
	if(from < 0 || to < 0 || from >= n || to >= n || from == to)
	{
		return;
	}
	std::swap(sources[from], sources[to]);
	// The shown image stays the same image.
	if(current == from)
	{
		current = to;
	}
	else if(current == to)
	{
		current = from;
	}
}

void FilesPane::toggleSourceExpand()
{
	auto& sources = m_state.m_sources;
	int& current = m_state.m_current;
	int& frame = m_state.m_frame;
	const int n = static_cast<int>(sources.size());
	const int i = m_cursor;
	if(i < 0 || i >= n)
	{
		return;
	}
	const Entry& at = sources[i].m_entry;
	if(at.m_kind == Entry::Kind::SEQUENCE)
	{
		const int frames = static_cast<int>(at.m_frames.size());
		if(frames > MAX_EXPAND_FRAMES)
		{
			m_ctx.m_message = fmt::format("{} frames: expands up to {}",
			                              frames,
			                              MAX_EXPAND_FRAMES);
			return;
		}
		// The sequence's place in the list becomes its frames, in order.
		const std::string key = sourceKey(sources[i]);
		Entry seq = at;
		std::vector<Source> expanded;
		for(const auto& f : seq.m_frames)
		{
			Entry e;
			e.m_kind = Entry::Kind::FILE;
			e.m_path = f;
			e.m_name = f.filename().string();
			e.m_expandedFrom = key;
			expanded.push_back({std::move(e)});
			m_ctx.m_ann.touch(sourceKey(expanded.back()));
		}
		sources.erase(sources.begin() + i);
		sources.insert(sources.begin() + i, expanded.begin(), expanded.end());
		// Keep showing the frame that was on screen.
		const int shown = std::clamp(frame, 0, frames - 1);
		if(current == i)
		{
			current = i + shown;
		}
		else if(current > i)
		{
			current += frames - 1;
		}
		m_cursor = current >= i && current < i + frames ? current : i;
		frame = std::min(frame, m_state.frameCount() - 1);
		m_expandedSources[key] = std::move(seq);
		m_ctx.m_message = fmt::format("{} frames", frames);
		return;
	}
	if(at.m_expandedFrom.empty())
	{
		return; // a plain file
	}
	// Fold: every frame of that sequence still open goes back to one row,
	// where the first of them is.
	const std::string key = at.m_expandedFrom;
	auto it = m_expandedSources.find(key);
	if(it == m_expandedSources.end())
	{
		return;
	}
	const Entry seq = it->second;
	m_expandedSources.erase(it);
	int first = -1, newCurrent = -1, newFrame = frame;
	std::vector<Source> kept;
	for(int k = 0; k < n; ++k)
	{
		const Entry& en = sources[k].m_entry;
		if(en.m_expandedFrom != key)
		{
			if(k == current)
			{
				newCurrent = static_cast<int>(kept.size());
			}
			kept.push_back(sources[k]);
			continue;
		}
		if(first < 0)
		{
			first = static_cast<int>(kept.size());
			kept.push_back({seq});
		}
		if(k == current)
		{
			// Show the same frame, now as a frame of the sequence.
			newCurrent = first;
			const auto f = std::ranges::find(seq.m_frames, en.m_path);
			newFrame = static_cast<int>(f - seq.m_frames.begin());
		}
	}
	sources = std::move(kept);
	current = std::max(0, newCurrent);
	frame = newFrame;
	m_cursor = first;
}

bool FilesPane::event(const Event& e)
{
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	if(m_ann.isOpen())
	{
		if(!m_ann.editing() && (ch("h") || e == Event::ArrowLeft))
		{
			// Back out to the file list, onto the row we came from.
			m_cursor = m_ann.group() == 0 ? -1 : m_state.m_current;
			m_ann.leave();
			return true;
		}
		if(!m_ann.editing() && ch("3"))
		{
			m_open = false; // focused: its number closes it
			m_state.m_focus = Focus::IMAGE;
			return true;
		}
		return m_ann.event(e);
	}
	auto& sources = m_state.m_sources;
	int& current = m_state.m_current;
	const int n = static_cast<int>(sources.size());
	const bool clearAsked = m_clearPending;
	m_clearPending = false;
	// Row -1 is "global": it can only be entered.
	const bool onGlobal = m_cursor < 0;
	if(ch("j") || e == Event::ArrowDown)
	{
		m_cursor = std::min(n - 1, m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		m_cursor = std::max(-1, m_cursor - 1);
	}
	else if(ch("l") || e == Event::ArrowRight ||
	        (onGlobal && e == Event::Return))
	{
		// Into the row's annotation lines (a file is shown as well).
		if(!onGlobal)
		{
			current = m_cursor;
		}
		m_ann.enter(onGlobal ? 0 : 1);
	}
	else if(e == Event::Return)
	{
		current = m_cursor;
	}
	else if(ch("e"))
	{
		toggleSourceExpand();
	}
	else if(ch("D"))
	{
		if(clearAsked)
		{
			m_ann.clear(-1);
			m_ctx.m_message = "cleared all annotations";
		}
		else
		{
			m_clearPending = true;
			m_ctx.m_message = "D again: clear all annotations";
		}
	}
	else if(onGlobal && (ch("J") || ch("K") || ch("d")))
	{
		// the global row stays put
	}
	else if(ch("J"))
	{
		moveSource(m_cursor, m_cursor + 1);
		m_cursor = std::min(n - 1, m_cursor + 1);
	}
	else if(ch("K"))
	{
		moveSource(m_cursor, m_cursor - 1);
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(ch("d"))
	{
		if(n <= 1)
		{
			m_ctx.m_message = "the last image stays";
			return true;
		}
		sources.erase(sources.begin() + m_cursor);
		if(current > m_cursor || current >= n - 1)
		{
			current = std::max(0, current - 1);
		}
		m_cursor = std::min(m_cursor, n - 2);
	}
	else if(ch("1") || ch("q") || e == Event::Escape || ch("h"))
	{
		m_state.m_focus = Focus::IMAGE;
	}
	else if(ch("3"))
	{
		m_open = false; // focused: its number closes it
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false;
	}
	return true;
}

} // namespace rega
