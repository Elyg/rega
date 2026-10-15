#pragma once

#include "app/AppContext.h"
#include "image/Sequence.h"
#include "term/ImageView.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rv
{

/// The file browser: parent | current directory | preview, yazi style, with a
/// fuzzy filter, marks across directories and sequences expandable into
/// frames. Reports what to view through `onOpen`; knows nothing of the viewer.
class Browser
{
public:
	/// View `entries` (`tile`: as a contact sheet).
	using OpenFn = std::function<void(std::vector<Entry> entries, bool tile)>;
	/// `d`: the marked images as diff pairs (see pairItems()).
	using DiffFn = std::function<void(std::vector<Entry> marked)>;

	/// @param chooserFile where `o` writes the chosen paths before quitting
	///                    (empty = `o` only says so).
	/// @param onOpen      called with the entries to open in the viewer.
	Browser(AppContext& ctx,
	        std::string chooserFile,
	        OpenFn onOpen,
	        DiffFn onDiff = {});
	Browser(const Browser&) = delete;
	Browser& operator=(const Browser&) = delete;

	/// Show `dir`, the cursor on `selectName` if given.
	void openDirectory(const std::filesystem::path& dir,
	                   std::string_view selectName = {});
	/// Handle an event; true when consumed.
	[[nodiscard]] bool event(ftxui::Event e);
	[[nodiscard]] ftxui::Element render();

	/// The filter is being typed (keys are text, not commands).
	bool typing() const noexcept
	{
		return m_filtering;
	}
	const std::filesystem::path& cwd() const noexcept
	{
		return m_cwd;
	}
	const std::vector<Entry>& entries() const noexcept
	{
		return m_entries;
	}
	/// Marked entries in the order they were marked, from any directory.
	const std::vector<Entry>& marks() const noexcept
	{
		return m_marks;
	}
	/// Mark the images among `entries` (those not marked yet), in order.
	void mark(const std::vector<Entry>& entries);
	const std::string& filter() const noexcept
	{
		return m_filter;
	}
	const Entry* selectedEntry() const;      ///< under the cursor, or nullptr
	std::vector<int> visibleIndices() const; ///< entries passing the filter

	/// Re-list the directories on screen, keeping the selection on the same
	/// entry by name. True if anything shown changed.
	bool refresh();
	/// The directories on screen (current, parent, previewed).
	std::vector<std::filesystem::path> shownDirs() const;

private:
	void refreshListing();
	/// listDirectory(m_cwd) with the sequences in m_expanded shown as frames.
	std::vector<Entry> currentListing() const;
	void toggleExpand(); ///< `e`: sequence ⇄ its frame files
	void moveSelection(int delta);
	bool isMarked(const Entry& e) const;
	void toggleMark(const Entry& e);
	void goParent();
	void enterSelected(bool tile = false);
	void chooseAndExit();
	bool filterEvent(const ftxui::Event& e);
	bool mouseEvent(ftxui::Event e);
	bool previewEvent(const ftxui::Event& e);
	void toggleMarkAll(); ///< `a`: every image listed, or none of them
	/// Where a column's list and its rows were drawn, for clicks.
	struct ListBoxes
	{
		ftxui::Box m_list{};
		std::vector<ftxui::Box> m_rows;
		/// The row shown at screen cell (x, y); -1 for none.
		int rowAt(int x, int y) const;
	};
	/// A column `width` cells wide, drawn where `boxes` then says.
	ftxui::Element renderEntryList(const std::vector<Entry>& entries,
	                               int selected,
	                               bool active,
	                               const std::vector<int>* indices,
	                               int width,
	                               ListBoxes& boxes);
	/// A click on a row of the current column (`row` into visibleIndices())
	/// or of the parent one.
	void clickRow(int row);
	void clickParentRow(int row);
	ftxui::Element renderPreview();
	/// First lines of a readable text file, or nullopt if it looks binary.
	/// Cached for the selected file (path + mtime + size).
	const std::optional<std::vector<std::string>>&
	textPreview(const std::filesystem::path& p);
	const std::vector<Entry>& cachedListing(const std::filesystem::path& dir);
	std::pair<int, int> columnWidths() const;

	AppContext& m_ctx;
	std::string m_chooserFile;
	OpenFn m_onOpen;
	DiffFn m_onDiff;

	std::filesystem::path m_cwd;
	std::vector<Entry> m_entries;
	int m_sel = 0; ///< index into visibleIndices()
	std::map<std::string, std::string> m_lastSelected; ///< dir → entry name
	std::map<std::string, std::vector<Entry>> m_listingCache;
	bool m_showHidden = false;
	bool m_filtering = false;
	std::string m_filter;
	std::vector<Entry> m_marks;
	std::set<std::string> m_expanded; ///< sequences shown as frames (keys)
	bool m_pendingG = false;
	ImageSlotPtr m_previewSlot;
	struct TextPreview
	{
		std::string m_key;
		std::optional<std::vector<std::string>> m_lines;
	} m_textPreview;
	/// `l` on a text file moves into the preview column to scroll it.
	bool m_previewFocus = false;
	int m_previewScroll = 0;
	ftxui::Box m_previewBox{};
	/// Column widths as fractions of the terminal (drag the dividers).
	double m_parentFrac = 0.15, m_currentFrac = 0.30;
	int m_dragDivider = 0; ///< 1 = parent|current, 2 = current|preview
	/// As last drawn (the preview's only to outlive the drawing: reflect()
	/// keeps a reference to each box).
	ListBoxes m_currentBoxes, m_parentBoxes, m_previewBoxes;
	/// The last click on a current-column row: a second one soon after on
	/// the same row opens it (a double click).
	int m_lastClickRow = -1;
	std::chrono::steady_clock::time_point m_lastClick{};
};

} // namespace rv
