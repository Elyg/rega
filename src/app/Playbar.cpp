#include "app/Playbar.h"

#include "term/Kitty.h"

#include <ftxui/screen/string.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

using namespace ftxui;

namespace rega
{

int trackFrameAt(int cell, int width, int frames)
{
	if(width <= 0 || frames <= 0)
	{
		return 0;
	}
	const auto f = static_cast<std::int64_t>(cell) * frames / width;
	return static_cast<int>(std::clamp<std::int64_t>(f, 0, frames - 1));
}

int evenTrackWidth(int frames, int cells)
{
	if(frames <= 0 || cells <= 0)
	{
		return std::max(1, cells);
	}
	// Whole frames per cell (rounded up), or whole cells per frame.
	const int even = frames > cells
	                     ? (frames + (frames + cells - 1) / cells - 1) /
	                           ((frames + cells - 1) / cells)
	                     : frames * (cells / frames);
	// Not at any price: a bar much shorter than its room looks broken.
	constexpr double MOST_LOST = 0.25;
	return even >= cells * (1.0 - MOST_LOST) ? even : cells;
}

namespace
{

enum class Look
{
	BLANK,
	LABEL,     ///< the sequence's first / last frame: dim
	IN_OUT,    ///< an in / out point: yellow
	CURRENT,   ///< the current frame's number: bold, pale green
	PLAYHEAD,  ///< the current frame on the bar
	CACHED,    ///< decoded, inside in / out
	STORED,    ///< kept in the terminal, inside in / out
	ELSEWHERE, ///< kept in the terminal for another view only
	EMPTY,     ///< not decoded yet, inside in / out
	OUTSIDE    ///< outside in / out
};

// Fixed colours, the same as text and as pixels (a picture can't use the
// theme's). The current frame is a paler green than the decoded ones: the
// theme's bright green is the same as its green in many themes.
using Rgb = std::array<uint8_t, 3>;
constexpr Rgb DECODED = {0x9e, 0xcd, 0x6a};
// Kept in the terminal (local, over ssh): cyan, a hue of its own beside the
// decoded green (remote) and the current frame's pale green.
constexpr Rgb STORED = {0x7d, 0xcf, 0xff};
// Kept for another view (a pan or a zoom away): the same cyan, dimmed.
constexpr Rgb ELSEWHERE = {0x3b, 0x6a, 0x82};
constexpr Rgb CURRENT = {0xd4, 0xf7, 0xcc};
constexpr Rgb NOT_DECODED = {0x58, 0x5b, 0x70};
constexpr Rgb OUT_OF_RANGE = {0x45, 0x47, 0x5a};

Color rgb(const Rgb& c)
{
	return Color::RGB(c[0], c[1], c[2]);
}

Decorator styleOf(Look l)
{
	switch(l)
	{
		case Look::LABEL:
			return dim;
		case Look::IN_OUT:
			return color(Color::Yellow);
		case Look::CURRENT:
			return bold | color(rgb(CURRENT));
		case Look::PLAYHEAD:
			return color(rgb(CURRENT));
		case Look::CACHED:
			return color(rgb(DECODED));
		case Look::STORED:
			return color(rgb(STORED));
		case Look::ELSEWHERE:
			return color(rgb(ELSEWHERE));
		case Look::EMPTY:
			return color(rgb(NOT_DECODED));
		case Look::OUTSIDE:
			return color(rgb(OUT_OF_RANGE));
		case Look::BLANK:
			break;
	}
	return nothing;
}

// A row of glyphs, each with a look, drawn as one text per run of a look.
struct Row
{
	std::vector<std::string> m_glyphs;
	std::vector<Look> m_looks;

