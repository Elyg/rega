#pragma once

#include "app/AppContext.h"
#include "app/LineEditor.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <optional>
#include <string>
#include <vector>

namespace rega
{

/// The level inside pane [3] (`l` on a row): one annotation set, global or the
/// current source's, as its slots and their lines; lines are added, edited
/// (typed in the bottom bar), reordered, moved and deleted here.
class AnnotationsPane
{
public:
	/// Rows of the open set: each slot, then its lines.
	struct Row
	{
		enum class Kind
		{
			SLOT,
			LINE
		};
		Kind m_kind;
		int m_group = 0;
		Slot m_slot = Slot::TL;
		int m_line = -1;
	};

	AnnotationsPane(ViewerState& state, AppContext& ctx);

	/// Inside a set (else pane [3] shows the file list).
	bool isOpen() const noexcept
	{
		return m_group >= 0;
	}
	/// 0 = global, 1 = the current source's, -1 = not open.
	int group() const noexcept
	{
		return m_group;
	}
	/// Open set `group` (see group()).
	void enter(int group) noexcept
	{
		m_group = group;
		m_cursor = 0;
	}
	/// Back to the file list.
	void leave() noexcept
	{
		m_group = -1;
		m_clearPending = false;
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}
	/// A click on row `row` of the set.
	void click(int row);
	std::vector<Row> rows() const;

	bool editing() const noexcept
	{
		return m_edit.has_value();
	}
	void finishEdit(bool commit); ///< `commit`: keep the typed line
	/// Empty the global set (0), the current source's (1), or everything on
	/// screen: global and every open source (-1).
	void clear(int group);
	static int lineCount(const AnnotationSet* set);

	[[nodiscard]] ftxui::Element render(bool focused);
	/// The bottom bar while a line is typed: a full-width input line.
	[[nodiscard]] ftxui::Element renderInput() const;
	/// Keys inside the set; `h` (back) and `3` (close) are the files pane's.
	[[nodiscard]] bool event(const ftxui::Event& e);

private:
	AnnotationSet& set(int group);
	/// Start editing line `line` of a slot (a new empty line when `line` < 0,
	/// inserted at `insertAt`, -1 = the end).
	void startEdit(int group, Slot slot, int line, int insertAt = -1);
	bool editEvent(const ftxui::Event& e);
	void select(int group, Slot slot, int line);
	/// Header attribute names of the current frame, for `[#…` completion.
	std::vector<std::string> headerKeys() const;

	ViewerState& m_state;
	AppContext& m_ctx;
	int m_group = -1;
	int m_cursor = 0;            ///< row in rows()
	bool m_clearPending = false; ///< `D` once asked; a second `D` clears
	struct Edit
	{
		int m_group = 0;
		Slot m_slot = Slot::BL;
		int m_line = 0;
		bool m_new = false; ///< Esc removes the line
		LineEditor m_editor;
	};
	std::optional<Edit> m_edit;
};

} // namespace rega
