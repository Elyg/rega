#include "gui/Font.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

namespace rega::gui
{

struct Font::Face
{
	stbtt_fontinfo m_info{};
};

namespace
{

// Box drawing (U+2500-U+257F): line weights left, right, up, down; 0 none,
// 1 light, 2 heavy, 3 double, 4 arc (rounded corner). Dashed lines draw
// solid; the diagonals (╱ ╲ ╳) come from the font.
constexpr std::array<const char*, 128> BOX = {
    "1100", "2200",  "0011",  "0022",  "1100", "2200", "0011", "0022", // 2500
    "1100", "2200",  "0011",  "0022",  "0101", "0201", "0102", "0202", // 2508
    "1001", "2001",  "1002",  "2002",  "0110", "0210", "0120", "0220", // 2510
    "1010", "2010",  "1020",  "2020",  "0111", "0211", "0121", "0112", // 2518
    "0122", "0221",  "0212",  "0222",  "1011", "2011", "1021", "1012", // 2520
    "1022", "2021",  "2012",  "2022",  "1101", "2101", "1201", "2201", // 2528
    "1102", "2102",  "1202",  "2202",  "1110", "2110", "1210", "2210", // 2530
    "1120", "2120",  "1220",  "2220",  "1111", "2111", "1211", "2211", // 2538
    "1121", "1112",  "1122",  "2121",  "1221", "2112", "1212", "2221", // 2540
    "2212", "2122",  "1222",  "2222",  "1100", "2200", "0011", "0022", // 2548
    "3300", "0033",  "0301",  "0103",  "0303", "3001", "1003", "3003", // 2550
    "0310", "0130",  "0330",  "3010",  "1030", "3030", "0311", "0133", // 2558
    "0333", "3011",  "1033",  "3033",  "3301", "1103", "3303", "3310", // 2560
    "1130", "3330",  "3311",  "1133",  "3333", "0404", "4004", "4040", // 2568
    "0440", nullptr, nullptr, nullptr, "1000", "0010", "0100", "0001", // 2570
    "2000", "0020",  "0200",  "0002",  "1200", "0012", "2100", "0021", // 2578
};

// A filled rectangle of coverage 255, clipped to the cell.
void fill(std::vector<uint8_t>& cell,
          int cw,
          int ch,
          int x0,
          int y0,
          int x1,
          int y1,
          uint8_t value = 255)
{
	x0 = std::clamp(x0, 0, cw);
	x1 = std::clamp(x1, 0, cw);
	y0 = std::clamp(y0, 0, ch);
	y1 = std::clamp(y1, 0, ch);
	for(int y = y0; y < y1; ++y)
	{
		for(int x = x0; x < x1; ++x)
		{
			uint8_t& p = cell[static_cast<size_t>(y) * cw + x];
			p = std::max(p, value);
		}
	}
}

} // namespace

Font::Font(std::span<const uint8_t> regular, std::span<const uint8_t> bold)
    : m_regularData(regular.begin(), regular.end()),
      m_boldData(bold.begin(), bold.end()), m_faces(2)
{
	const std::vector<uint8_t>* data[2] = {&m_regularData, &m_boldData};
	for(int i = 0; i < 2; ++i)
	{
		const unsigned char* d = data[i]->data();
		if(data[i]->empty() ||
		   !stbtt_InitFont(&m_faces[i].m_info,
		                   d,
		                   stbtt_GetFontOffsetForIndex(d, 0)))
		{
			throw std::runtime_error("the built-in font did not load");
		}
	}
	setPixelHeight(15.0f);
}

Font::~Font() = default;

void Font::setPixelHeight(float pixelHeight)
{
	m_pixelHeight = std::max(4.0f, pixelHeight);
	const stbtt_fontinfo& info = m_faces[0].m_info;
	const float scale = stbtt_ScaleForMappingEmToPixels(&info, m_pixelHeight);
	int ascent = 0, descent = 0, gap = 0;
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
	int advance = 0, lsb = 0;
	stbtt_GetCodepointHMetrics(&info, 'M', &advance, &lsb);
	m_cellW = std::max(1, static_cast<int>(std::lround(advance * scale)));
	const float lineH = (ascent - descent + gap) * scale;
	m_cellH = std::max(2, static_cast<int>(std::lround(lineH)));
	// The extra (rounding, line gap) split above and below the glyphs.
	m_baseline = static_cast<int>(std::lround(
	    ascent * scale + (m_cellH - (ascent - descent) * scale) / 2));
	clearAtlas();
}

void Font::clearAtlas()
{
	// Big enough for every glyph a screen shows at large sizes.
	m_atlasSize = m_cellH > 48 ? 4096 : m_cellH > 24 ? 2048 : 1024;
	m_atlas.assign(static_cast<size_t>(m_atlasSize) * m_atlasSize, 0);
	m_penX = m_penY = m_rowH = 0;
	m_glyphs.clear();
	m_blank.clear();
	m_dirty = true;
	m_reset = true;
}

bool Font::allocate(int w, int h, int& x, int& y)
{
	constexpr int PAD = 1; // no bleeding between glyphs when scaled
	if(m_penX + w + PAD > m_atlasSize)
	{
		m_penX = 0;
		m_penY += m_rowH + PAD;
		m_rowH = 0;
	}
	if(m_penY + h + PAD > m_atlasSize || w + PAD > m_atlasSize)
	{
		return false;
	}
	x = m_penX;
	y = m_penY;
	m_penX += w + PAD;
	m_rowH = std::max(m_rowH, h);
	return true;
}

Font::Glyph* Font::add(uint64_t key,
                       const std::vector<uint8_t>& pixels,
                       int w,
                       int h,
                       int offX,
                       int offY)
{
	int x = 0, y = 0;
	if(!allocate(w, h, x, y))
	{
		// Full: start again; glyphs of this frame come back on its redraw.
		spdlog::debug("font atlas full: cleared");
		clearAtlas();
		if(!allocate(w, h, x, y))
		{
			return nullptr;
		}
	}
	for(int r = 0; r < h; ++r)
	{
		std::copy_n(&pixels[static_cast<size_t>(r) * w],
		            w,
		            &m_atlas[static_cast<size_t>(y + r) * m_atlasSize + x]);
	}
	m_dirty = true;
	Glyph& g = m_glyphs[key];
	g = {x, y, w, h, offX, offY};
	return &g;
}

const Font::Glyph* Font::glyph(char32_t cp, bool bold)
{
	if(cp == U' ' || cp == 0 || cp == 0x00A0)
	{
		return nullptr;
	}
	const uint64_t key = (static_cast<uint64_t>(cp) << 1) | (bold ? 1 : 0);
	if(auto it = m_glyphs.find(key); it != m_glyphs.end())
	{
		return &it->second;
	}
	if(m_blank.contains(key))
	{
		return nullptr;
	}

	std::vector<uint8_t> cell;
	if(drawShape(cp, cell))
	{
		return add(key, cell, m_cellW, m_cellH, 0, 0);
	}

	const stbtt_fontinfo* info = &m_faces[bold ? 1 : 0].m_info;
	int index = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
	if(index == 0 && bold)
	{
		info = &m_faces[0].m_info; // the regular face may have it
		index = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
	}
	if(index == 0)
	{
		spdlog::debug("font: no glyph for U+{:04X}", static_cast<uint32_t>(cp));
		index = stbtt_FindGlyphIndex(info, '?');
	}
	const float scale = stbtt_ScaleForMappingEmToPixels(info, m_pixelHeight);
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	stbtt_GetGlyphBitmapBox(info, index, scale, scale, &x0, &y0, &x1, &y1);
	const int w = x1 - x0, h = y1 - y0;
	if(w <= 0 || h <= 0)
	{
		m_blank[key] = true;
		return nullptr;
	}
	std::vector<uint8_t> pixels(static_cast<size_t>(w) * h);
	stbtt_MakeGlyphBitmap(info, pixels.data(), w, h, w, scale, scale, index);
	// Light text on a dark background reads thin when blended in sRGB:
	// lift the coverage a little.
	static const std::array<uint8_t, 256> LIFT = []
	{
		std::array<uint8_t, 256> t{};
		for(int i = 0; i < 256; ++i)
		{
			t[i] = static_cast<uint8_t>(
			    std::lround(255.0 * std::pow(i / 255.0, 1.0 / 1.35)));
		}
		return t;
	}();
	for(uint8_t& p : pixels)
	{
		p = LIFT[p];
	}
	return add(key, pixels, w, h, x0, m_baseline + y0);
}

bool Font::drawShape(char32_t cp, std::vector<uint8_t>& cell) const
{
	const int cw = m_cellW, ch = m_cellH;
	auto blank = [&] { cell.assign(static_cast<size_t>(cw) * ch, 0); };

	// Block elements (U+2580-U+259F).
	if(cp >= 0x2580 && cp <= 0x259F)
	{
		blank();
		auto rows = [&](double from, double to)
		{
			fill(cell,
			     cw,
			     ch,
			     0,
			     static_cast<int>(std::lround(from * ch)),
			     cw,
			     static_cast<int>(std::lround(to * ch)));
		};
		auto cols = [&](double from, double to)
		{
			fill(cell,
			     cw,
			     ch,
			     static_cast<int>(std::lround(from * cw)),
			     0,
			     static_cast<int>(std::lround(to * cw)),
			     ch);
		};
		const int mx = cw / 2, my = ch / 2;
		auto quads = [&](bool ul, bool ur, bool ll, bool lr)
		{
			if(ul)
			{
				fill(cell, cw, ch, 0, 0, mx, my);
			}
			if(ur)
			{
				fill(cell, cw, ch, mx, 0, cw, my);
			}
			if(ll)
			{
				fill(cell, cw, ch, 0, my, mx, ch);
			}
			if(lr)
			{
				fill(cell, cw, ch, mx, my, cw, ch);
			}
		};
		switch(cp)
		{
			case 0x2580:
				rows(0, 0.5);
				break;
			case 0x2588:
				rows(0, 1);
				break;
			case 0x2590:
				cols(0.5, 1);
				break;
			case 0x2591:
			case 0x2592:
			case 0x2593:
				fill(cell,
				     cw,
				     ch,
				     0,
				     0,
				     cw,
				     ch,
				     cp == 0x2591   ? 64
				     : cp == 0x2592 ? 128
				                    : 192);
				break;
			case 0x2594:
				rows(0, 0.125);
				break;
			case 0x2595:
				cols(0.875, 1);
				break;
			case 0x2596:
				quads(false, false, true, false);
				break;
			case 0x2597:
				quads(false, false, false, true);
				break;
			case 0x2598:
				quads(true, false, false, false);
				break;
			case 0x2599:
				quads(true, false, true, true);
				break;
			case 0x259A:
				quads(true, false, false, true);
				break;
			case 0x259B:
				quads(true, true, true, false);
				break;
			case 0x259C:
				quads(true, true, false, true);
				break;
			case 0x259D:
				quads(false, true, false, false);
				break;
			case 0x259E:
				quads(false, true, true, false);
				break;
			case 0x259F:
				quads(false, true, true, true);
				break;
			default:
				if(cp >= 0x2581 && cp <= 0x2587) // lower 1/8 .. 7/8
				{
					rows(1.0 - (cp - 0x2580) / 8.0, 1);
				}
				else if(cp >= 0x2589 && cp <= 0x258F) // left 7/8 .. 1/8
				{
					cols(0, (0x2590 - cp) / 8.0);
				}
				break;
		}
		return true;
	}

	// Braille (U+2800-U+28FF): 2 x 4 dots.
	if(cp >= 0x2800 && cp <= 0x28FF)
	{
		blank();
		static constexpr int DOT_X[8] = {0, 0, 0, 1, 1, 1, 0, 1};
		static constexpr int DOT_Y[8] = {0, 1, 2, 0, 1, 2, 3, 3};
		const int bits = static_cast<int>(cp - 0x2800);
		const int dw = std::max(1, cw / 4), dh = std::max(1, ch / 8);
		for(int b = 0; b < 8; ++b)
		{
			if(bits & (1 << b))
			{
				const int x = (2 * DOT_X[b] + 1) * cw / 4 - dw / 2;
				const int y = (2 * DOT_Y[b] + 1) * ch / 8 - dh / 2;
				fill(cell, cw, ch, x, y, x + dw, y + dh);
			}
		}
		return true;
	}

	// Box drawing.
	if(cp < 0x2500 || cp > 0x257F || !BOX[cp - 0x2500])
	{
		return false;
	}
	blank();
	const char* w = BOX[cp - 0x2500];
	const int light = std::max(1, static_cast<int>(std::lround(ch / 18.0)));
	const int heavy = std::max(light + 1, light * 2);
	const int cx = cw / 2, cy = ch / 2; // where the lines meet
	// The widest line meeting at the centre: the others run to its far side
	// so the joint is solid.
	auto thick = [&](char c) { return c == '2' ? heavy : light; };
	int reachX = 0, reachY = 0; // half-widths of vertical / horizontal lines
	for(int d = 0; d < 4; ++d)
	{
		const int t = w[d] == '0' ? 0 : w[d] == '3' ? light * 3 : thick(w[d]);
		(d < 2 ? reachY : reachX) =
		    std::max(d < 2 ? reachY : reachX, t / 2 + 1);
	}
	if(w[0] == '4' || w[1] == '4')
	{
		// A rounded corner: a quarter circle from the edge midpoints.
		const bool right = w[1] == '4', down = w[3] == '4';
		const double r = std::min(cw, ch) / 2.0;
		const double ox = right ? cx + r : cx - r; // the circle's centre
		const double oy = down ? cy + r : cy - r;
		const double half = light / 2.0;
		for(int y = 0; y < ch; ++y)
		{
			for(int x = 0; x < cw; ++x)
			{
				const double px = x + 0.5, py = y + 0.5;
				double dist;
				const bool inArcX = right ? px <= ox : px >= ox;
				const bool inArcY = down ? py <= oy : py >= oy;
				if(inArcX && inArcY)
				{
					dist = std::abs(std::hypot(px - ox, py - oy) - r);
				}
				else if(!inArcX) // the straight run to the side edge
				{
					dist = std::abs(py - (cy + 0.5));
				}
				else // the straight run to the top / bottom edge
				{
					dist = std::abs(px - (cx + 0.5));
				}
				const double cov = std::clamp(half + 0.5 - dist, 0.0, 1.0);
				uint8_t& p = cell[static_cast<size_t>(y) * cw + x];
				p = std::max(p, static_cast<uint8_t>(std::lround(cov * 255)));
			}
		}
		return true;
	}
	for(int d = 0; d < 4; ++d)
	{
		const char c = w[d];
		if(c == '0')
		{
			continue;
		}
		const bool horizontal = d < 2;
		// Double lines: two light ones a light width either side of centre.
		const int count = c == '3' ? 2 : 1;
		const int t = thick(c);
		for(int k = 0; k < count; ++k)
		{
			const int shift = count == 1 ? 0 : (k == 0 ? -light : light);
			if(horizontal)
			{
				const int y0 = cy - t / 2 + shift;
				const int x0 = d == 0 ? 0 : cx - reachX;
				const int x1 = d == 0 ? cx + reachX : cw;
				fill(cell, cw, ch, x0, y0, x1, y0 + t);
			}
			else
			{
				const int x0 = cx - t / 2 + shift;
				const int y0 = d == 2 ? 0 : cy - reachY;
				const int y1 = d == 2 ? cy + reachY : ch;
				fill(cell, cw, ch, x0, y0, x0 + t, y1);
			}
		}
	}
	return true;
}

std::u32string decodeUtf8(std::string_view s)
{
	std::u32string out;
	out.reserve(s.size());
	for(size_t i = 0; i < s.size();)
	{
		const auto c = static_cast<unsigned char>(s[i]);
		int len = c < 0x80         ? 1
		          : (c >> 5) == 6  ? 2
		          : (c >> 4) == 14 ? 3
		          : (c >> 3) == 30 ? 4
		                           : 0;
		if(len == 0 || i + len > s.size())
		{
			out.push_back(0xFFFD);
			++i;
			continue;
		}
		char32_t cp = len == 1 ? c : c & (0x3F >> (len - 1));
		bool ok = true;
		for(int k = 1; k < len; ++k)
		{
			const auto cc = static_cast<unsigned char>(s[i + k]);
			if((cc & 0xC0) != 0x80)
			{
				ok = false;
				break;
			}
			cp = (cp << 6) | (cc & 0x3F);
		}
		out.push_back(ok ? cp : 0xFFFD);
		i += ok ? len : 1;
	}
	return out;
}

} // namespace rega::gui
