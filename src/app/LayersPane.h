#pragma once

#include "app/ViewerState.h"
#include "image/ImageService.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace rega
{

/// [5] layer list: moving the cursor (or clicking a row) shows that layer.
class LayersPane
{
public:
	explicit LayersPane(ViewerState& state);

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
		return m_state.m_focus == Focus::LAYERS;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}
	/// Open and focus it, the cursor on the layer being shown.
	void show(const ImageInfoPtr& info);
	/// A click at screen row `y` (over the pane): focus it, show that layer.
	void click(int y, const ImageInfoPtr& info);

	[[nodiscard]] ftxui::Element render(const ImageInfoPtr& info);
	[[nodiscard]] bool event(const ftxui::Event& e, const ImageInfoPtr& info);

private:
	ViewerState& m_state;
	bool m_open = false;
	int m_cursor = 0; ///< index into the image's layers
	ftxui::Box m_box{};
};

} // namespace rega
