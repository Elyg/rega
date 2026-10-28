#pragma once

#include <ftxui/component/event.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rega
{

/// One line of text being typed, readline style, edited by glyph (so a
/// multi-byte character is one step): cursor keys, Ctrl-A/E/U/W, Up/Down
/// history recall and Ctrl-N completion of `[#key]` / `[#@key]` placeholders.
/// No drawing: the owner shows text() and cursor().
class LineEditor
{
public:
	/// What event() did with a key.
	enum class Result
	{
		IGNORED, ///< not a key the line handles
		EDITED,  ///< text or cursor changed (or nothing to do)
		COMMIT,  ///< Enter
		CANCEL   ///< Esc: the text is back to how it started
	};

	/// Start editing `text`, cursor at the end.
	explicit LineEditor(std::string_view text = {});

	/// @param history recalled lines, most recent first.
	[[nodiscard]] Result event(const ftxui::Event& e,
	                           std::span<const std::string> history);
	/// Ctrl-N: complete the `[#…` the cursor is in with the next of `builtins`
	/// (after `[#@`) or `keys` (after `[#`) that starts with what is typed.
	/// Called again, cycles. Returns a line for the status bar.
	[[nodiscard]] std::string complete(std::span<const std::string> builtins,
	                                   std::span<const std::string> keys);

	std::string text() const;
	/// The text editing started from.
	const std::string& original() const noexcept
	{
		return m_original;
	}
	const std::vector<std::string>& glyphs() const noexcept
	{
		return m_glyphs;
	}
	/// Cursor position, in glyphs.
	int cursor() const noexcept
	{
		return m_cursor;
	}
	void setText(std::string_view text); ///< cursor to the end

private:
	std::vector<std::string> m_glyphs;
	int m_cursor = 0;
	std::string m_original; ///< restored on Esc
	int m_historyIdx = -1;  ///< Up/Down recall, -1 = typed
	std::string m_typed;    ///< the text before recalling
	std::vector<std::string> m_completions;
	int m_compIdx = -1;  ///< -1 = not completing
	int m_compStart = 0; ///< glyph index after `[#` / `[#@`
	int m_compLen = 0;   ///< glyphs inserted by the completion
};

} // namespace rega
