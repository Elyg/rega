#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace rega
{

/// Contact-sheet geometry, in terminal cells: tiles of one size in a grid,
/// filled left to right then down, a thin gap between them. Pure arithmetic,
/// no drawing.
struct SheetLayout
{
	int m_count = 0;
	int m_cols = 1, m_rows = 1;
	int m_tileW = 1, m_tileH = 1; ///< cells
	int m_gapX = 0, m_gapY = 0;   ///< cells between columns / rows

	int width() const
	{
		return m_cols * m_tileW + (m_cols - 1) * m_gapX;
	}
	int height() const
	{
		return m_rows * m_tileH + (m_rows - 1) * m_gapY;
	}
	/// Top-left cell of tile `i` on the sheet.
	std::pair<int, int> origin(int i) const
	{
		return {(i % m_cols) * (m_tileW + m_gapX),
		        (i / m_cols) * (m_tileH + m_gapY)};
	}
	/// The tile at sheet point (x, y); -1 between or past the tiles.
	int tileAt(double x, double y) const;
	bool operator==(const SheetLayout&) const = default;
};

/// Lay out tiles whose frames are `frames` (pixel width, height) in an area
/// of `areaW` x `areaH` cells of `pxX` x `pxY` pixels: the column count that
/// shows the most image overall, each tile the size of the largest image
/// drawn in it, a cell row between rows and about as many pixels between
/// columns. At most about `maxVisible` tiles fit on screen; more make a
/// sheet taller than the area, to pan through.
SheetLayout layoutSheet(const std::vector<std::pair<double, double>>& frames,
                        int areaW,
                        int areaH,
                        double pxX,
                        double pxY,
                        int maxVisible);

/// Where the sheet sits in its area: `m_zoom` x the fitted size (1 = fit),
/// sheet point (`m_cx`, `m_cy`) at the middle of the area.
struct SheetView
{
	double m_zoom = 1.0;
	double m_cx = 0, m_cy = 0;
	bool operator==(const SheetView&) const = default;
};

/// Tiles a fitted sheet shows at most: each one on screen is its own kitty
/// image, and image ids are 8-bit (1..255, see kitty::Transmitter::allocId).
/// Past this many, the sheet runs off the bottom.
constexpr int MAX_VISIBLE_TILES = 180;

/// The largest zoom (tiles get huge: a pixel-peeping limit, not a layout one).
constexpr double MAX_SHEET_ZOOM = 512.0;

/// An area of `m_w` x `m_h` cells.
struct SheetArea
{
	int m_w = 1, m_h = 1;
};

/// The least zoom of a sheet that fits its area: tiles a quarter of fitted,
/// the sheet centred. One that runs off the bottom stops at fit: zoomed out,
/// it would put more tiles on screen than kitty has image ids for.
constexpr double MIN_SHEET_ZOOM = 0.25;

/// Keep the zoom in [MIN_SHEET_ZOOM or 1, MAX_SHEET_ZOOM] (see
/// MIN_SHEET_ZOOM) and the sheet over the area: centred
/// along an axis it fits in, else with no blank beyond its edges.
void clampSheetView(SheetView& v, const SheetLayout& l, SheetArea a);
/// Fit: zoom 1, the top of the sheet (all of it, when it fits).
SheetView fitSheetView(const SheetLayout& l, SheetArea a);
/// Zoom by `factor`, keeping the sheet point under area cell (ax, ay) put.
void zoomSheetView(SheetView& v,
                   const SheetLayout& l,
                   SheetArea a,
                   double factor,
                   double ax,
                   double ay);
/// Move the view by (dx, dy) area cells.
void panSheetView(
    SheetView& v, const SheetLayout& l, SheetArea a, double dx, double dy);

/// Tile `i` on screen: `m_x0..m_x1` x `m_y0..m_y1` the whole tile in area
/// cells (fractional, maybe past the area), `m_box*` the whole cells of it
/// inside the area (half-open).
struct TilePlacement
{
	double m_x0 = 0, m_y0 = 0, m_x1 = 0, m_y1 = 0;
	int m_boxX0 = 0, m_boxY0 = 0, m_boxX1 = 0, m_boxY1 = 0;
};
/// nullopt when no whole cell of tile `i` is inside the area.
std::optional<TilePlacement>
placeTile(const SheetLayout& l, const SheetView& v, SheetArea a, int i);
/// Sheet point under area cell (ax, ay) (a cell's centre: + 0.5).
std::pair<double, double>
sheetPointAt(const SheetView& v, SheetArea a, double ax, double ay);

} // namespace rega
