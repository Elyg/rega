#pragma once

#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rega
{

/// [6] colour column: the OCIO config, display, view and look, and the shown
/// image's input colour space. h/l steps a value, Enter picks it from a
/// fuzzy list (configs with dozens of colour spaces).
class ColourPane
{
public:
	/// The pane's rows, top to bottom.
	enum class Row
	{
		CONFIG,
		DISPLAY,
		VIEW,
		LOOK,
		INPUT
	};
	static constexpr int ROWS = 5;

	ColourPane(ViewerState& state, AppContext& ctx);

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
		return m_state.m_focus == Focus::COLOUR;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}
	/// The fuzzy list is open (keys are its filter).
	bool picking() const noexcept
	{
		return m_picker.has_value();
	}
	/// Open and focus it.
	void show();

	/// `path`: the image shown, of source `sourceKey` (its input row).
	[[nodiscard]] ftxui::Element render(const std::filesystem::path& path,
	                                    const std::string& sourceKey);
	/// Focused: j/k rows, h/l step, Enter list, 6 closes. Other keys (s,
	/// exposure…) fall through to the viewer.
	[[nodiscard]] bool event(const ftxui::Event& e,
	                         const std::filesystem::path& path,
	                         const std::string& sourceKey);

	/// What row `r` can be set to (as shown), and which of them it is.
	std::vector<std::string> values(Row r,
	                                const std::filesystem::path& path,
	                                const std::string& sourceKey,
	                                int& current) const;
	/// Set row `r` to its `index`th value.
	void choose(Row r,
	            int index,
	            const std::filesystem::path& path,
	            const std::string& sourceKey);

private:
	/// The fuzzy list over one row's values.
	struct Picker
	{
		Row m_row;
		std::string m_filter;
		int m_selected = 0; ///< among the matches
	};
	/// Indices of the values matching the picker's filter.
	std::vector<int> matches(const std::vector<std::string>& all) const;
	bool pickerEvent(const ftxui::Event& e,
	                 const std::filesystem::path& path,
	                 const std::string& sourceKey);

	ViewerState& m_state;
	AppContext& m_ctx;
	bool m_open = false;
	int m_cursor = 0;
	std::optional<Picker> m_picker;
	ftxui::Box m_box{};
};

} // namespace rega
