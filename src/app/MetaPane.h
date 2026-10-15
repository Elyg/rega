#pragma once

#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace rv
{

/// [2] metadata column: the current frame's header (parts, attributes,
/// layers), scrollable, with a vim-style cursor and visual-line copy.
class MetaPane
{
public:
	/// One row of the pane.
	struct Item
	{
		enum class Kind
		{
			TITLE,
			SUMMARY, ///< a diff's: how many rows differ
			PATH,
			PART,
			ATTRIBUTE,
			LAYERS,
			LAYER
		};
		/// On a diff (A against B): how this row's two values compare.
		enum class Diff
		{
			NONE, ///< not a diff
			SAME,
			CHANGED,
			ONLY_A,
			ONLY_B
		};
		Kind m_kind;
		std::string m_name;
		std::string m_value; ///< a diff's: A's (SAME: both)
		Diff m_diff = Diff::NONE;
		std::string m_valueB; ///< a diff's: B's
	};

	MetaPane(ViewerState& state, AppContext& ctx);

	bool isOpen() const noexcept
	{
		return m_open;
	}
	void setOpen(bool open) noexcept
	{
		m_open = open;
	}
	bool focused() const noexcept
	{
		return m_state.m_focus == Focus::META;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}
	/// Scroll by `rows` (wheel, `{` / `}`, PageUp / PageDown).
	void scroll(int rows) noexcept
	{
		m_scroll = std::max(0, m_scroll + rows);
	}
	/// Back to the top (a new set of images).
	void resetScroll() noexcept
	{
		m_scroll = 0;
	}

	/// The rows for `info`'s header; on a diff, A's against B's (`info`):
	/// the rows that differ, or all of them (`d`).
	std::vector<Item> items(const ImageInfoPtr& info) const;
	/// On a diff: every row, not just those that differ.
	bool diffAll() const noexcept
	{
		return m_diffAll;
	}
	/// items() through the filter: the rows whose name (fuzzy) or value
	/// (a substring) matches, under their part / "layers" heading.
	std::vector<Item> shown(const ImageInfoPtr& info) const;
	/// `/` typing a filter: every key goes to the pane.
	bool typing() const noexcept
	{
		return m_filtering;
	}
	/// Leave the filter as typed (focus went elsewhere).
	void stopTyping() noexcept
	{
		m_filtering = false;
	}
	const std::string& filter() const noexcept
	{
		return m_filter;
	}
	/// A click at screen row `y`: focus, the cursor to that row (`extend`,
	/// Shift / Ctrl: select from the cursor to it, as `v` does).
	void click(int y, bool extend);
	/// The mouse dragged to screen row `y` after a click: select from the
	/// clicked row to it (past the ends: the first / last row drawn).
	void dragTo(int y);
	[[nodiscard]] ftxui::Element render(const ImageInfoPtr& info);
	/// Focused: j/k gg G ^d/^u, / filter, v select, y/Y copy, d (a diff)
	/// differences / all, 2 closes. Other
	/// keys (exposure…) fall through to the viewer.
	[[nodiscard]] bool event(const ftxui::Event& e, const ImageInfoPtr& info);

private:
	/// Cells before an attribute / layer name.
	static constexpr int NAME_INDENT = 2;

	void copy(const ImageInfoPtr& info, bool valuesOnly);
	/// A diff's rows: `a`'s header against `b`'s.
	std::vector<Item> diffItems(const ImageInfoPtr& a,
	                            const ImageInfoPtr& b) const;
	/// Whether the current source is a diff.
	bool onDiff() const;
	bool filterEvent(const ftxui::Event& e, const ImageInfoPtr& info);
	/// The cursor to the first row the filter let through (not the title).
	void toFirstMatch(const ImageInfoPtr& info);
	/// Width of the name column (incl. the gap before the value).
	int nameColumn(const std::vector<Item>& all) const;
	/// The screen lines of one row: one, cut to fit, or (`expand`, the
	/// focused cursor row) as many as its whole name and value take.
	ftxui::Elements itemLines(const Item& it, int nameCol, bool expand) const;

	ViewerState& m_state;
	AppContext& m_ctx;
	bool m_open = false;
	int m_cursor = 0;
	int m_anchor = -1; ///< visual-line selection start, -1 = none
	bool m_pendingG = false;
	int m_scroll = 0;         ///< first visible row
	bool m_filtering = false; ///< `/`: typing the filter
	std::string m_filter;
	bool m_diffAll = false;       ///< `d` on a diff: every row
	std::vector<int> m_lineItems; ///< per screen line drawn: its row
	ftxui::Box m_box{}; ///< where it was drawn (wheel scrolling, page size)
};

} // namespace rv
