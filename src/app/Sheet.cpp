#include "app/Sheet.h"

#include <algorithm>
#include <cmath>

namespace rega
{

int SheetLayout::tileAt(double x, double y) const
{
	if(x < 0 || y < 0 || m_tileW <= 0 || m_tileH <= 0)
	{
		return -1;
	}
	const int pitchX = m_tileW + m_gapX, pitchY = m_tileH + m_gapY;
	const int c = static_cast<int>(x / pitchX);
	const int r = static_cast<int>(y / pitchY);
	if(c >= m_cols || r >= m_rows || x - c * pitchX >= m_tileW ||
	   y - r * pitchY >= m_tileH)
	{
		return -1; // past the sheet, or in a gap
	}
	const int i = r * m_cols + c;
	return i < m_count ? i : -1;
}

namespace
{

// layoutSheet() with tiles `gapX` x `gapY` cells apart.
SheetLayout layoutWithGaps(const std::vector<std::pair<double, double>>& frames,
                           int areaW,
                           int areaH,
                           double pxX,
                           double pxY,
                           int maxVisible,
                           int gapX,
                           int gapY)
{
	SheetLayout l;
	l.m_count = static_cast<int>(frames.size());
	l.m_gapX = gapX;
	l.m_gapY = gapY;
	// The cells c tiles share along an axis, less the gaps between them.
	auto room = [](int area, int c, int gap)
	{ return std::max(c, area - (c - 1) * gap); };
	// Past maxVisible, size the tiles as if only that many had to fit; the
	// rest go below, off screen.
	const int shown = std::clamp(l.m_count, 1, std::max(1, maxVisible));
	const std::vector<std::pair<double, double>> sample(frames.begin(),
	                                                    frames.begin() + shown);
	// The column count that shows the most image overall (ties: fewer
	// columns). It depends on every image, not the selected one, so it stays
	// put as the selection moves.
	double bestArea = -1;
	for(int c = 1; c <= shown; ++c)
	{
		const int r = (shown + c - 1) / c;
		const double boxW = room(areaW, c, l.m_gapX) * pxX / c;
		const double boxH = room(areaH, r, l.m_gapY) * pxY / r;
		double area = 0;
		for(const auto& [w, h] : sample)
		{
			const double sc = std::min(boxW / w, boxH / h);
			area += sc * w * sc * h;
		}
		if(area > bestArea * 1.0001)
		{
			bestArea = area;
			l.m_cols = c;
		}
	}
	const int shownRows = (shown + l.m_cols - 1) / l.m_cols;
	const int roomW = room(areaW, l.m_cols, l.m_gapX);
	const int roomH = room(areaH, shownRows, l.m_gapY);
	const double boxW = roomW * pxX / l.m_cols, boxH = roomH * pxY / shownRows;
	// Shrink the shared box to the largest image actually drawn in it, so
	// neighbours nearly touch.
	double usedW = 1, usedH = 1;
	for(const auto& [w, h] : frames)
	{
		const double sc = std::min(boxW / w, boxH / h);
		usedW = std::max(usedW, sc * w);
		usedH = std::max(usedH, sc * h);
	}
	l.m_tileW = std::clamp(static_cast<int>(usedW / pxX), 1, roomW / l.m_cols);
	l.m_tileH = std::clamp(static_cast<int>(usedH / pxY), 1, roomH / shownRows);
	l.m_rows = (l.m_count + l.m_cols - 1) / l.m_cols;
	return l;
}

} // namespace

SheetLayout layoutSheet(const std::vector<std::pair<double, double>>& frames,
                        int areaW,
                        int areaH,
                        double pxX,
                        double pxY,
                        int maxVisible)
{
	if(frames.empty())
	{
		return {};
	}
	areaW = std::max(1, areaW);
	areaH = std::max(1, areaH);
	// Tiles apart by a cell row, and by as many columns as come closest to
	// it in pixels, so the gaps look alike both ways...
	const int gapY = 1;
	const int gapX = std::max(1, static_cast<int>(std::lround(pxY / pxX)));
	SheetLayout l =
	    layoutWithGaps(frames, areaW, areaH, pxX, pxY, maxVisible, gapX, gapY);
	// ...while that stays thin beside them. Tiny tiles (a long sequence)
	// touch: a gap would take much of each, and fit more of them on screen
	// than kitty has image ids for (see MAX_VISIBLE_TILES).
	constexpr int MIN_GAPS_PER_TILE = 6;
	if(l.m_tileW < MIN_GAPS_PER_TILE * gapX ||
	   l.m_tileH < MIN_GAPS_PER_TILE * gapY)
	{
		l = layoutWithGaps(frames, areaW, areaH, pxX, pxY, maxVisible, 0, 0);
	}
	return l;
}

namespace
{

// See MIN_SHEET_ZOOM.
double minSheetZoom(const SheetLayout& l, SheetArea a)
{
	return l.width() <= a.m_w && l.height() <= a.m_h ? MIN_SHEET_ZOOM : 1.0;
}

} // namespace

void clampSheetView(SheetView& v, const SheetLayout& l, SheetArea a)
{
	v.m_zoom = std::clamp(v.m_zoom, minSheetZoom(l, a), MAX_SHEET_ZOOM);
	auto axis = [](double& c, double sheet, double area, double zoom)
	{
		const double half = area / (2 * zoom); // sheet cells either side
		c = sheet <= 2 * half ? sheet / 2 : std::clamp(c, half, sheet - half);
	};
	axis(v.m_cx, l.width(), a.m_w, v.m_zoom);
	axis(v.m_cy, l.height(), a.m_h, v.m_zoom);
}

SheetView fitSheetView(const SheetLayout& l, SheetArea a)
{
	SheetView v;
	v.m_cx = l.width() / 2.0;
	v.m_cy = 0; // the top, once clamped
	clampSheetView(v, l, a);
	return v;
}

std::pair<double, double>
sheetPointAt(const SheetView& v, SheetArea a, double ax, double ay)
{
	return {v.m_cx + (ax - a.m_w / 2.0) / v.m_zoom,
	        v.m_cy + (ay - a.m_h / 2.0) / v.m_zoom};
}

void zoomSheetView(SheetView& v,
                   const SheetLayout& l,
                   SheetArea a,
                   double factor,
                   double ax,
                   double ay)
{
	const auto [px, py] = sheetPointAt(v, a, ax, ay);
	v.m_zoom =
	    std::clamp(v.m_zoom * factor, minSheetZoom(l, a), MAX_SHEET_ZOOM);
	if(std::abs(v.m_zoom - 1.0) < 1e-9)
	{
		v.m_zoom = 1.0; // back at fit exactly, steps in and out notwithstanding
	}
	v.m_cx = px - (ax - a.m_w / 2.0) / v.m_zoom;
	v.m_cy = py - (ay - a.m_h / 2.0) / v.m_zoom;
	clampSheetView(v, l, a);
}

void panSheetView(
    SheetView& v, const SheetLayout& l, SheetArea a, double dx, double dy)
{
	v.m_cx += dx / v.m_zoom;
	v.m_cy += dy / v.m_zoom;
	clampSheetView(v, l, a);
}

std::optional<TilePlacement>
placeTile(const SheetLayout& l, const SheetView& v, SheetArea a, int i)
{
	if(i < 0 || i >= l.m_count)
	{
		return std::nullopt;
	}
	const auto [sx, sy] = l.origin(i);
	TilePlacement p;
	p.m_x0 = a.m_w / 2.0 + (sx - v.m_cx) * v.m_zoom;
	p.m_y0 = a.m_h / 2.0 + (sy - v.m_cy) * v.m_zoom;
	p.m_x1 = p.m_x0 + l.m_tileW * v.m_zoom;
	p.m_y1 = p.m_y0 + l.m_tileH * v.m_zoom;
	// Whole cells: edges rounded, so neighbours share them exactly.
	auto cell = [](double e) { return static_cast<int>(std::lround(e)); };
	p.m_boxX0 = std::max(0, cell(p.m_x0));
	p.m_boxY0 = std::max(0, cell(p.m_y0));
	p.m_boxX1 = std::min(a.m_w, cell(p.m_x1));
	p.m_boxY1 = std::min(a.m_h, cell(p.m_y1));
	if(p.m_boxX1 <= p.m_boxX0 || p.m_boxY1 <= p.m_boxY0)
	{
		return std::nullopt;
	}
	return p;
}

} // namespace rega
