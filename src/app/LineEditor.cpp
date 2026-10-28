#include "app/LineEditor.h"

#include <ftxui/screen/string.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>

using namespace ftxui;

namespace rega
{

LineEditor::LineEditor(std::string_view text)
    : m_glyphs(Utf8ToGlyphs(std::string(text))),
      m_cursor(static_cast<int>(m_glyphs.size())), m_original(text)
{
}

std::string LineEditor::text() const
{
	std::string s;
	for(const auto& g : m_glyphs)
	{
		s += g;
	}
	return s;
}

void LineEditor::setText(std::string_view text)
{
	m_glyphs = Utf8ToGlyphs(std::string(text));
	m_cursor = static_cast<int>(m_glyphs.size());
}

std::string LineEditor::complete(std::span<const std::string> builtins,
                                 std::span<const std::string> keys)
{
	if(m_compIdx < 0)
	{
		// The `[#` token the cursor is in: no `]` between it and the cursor.
		int open = -1;
		for(int k = m_cursor - 1; k >= 1; --k)
		{
			if(m_glyphs[k] == "]")
			{
				break;
			}
			if(m_glyphs[k - 1] == "[" && m_glyphs[k] == "#")
			{
				open = k + 1;
				break;
			}
		}
		if(open < 0)
		{
			return "C-n completes after [# or [#@";
		}
		const bool builtin = open < m_cursor && m_glyphs[open] == "@";
		const int start = builtin ? open + 1 : open;
		std::string prefix;
		for(int k = start; k < m_cursor; ++k)
		{
			prefix += m_glyphs[k];
		}
		m_completions.clear();
		for(const auto& n : builtin ? builtins : keys)
		{
			if(n.starts_with(prefix))
			{
				m_completions.push_back(n);
			}
		}
		if(m_completions.empty())
		{
			return "no key starts with '" + prefix + "'";
		}
		// Replace what was typed of the key.
		m_glyphs.erase(m_glyphs.begin() + start, m_glyphs.begin() + m_cursor);
		m_cursor = m_compStart = start;
		m_compLen = 0;
	}
	// Swap in the next candidate (with its closing `]`).
	m_glyphs.erase(m_glyphs.begin() + m_compStart,
	               m_glyphs.begin() + m_compStart + m_compLen);
	m_compIdx = (m_compIdx + 1) % static_cast<int>(m_completions.size());
	auto ins = Utf8ToGlyphs(m_completions[m_compIdx] + "]");
	m_glyphs.insert(m_glyphs.begin() + m_compStart, ins.begin(), ins.end());
	m_compLen = static_cast<int>(ins.size());
	m_cursor = m_compStart + m_compLen;
	return fmt::format("key {}/{}", m_compIdx + 1, m_completions.size());
}

LineEditor::Result LineEditor::event(const Event& e,
                                     std::span<const std::string> history)
{
	if(e != Event::CtrlN)
	{
		m_compIdx = -1; // any other key ends a completion
	}
	if(e == Event::Return)
	{
		return Result::COMMIT;
	}
	if(e == Event::Escape)
	{
		setText(m_original);
		return Result::CANCEL;
	}
	if(e == Event::ArrowUp && !history.empty())
	{
		if(m_historyIdx < 0)
		{
			m_typed = text();
		}
		m_historyIdx =
		    std::min(m_historyIdx + 1, static_cast<int>(history.size()) - 1);
		setText(history[m_historyIdx]);
	}
	else if(e == Event::ArrowDown && m_historyIdx >= 0)
	{
		--m_historyIdx;
		setText(m_historyIdx < 0 ? m_typed : history[m_historyIdx]);
	}
	else if(e == Event::ArrowLeft)
	{
		m_cursor = std::max(0, m_cursor - 1);
	}
	else if(e == Event::ArrowRight)
	{
		m_cursor = std::min(static_cast<int>(m_glyphs.size()), m_cursor + 1);
	}
	else if(e == Event::Home || e == Event::CtrlA)
	{
		m_cursor = 0;
	}
	else if(e == Event::End || e == Event::CtrlE)
	{
		m_cursor = static_cast<int>(m_glyphs.size());
	}
	else if(e == Event::Backspace)
	{
		if(m_cursor > 0)
		{
			m_glyphs.erase(m_glyphs.begin() + --m_cursor);
		}
	}
	else if(e == Event::Delete)
	{
		if(m_cursor < static_cast<int>(m_glyphs.size()))
		{
			m_glyphs.erase(m_glyphs.begin() + m_cursor);
		}
	}
	else if(e == Event::CtrlU)
	{
		m_glyphs.erase(m_glyphs.begin(), m_glyphs.begin() + m_cursor);
		m_cursor = 0;
	}
	else if(e == Event::CtrlW)
	{
		int k = m_cursor;
		while(k > 0 && m_glyphs[k - 1] == " ")
		{
			--k;
		}
		while(k > 0 && m_glyphs[k - 1] != " ")
		{
			--k;
		}
		m_glyphs.erase(m_glyphs.begin() + k, m_glyphs.begin() + m_cursor);
		m_cursor = k;
	}
	else if(e.is_character())
	{
		auto ins = Utf8ToGlyphs(e.character());
		m_glyphs.insert(m_glyphs.begin() + m_cursor, ins.begin(), ins.end());
		m_cursor += static_cast<int>(ins.size());
	}
	else
	{
		return Result::IGNORED;
	}
	return Result::EDITED;
}

} // namespace rega
