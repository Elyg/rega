#include "term/Native.h"

#include <ftxui/screen/screen.hpp>

#include <algorithm>

namespace rega
{

void NativeImages::place(ftxui::Screen& screen,
                         uint32_t id,
                         int x,
                         int y,
                         int cols,
                         int rows,
                         int shownCols,
                         int shownRows,
                         std::shared_ptr<const Rgba8Image> bitmap)
{
	if(cols <= 0 || rows <= 0 || shownCols <= 0 || shownRows <= 0 || !bitmap)
	{
		return;
	}
	const ftxui::Color fg = ftxui::Color::RGB(static_cast<uint8_t>(id >> 16),
	                                          static_cast<uint8_t>(id >> 8),
	                                          static_cast<uint8_t>(id));
	const int x1 = std::min(x + std::min(cols, shownCols), screen.dimx());
	const int y1 = std::min(y + std::min(rows, shownRows), screen.dimy());
	for(int r = std::max(0, y); r < y1; ++r)
	{
		for(int c = std::max(0, x); c < x1; ++c)
		{
			auto& px = screen.PixelAt(c, r);
			px = ScreenCell{};
			px.character = NATIVE_CELL;
			px.foreground_color = fg;
		}
	}
	m_placed.push_back({id, x, y, cols, rows, std::move(bitmap)});
}

uint32_t nativeCellId(uint8_t r, uint8_t g, uint8_t b) noexcept
{
	return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
	       b;
}

} // namespace rega
