#pragma once

#include <ftxui/dom/elements.hpp>

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rega::ui
{

/// `name` with the bytes at `pos` (fuzzyMatch hits) highlighted.
[[nodiscard]] ftxui::Element highlighted(std::string_view name,
                                         std::span<const size_t> pos,
                                         ftxui::Decorator base);

/// Byte offsets of the first occurrence of `needle` in `hay`, ignoring
/// (ASCII) case, for highlighted(); empty when it is not there or `needle`
/// is empty.
[[nodiscard]] std::vector<size_t> findIgnoringCase(std::string_view hay,
                                                   std::string_view needle);

/// A pane footer hint: key, what it does.
using PaneHint = std::pair<std::string_view, std::string_view>;
/// Pane footer: an optional dim prefix (position), then "(key what)" hints
/// with the key in bold.
[[nodiscard]] ftxui::Element paneHints(std::string_view prefix,
                                       std::initializer_list<PaneHint> hints);

/// Bottom bar, one line of `width` cells, by priority: `status` (frame / fps)
/// always on the left, then `warning` (a setup problem, yellow) if any; a
/// message takes the hints' place while it shows; `tail` (cache) dim at the
/// right end. Short of room, hints go first, then the tail; the message is
/// truncated last (with …).
struct StatusLine
{
	std::string_view m_message;
	std::string_view m_hints;
	ftxui::Elements m_status;
	std::string_view m_tail;
	std::string_view m_warning;
};
/// Render `line` (see StatusLine) in `width` cells.
[[nodiscard]] ftxui::Element statusLine(StatusLine line, int width);

/// "─[n]─Name───…" pane title, `width` cells wide.
[[nodiscard]] std::string paneTitle(std::string_view head, int width);

/// `s` in at most `width` cells: whole if it fits, else cut at the end with
/// "…" (by display width, never inside a glyph).
[[nodiscard]] std::string ellipsizeEnd(std::string_view s, int width);
/// Like ellipsizeEnd(), but the start goes: "…/shot/render.ass".
[[nodiscard]] std::string ellipsizeStart(std::string_view s, int width);
/// Like ellipsizeEnd(), but the middle goes: "shaders.cam…anLeft" keeps
/// names that share a long prefix apart.
[[nodiscard]] std::string ellipsizeMiddle(std::string_view s, int width);
/// ellipsizeMiddle(), with `pos` (ascending byte offsets into `s`, as
/// fuzzyMatch gives) moved onto the same bytes of the result; those in the
/// cut-out middle go.
[[nodiscard]] std::pair<std::string, std::vector<size_t>>
ellipsizeMiddle(std::string_view s, int width, std::span<const size_t> pos);
/// `s` broken into lines of at most `width` cells (by glyph, not by word);
/// one empty line for an empty `s`.
[[nodiscard]] std::vector<std::string> wrapWidth(std::string_view s, int width);

} // namespace rega::ui