	explicit Row(int width)
	    : m_glyphs(std::max(0, width), " "),
	      m_looks(std::max(0, width), Look::BLANK)
	{
	}
	int width() const
	{
		return static_cast<int>(m_glyphs.size());
	}
	// ASCII `s` from column `x` on (cut at the end).
	void put(int x, const std::string& s, Look look)
	{
		for(int i = 0; i < static_cast<int>(s.size()); ++i)
		{
			if(x + i >= 0 && x + i < width())
			{
				m_glyphs[x + i] = std::string(1, s[i]);
				m_looks[x + i] = look;
			}
		}
	}
	Element element() const
	{
		Elements runs;
		for(int x = 0; x < width();)
		{
			std::string run;
			const Look look = m_looks[x];
			for(; x < width() && m_looks[x] == look; ++x)
			{
				run += m_glyphs[x];
			}
			runs.push_back(text(run) | styleOf(look));
		}
		return hbox(std::move(runs));
	}
};

// Frames [lo, hi] that cell (or pixel column) `c` of `w` stands for.
std::pair<int, int> framesOf(int c, int w, int n)
{
	const int lo = trackFrameAt(c, w, n);
	const int hi =
	    c + 1 < w ? std::max(lo, trackFrameAt(c + 1, w, n) - 1) : n - 1;
	return {lo, hi};
}

// Whether `per` (one flag a frame) holds for every frame of [lo, hi] that
// playback reaches (inside the range). A cell across in / out counts only
// its frames inside.
bool allOf(
    const std::vector<bool>& per, const PlaybarState& s, int n, int lo, int hi)
{
	if(static_cast<int>(per.size()) != n)
	{
		return false;
	}
	for(int f = std::max(lo, s.m_range.m_first);
	    f <= std::min(hi, s.m_range.m_last);
	    ++f)
	{
		if(!per[f])
		{
			return false;
		}
	}
	return true;
}

bool decoded(const PlaybarState& s, int n, int lo, int hi)
{
	return allOf(s.m_cached, s, n, lo, hi);
}

bool stored(const PlaybarState& s, int n, int lo, int hi)
{
	return allOf(s.m_stored, s, n, lo, hi);
}

bool storedElsewhere(const PlaybarState& s, int n, int lo, int hi)
{
	return allOf(s.m_storedElsewhere, s, n, lo, hi);
}

bool inRange(const PlaybarState& s, int lo, int hi)
{
	return hi >= s.m_range.m_first && lo <= s.m_range.m_last;
}

} // namespace

namespace
{

// Numbers in the picture: a 5x7 font of the digits and a minus (frame
// numbers are nothing else). Bit 4 is the leftmost column.
struct Glyph
{
	char m_char;
	std::array<uint8_t, 7> m_rows;
};
constexpr Glyph FONT[] = {
    {'0', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
};
constexpr int GLYPH_W = 5, GLYPH_H = 7;

const Glyph* glyphOf(char c)
{
	for(const Glyph& g : FONT)
	{
		if(g.m_char == c)
		{
			return &g;
		}
	}
	return nullptr;
}

// The labels' colours in the picture (as text: dim, and the theme's yellow).
constexpr Rgb LABEL_GREY = {0x93, 0x99, 0xb2};
constexpr Rgb IN_OUT_YELLOW = {0xf9, 0xe2, 0xaf};

// An RGBA picture, drawn on with filled rectangles.
struct Canvas
{
	Rgba8Image& m_img;

	// [x0, x1) x [y0, y1) in `c`, clipped.
	void fill(int x0, int x1, int y0, int y1, const Rgb& c)
	{
		for(int y = std::max(0, y0); y < std::min(y1, m_img.m_height); ++y)
		{
			for(int x = std::max(0, x0); x < std::min(x1, m_img.m_width); ++x)
			{
				uint8_t* p =
				    &m_img.m_pixels[(static_cast<size_t>(y) * m_img.m_width + x) *
				                    4];
				p[0] = c[0];
				p[1] = c[1];
				p[2] = c[2];
				p[3] = 255;
			}
		}
	}
	// `t` at (x, y), each font pixel `scale` square; bold thickens strokes.
	void
	text(int x, int y, const std::string& t, int scale, const Rgb& c, bool bold)
	{
		const int extra = bold ? std::max(1, scale / 2) : 0;
		for(size_t i = 0; i < t.size(); ++i)
		{
			const Glyph* g = glyphOf(t[i]);
			const int gx = x + static_cast<int>(i) * advance(scale);
			for(int r = 0; g && r < GLYPH_H; ++r)
			{
				for(int col = 0; col < GLYPH_W; ++col)
				{
					if(g->m_rows[r] >> (GLYPH_W - 1 - col) & 1)
					{
						fill(gx + col * scale,
						     gx + (col + 1) * scale + extra,
						     y + r * scale,
						     y + (r + 1) * scale,
						     c);
					}
				}
			}
		}
	}
	static int advance(int scale)
	{
		return (GLYPH_W + 1) * scale;
	}
	static int textWidth(const std::string& t, int scale)
	{
		return t.empty() ? 0
		                 : static_cast<int>(t.size()) * advance(scale) - scale;
	}
};

} // namespace

Rgba8Image paintPlaybar(const PlaybarState& s, int width, int height)
{
	Rgba8Image img;
	img.m_width = std::max(0, width);
	img.m_height = std::max(0, height);
	img.m_pixels.assign(static_cast<size_t>(img.m_width) * img.m_height * 4, 0);
	if(width <= 0 || height <= 1)
	{
		return img;
	}
	Canvas cv{img};
	const int n = std::max(1, s.m_frames);
	// Two rows: the numbers, then the bar in the top half of its row (an
	// eighth outside in / out), as the text playbar draws them.
	const int row = height / 2;
	const int band = std::max(1, row / 2);
	const int thin = std::max(1, row / 8);
	for(int x = 0; x < width; ++x)
	{
		const auto [lo, hi] = framesOf(x, width, n);
		if(!inRange(s, lo, hi))
		{
			cv.fill(x, x + 1, row, row + thin, OUT_OF_RANGE);
		}
		else
		{
			cv.fill(x,
			        x + 1,
			        row,
			        row + band,
			        stored(s, n, lo, hi)            ? STORED
			        : storedElsewhere(s, n, lo, hi) ? ELSEWHERE
			        : decoded(s, n, lo, hi)         ? DECODED
			                                        : NOT_DECODED);
		}
	}
	// The current frame's share of the bar, at least a few pixels wide.
	constexpr int MIN_PLAYHEAD = 3;
	auto columnOf = [&](int f)
	{ return static_cast<int>(static_cast<std::int64_t>(f) * width / n); };
	int x0 = columnOf(s.m_frame);
	int x1 = std::max(x0 + MIN_PLAYHEAD, columnOf(s.m_frame + 1));
	if(x1 > width)
	{
		x0 = std::max(0, x0 - (x1 - width));
		x1 = width;
	}
	cv.fill(x0, x1, row, row + band, CURRENT);

	// The numbers, about the terminal's text size, sitting just above the
	// bar: the current one centred on the playhead (it glides with it), the
	// range's ends at theirs unless it is in the way.
	auto label = [&](int f)
	{
		std::string l = s.m_label ? s.m_label(f) : std::string();
		return l.empty() ? std::to_string(f + 1) : l;
	};
	// Digits about 60% of the row high, as large as fit above the bar.
	const int above = std::max(1, row / 8);
	const int scale = std::clamp(static_cast<int>(row * 0.6 / GLYPH_H + 0.5),
	                             1,
	                             std::max(1, (row - above) / GLYPH_H));
	const int y = std::max(0, row - GLYPH_H * scale - above);
	const std::string cur = label(s.m_frame);
	const int curW = Canvas::textWidth(cur, scale);
	const int curX = std::clamp(static_cast<int>((x0 + x1) / 2.0 - curW / 2.0),
	                            0,
	                            std::max(0, width - curW));
	const int gap = Canvas::advance(scale);
	auto clear = [&](int x, int w)
	{ return x + w + gap <= curX || x >= curX + curW + gap; };
	const std::string first = label(s.m_range.m_first);
	const std::string last = label(s.m_range.m_last);
	const int firstW = Canvas::textWidth(first, scale);
	const int lastW = Canvas::textWidth(last, scale);
	const int firstX =
	    std::clamp(columnOf(s.m_range.m_first), 0, std::max(0, width - firstW));
	const int lastX = std::clamp(columnOf(s.m_range.m_last + 1) - lastW,
	                             0,
	                             std::max(0, width - lastW));
	const bool firstShown = clear(firstX, firstW);
	if(firstShown)
	{
		cv.text(firstX,
		        y,
		        first,
		        scale,
		        s.m_hasIn ? IN_OUT_YELLOW : LABEL_GREY,
		        false);
	}
	if(clear(lastX, lastW) && (!firstShown || lastX >= firstX + firstW + gap))
	{
		cv.text(lastX,
		        y,
		        last,
		        scale,
		        s.m_hasOut ? IN_OUT_YELLOW : LABEL_GREY,
		        false);
	}
	cv.text(curX, y, cur, scale, CURRENT, true);
	return img;
}

Playbar::Playbar(const TermCaps& caps, kitty::Transmitter& tx)
{
	if(caps.m_graphics == GraphicsMode::KITTY)
	{
		m_picture = std::make_unique<BitmapSlot>(caps, tx);
	}
}

Element Playbar::render(const PlaybarState& s, int width)
{
	const int n = std::max(1, s.m_frames);
	const bool ranged = s.m_hasIn || s.m_hasOut;
	// Both sides keep their width (set in / out, start playing): the bar
	// must not shift under the mouse.
	const std::string count =
	    ranged ? fmt::format("{}/{}", s.m_range.count(), n) : std::to_string(n);
	const int countWidth = static_cast<int>(fmt::formatted_size("{}/{}", n, n));
	std::string left = fmt::format(" {:>{}} frames ", count, countWidth);
	std::string right = fmt::format(" {} {:5.2f} {:4.1f} fps ",
	                                s.m_playing ? "▸" : "‖",
	                                s.m_fps,
	                                s.m_playing ? s.m_measured : 0.0);
	auto widthOf = [](const std::string& t)
	{ return static_cast<int>(string_width(t)); };
	if(width - widthOf(left + right) < 12)
	{
		left = right = " "; // a narrow terminal: the bar only
	}
	m_trackX = widthOf(left);
	const int room = std::max(1, width - m_trackX - widthOf(right));
	// A picture moves by pixels: no need to even out the cells.
	m_trackWidth = m_picture ? std::min(room, kitty::MAX_PLACEHOLDER_INDEX)
	                         : evenTrackWidth(n, room);
	m_frames = n;
	const int w = m_trackWidth;

	// The frames each cell stands for: [lo, hi].
	std::vector<int> lo(w), hi(w);
	for(int c = 0; c < w; ++c)
	{
		std::tie(lo[c], hi[c]) = framesOf(c, w, n);
	}
	// The cells frame f covers: [firstCell(f), lastCell(f)].
	auto firstCell = [&](int f)
	{
		int c = 0;
		while(c < w - 1 && hi[c] < f)
		{
			++c;
		}
		return c;
	};
	auto lastCell = [&](int f)
	{
		int c = w - 1;
		while(c > 0 && lo[c] > f)
		{
			--c;
		}
		return c;
	};

	const std::string gapToRight(
	    static_cast<size_t>(std::max(0, width - m_trackX - w - widthOf(right))),
	    ' ');
	if(m_picture)
	{
		// The numbers and the bar as one picture, two rows high.
		return hbox(
		           {vbox({text(std::string(m_trackX, ' ')),
		                  text(left) | (ranged ? color(Color::Yellow) : dim)}),
		            m_picture->element([s](int pw, int ph)
		                               { return paintPlaybar(s, pw, ph); }) |
		                size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, 2),
		            vbox({text(""),
		                  hbox({text(gapToRight),
		                        text(right) | (s.m_playing ? color(Color::Green)
		                                                   : dim)})})}) |
		       reflect(m_box);
	}
	Element bar;
	{
		// A half-height bar: cyan where the terminal keeps the frames, green
		// where decoded, grey where not yet, the current frame a paler green;
		// thinner still outside in / out.
		Row cells(w);
		for(int c = 0; c < w; ++c)
		{
			const bool in = inRange(s, lo[c], hi[c]);
			const bool current = s.m_frame >= lo[c] && s.m_frame <= hi[c];
			cells.m_glyphs[c] = current || in ? "▀" : "▔";
			cells.m_looks[c] = current                      ? Look::PLAYHEAD
			                   : !in                        ? Look::OUTSIDE
			                   : stored(s, n, lo[c], hi[c]) ? Look::STORED
			                   : storedElsewhere(s, n, lo[c], hi[c])
			                       ? Look::ELSEWHERE
			                   : decoded(s, n, lo[c], hi[c]) ? Look::CACHED
			                                                 : Look::EMPTY;
		}
		bar = cells.element();
	}

