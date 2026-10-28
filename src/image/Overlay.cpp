#include "image/Overlay.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>

namespace rega
{

namespace
{

struct Glyph
{
	std::string m_text;
	bool m_dim = false;
	int m_width = 1;
};
using Glyphs = std::vector<Glyph>;

Glyphs toGlyphs(const OverlayLine& line)
{
	Glyphs out;
	for(const auto& run : line)
	{
		for(auto& g : ftxui::Utf8ToGlyphs(run.m_text))
		{
			const int w = std::max(1, ftxui::string_width(g));
			out.push_back({std::move(g), run.m_dim, w});
		}
	}
	return out;
}

int widthOf(const Glyphs& g)
{
	int w = 0;
	for(const auto& x : g)
	{
		w += x.m_width;
	}
	return w;
}

// Cut to at most `maxW` cells, ending in … when something was dropped.
Glyphs truncated(const Glyphs& g, int maxW)
{
	if(maxW <= 0)
	{
		return {};
	}
	if(widthOf(g) <= maxW)
	{
		return g;
	}
	Glyphs out;
	int used = 0;
	for(const auto& x : g)
	{
		if(used + x.m_width > maxW - 1)
		{
			break;
		}
		out.push_back(x);
		used += x.m_width;
	}
	out.push_back({"…", false, 1});
	return out;
}

void place(std::vector<OverlayCell>& out, const Glyphs& g, int x, int y)
{
	for(const auto& c : g)
	{
		out.push_back({x, y, c.m_text, c.m_dim});
		for(int k = 1; k < c.m_width; ++k)
		{
			out.push_back({x + k, y, "", c.m_dim});
		}
		x += c.m_width;
	}
}

// One row: left / centre / right texts within [x0, x0 + w).
void layoutRow(std::vector<OverlayCell>& out,
               int x0,
               int w,
               int y,
               const Glyphs& left,
               const Glyphs& centre,
               const Glyphs& right)
{
	const bool hasL = !left.empty(), hasR = !right.empty();
	const int capL = hasR ? w / 2 : w;
	const Glyphs l = truncated(left, capL);
	const int lw = widthOf(l);
	const int capR = hasL ? w - lw - 1 : w;
	const Glyphs r = truncated(right, capR);
	const int rw = widthOf(r);
	place(out, l, x0, y);
	place(out, r, x0 + w - rw, y);
	if(centre.empty())
	{
		return;
	}
	const int gapStart = x0 + lw + (lw ? 1 : 0);
	const int gapEnd = x0 + w - rw - (rw ? 1 : 0); // exclusive
	const int avail = gapEnd - gapStart;
	const int natural = widthOf(centre);
	if(natural > avail && avail < 3)
	{
		return;
	}
	const Glyphs c = truncated(centre, avail);
	const int cw = widthOf(c);
	const int x = std::clamp(x0 + (w - cw) / 2, gapStart, gapEnd - cw);
	place(out, c, x, y);
}

} // namespace

const char* slotKey(Slot s)
{
	static const char* const KEYS[] = {"tl", "tc", "tr", "bl", "bc", "br"};
	return KEYS[static_cast<int>(s)];
}

const char* slotName(Slot s)
{
	static const char* const NAMES[] = {"top left",
	                                    "top centre",
	                                    "top right",
	                                    "bottom left",
	                                    "bottom centre",
	                                    "bottom right"};
	return NAMES[static_cast<int>(s)];
}

bool isTopSlot(Slot s)
{
	return s == Slot::TL || s == Slot::TC || s == Slot::TR;
}

bool overlayEmpty(const OverlayText& t)
{
	return std::all_of(t.begin(),
	                   t.end(),
	                   [](const auto& lines) { return lines.empty(); });
}

std::vector<OverlayCell>
layoutOverlay(int x0, int y0, int w, int h, const OverlayText& text)
{
	std::vector<OverlayCell> out;
	const int ix = w >= 3 ? 1 : 0, iy = h >= 3 ? 1 : 0;
	const int innerX = x0 + ix, innerY = y0 + iy;
	const int innerW = w - 2 * ix, innerH = h - 2 * iy;
	if(innerW <= 0 || innerH <= 0 || overlayEmpty(text))
	{
		return out;
	}
	std::array<std::vector<Glyphs>, SLOT_COUNT> g;
	for(int s = 0; s < SLOT_COUNT; ++s)
	{
		for(const auto& line : text[s])
		{
			g[s].push_back(toGlyphs(line));
		}
	}
	auto count = [&](Slot s)
	{ return static_cast<int>(g[static_cast<int>(s)].size()); };
	const int topN =
	    std::max({count(Slot::TL), count(Slot::TC), count(Slot::TR)});
	const int bottomN =
	    std::max({count(Slot::BL), count(Slot::BC), count(Slot::BR)});
	const int bottomRows = std::min(bottomN, innerH);
	const int topRows = std::min(topN, innerH - bottomRows);

	static const Glyphs NONE;
	// Top: line i on row i.
	auto topLine = [&](Slot s, int i) -> const Glyphs&
	{
		const auto& v = g[static_cast<int>(s)];
		return i < static_cast<int>(v.size()) ? v[i] : NONE;
	};
	for(int i = 0; i < topRows; ++i)
	{
		layoutRow(out,
		          innerX,
		          innerW,
		          innerY + i,
		          topLine(Slot::TL, i),
		          topLine(Slot::TC, i),
		          topLine(Slot::TR, i));
	}
	// Bottom: each slot's last line on the last row (k counts up from it).
	auto bottomLine = [&](Slot s, int k) -> const Glyphs&
	{
		const auto& v = g[static_cast<int>(s)];
		const int n = static_cast<int>(v.size());
		return k < n ? v[n - 1 - k] : NONE;
	};
	for(int k = 0; k < bottomRows; ++k)
	{
		layoutRow(out,
		          innerX,
		          innerW,
		          innerY + innerH - 1 - k,
		          bottomLine(Slot::BL, k),
		          bottomLine(Slot::BC, k),
		          bottomLine(Slot::BR, k));
	}
	return out;
}

} // namespace rega
