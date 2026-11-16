#pragma once

#include "app/AppContext.h"
#include "app/LineEditor.h"
#include "app/Recent.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rega
{

/// A pasted or typed path as rega opens it: trimmed, unquoted, `file://`
/// URLs decoded, `~` expanded (with `home`). Empty when nothing is left.
[[nodiscard]] std::string pastedPath(std::string_view text,
                                     const std::filesystem::path& home);
/// pastedPath() of each line of `text` (blank lines dropped).
[[nodiscard]] std::vector<std::string>
pastedPaths(std::string_view text, const std::filesystem::path& home);

/// The window's first screen when it was given nothing to open: open files
/// or a folder (the system's dialogs), browse the last folder, the tutorial,
/// a list of pasted paths to open together, or something opened lately.
/// Every row takes a click or a key.
class StartScreen
{
public:
	struct Actions
	{
		/// The system's Open dialog: files (false) or a folder (true).
		std::function<void(bool folder)> m_dialog;
		/// The file browser, where it was left.
		std::function<void()> m_browse;
		/// The tutorial: its images and the tour through them.
		std::function<void()> m_tutorial;
		/// Open these paths (a recent item, or the pasted list) together.
		std::function<void(const std::vector<std::string>& paths)> m_open;
	};

	/// The fixed rows; the pasted paths, their Open button and the recent
	/// items follow (see firstPasted() …).
	enum Row
	{
		OPEN_FILES,
		OPEN_FOLDER,
		BROWSE,
		TUTORIAL,
		PASTE ///< the field
	};

	/// A path in the pasted list.
	struct Pasted
	{
		std::string m_path;
		bool m_found = false; ///< opens something (checked when added)
	};

	StartScreen(AppContext& ctx, Recent& recent, Actions actions);

	/// Shown now: the recent items that still exist, the cursor on the top.
	void show();
	/// Handle an event; true when consumed.
	[[nodiscard]] bool event(ftxui::Event e);
	/// Text from the clipboard: a line or more, each a path for the list;
	/// one line while the field is being typed in goes into it.
	void paste(std::string_view text);
	/// `browseDir`: where the browser is (named on its row).
	[[nodiscard]] ftxui::Element render(const std::filesystem::path& browseDir);

	/// The field is being typed in (keys are text, not commands).
	[[nodiscard]] bool typing() const noexcept
	{
		return m_field.has_value();
	}
	/// The recent items listed (those that still exist), as Recent has them.
	[[nodiscard]] const std::vector<RecentItem>& shown() const noexcept
	{
		return m_items;
	}
	[[nodiscard]] const std::vector<Pasted>& pasted() const noexcept
	{
		return m_pasted;
	}
	[[nodiscard]] int selected() const noexcept
	{
		return m_sel;
	}
	[[nodiscard]] int firstPasted() const noexcept
	{
		return PASTE + 1;
	}
	/// The "Open these together" row (there when anything was pasted).
	[[nodiscard]] int openPastedRow() const noexcept
	{
		return firstPasted() + static_cast<int>(m_pasted.size());
	}
	[[nodiscard]] int firstRecent() const noexcept
	{
		return openPastedRow() + (m_pasted.empty() ? 0 : 1);
	}

private:
	[[nodiscard]] int rows() const noexcept
	{
		return firstRecent() + static_cast<int>(m_items.size());
	}
	[[nodiscard]] bool onPasted(int row) const noexcept
	{
		return row >= firstPasted() && row < openPastedRow();
	}
	void activate(int row);
	/// Add `path` to the pasted list (not twice); false if nothing to add.
	bool add(const std::string& path);
	void removePasted(int row);
	[[nodiscard]] int rowAt(int x, int y) const;
	[[nodiscard]] bool fieldEvent(const ftxui::Event& e);

	AppContext& m_ctx;
	Recent& m_recent;
	Actions m_actions;
	std::vector<RecentItem> m_items;
	std::vector<Pasted> m_pasted;
	std::optional<LineEditor> m_field; ///< set while typing in the field
	int m_sel = 0;
	std::vector<ftxui::Box> m_boxes;  ///< where each row was drawn
	std::vector<ftxui::Box> m_remove; ///< each pasted row's ×
};

} // namespace rega
