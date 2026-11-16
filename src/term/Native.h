#pragma once

#include "image/Render.h"

#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rega
{

/// One cell of an ftxui::Screen: ftxui::Pixel, ftxui::Cell in later FTXUI.
using ScreenCell =
    std::remove_cvref_t<decltype(std::declval<ftxui::Screen&>().PixelAt(0, 0))>;

/// Pictures for the window (`--gui`, GraphicsMode::NATIVE) to draw itself:
/// what ImageSlot and BitmapSlot hand over in place of kitty escapes. A slot
/// puts NATIVE_CELL in each cell its picture covers, the slot's id in the
/// cell's foreground colour (as kitty placeholders carry an image id); the
/// window draws each picture under the cells that still hold its marker
/// after the whole frame drew, so a popup drawn over a picture hides it there
/// as it does in the terminal.
class NativeImages
{
public:
	/// Where a picture goes, in window pixels from its box's top-left.
	struct Rect
	{
		double m_x = 0, m_y = 0, m_w = 0, m_h = 0;
	};
	/// One picture on screen this frame.
	struct Placement
	{
		uint32_t m_id = 0;    ///< the slot's
		int m_x = 0, m_y = 0; ///< top-left cell
		int m_cols = 0, m_rows = 0;
		/// Pixels, m_cols x m_rows cells of TermCaps::m_cellW x m_cellH.
		std::shared_ptr<const Rgba8Image> m_bitmap;
		/// Set: the bitmap drawn there instead, clipped to the box (a
		/// picture made for the view before, moved and scaled to the view
		/// now until its own is ready: a pan follows the pointer at once).
		std::optional<Rect> m_at;
		/// With m_at: the box filled first (TILE_FILL), as a tile still
		/// rendering looks.
		bool m_fill = false;
	};

	/// A frame starts: forget the last one's placements (UI thread).
	void begin()
	{
		m_placed.clear();
	}
	/// `bitmap` covers `cols` x `rows` cells from (`x`, `y`); it shows in
	/// the first `shownCols` x `shownRows` of them, which get marked on
	/// `screen` (less while a picture for a resized box is on its way).
	void place(ftxui::Screen& screen,
	           uint32_t id,
	           int x,
	           int y,
	           int cols,
	           int rows,
	           int shownCols,
	           int shownRows,
	           std::shared_ptr<const Rgba8Image> bitmap,
	           std::optional<Rect> at = std::nullopt,
	           bool fill = false);
	[[nodiscard]] const std::vector<Placement>& placements() const noexcept
	{
		return m_placed;
	}
	/// A fresh id for a slot (never 0).
	[[nodiscard]] uint32_t allocId() noexcept
	{
		return ++m_nextId;
	}

private:
	std::vector<Placement> m_placed;
	uint32_t m_nextId = 0;
};

/// A tile's box while its picture renders (a shade above the window's grey).
inline constexpr uint8_t TILE_FILL[3] = {40, 42, 47};

/// The character a cell under a native picture holds (U+100000, private use).
inline const std::string NATIVE_CELL = "\xF4\x80\x80\x80";
/// The slot id a NATIVE_CELL cell's foreground colour carries.
[[nodiscard]] uint32_t nativeCellId(uint8_t r, uint8_t g, uint8_t b) noexcept;

} // namespace rega