	// Numbers above: the range's ends at theirs, the current frame centred
	// over the playhead (it wins where they would touch).
	auto label = [&](int f)
	{
		std::string l = s.m_label ? s.m_label(f) : std::string();
		return l.empty() ? std::to_string(f + 1) : l;
	};
	auto len = [](const std::string& t) { return static_cast<int>(t.size()); };
	const std::string cur = label(s.m_frame);
	const double centre =
	    (firstCell(s.m_frame) + lastCell(s.m_frame) + 1) / 2.0;
	const int curX = std::clamp(static_cast<int>(centre - len(cur) / 2.0),
	                            0,
	                            std::max(0, w - len(cur)));
	auto clear = [&](int x, int l)
	{ return x + l < curX || x > curX + len(cur); }; // a cell apart
	const std::string first = label(s.m_range.m_first);
	const std::string last = label(s.m_range.m_last);
	const int firstX = std::clamp(firstCell(s.m_range.m_first),
	                              0,
	                              std::max(0, w - len(first)));
	const int lastX = std::clamp(lastCell(s.m_range.m_last) + 1 - len(last),
	                             0,
	                             std::max(0, w - len(last)));
	Row labels(w);
	const bool firstShown = clear(firstX, len(first));
	if(firstShown)
	{
		labels.put(firstX, first, s.m_hasIn ? Look::IN_OUT : Look::LABEL);
	}
	if(clear(lastX, len(last)) && (!firstShown || lastX > firstX + len(first)))
	{
		labels.put(lastX, last, s.m_hasOut ? Look::IN_OUT : Look::LABEL);
	}
	labels.put(curX, cur, Look::CURRENT);

	const std::string padLeft(m_trackX, ' ');
	const std::string padRight(static_cast<size_t>(
	                               std::max(0, width - m_trackX - w)),
	                           ' ');
	return vbox({
	           hbox({text(padLeft), labels.element(), text(padRight)}),
	           hbox({text(left) | (ranged ? color(Color::Yellow) : dim),
	                 bar,
	                 text(gapToRight),
	                 text(right) | (s.m_playing ? color(Color::Green) : dim)}),
	       }) |
	       reflect(m_box);
}

int Playbar::frameAt(int x) const
{
	const int cell = x - m_box.x_min - m_trackX;
	// A cell stands for its first frame; the last one (and past it) for the
	// last frame, so the end is reachable when cells hold several.
	if(cell >= m_trackWidth - 1)
	{
		return m_frames - 1;
	}
	return trackFrameAt(std::max(0, cell), m_trackWidth, m_frames);
}

} // namespace rega
