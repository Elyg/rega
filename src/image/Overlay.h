#pragma once

#include <array>
#include <string>
#include <vector>

namespace rega
{

/// Burn-in style text slots around a frame (no middle row).
enum class Slot
{
	TL,
	TC,
	TR,
	BL,
	BC,
	BR
};
constexpr int SLOT_COUNT = 6;

const char* slotKey(Slot s);  ///< "tl" … "br"
const char* slotName(Slot s); ///< "top left" … "bottom right"
bool isTopSlot(Slot s);       ///< TL, TC or TR

/// A run of text; `m_dim` marks an unresolved placeholder.
struct OverlayRun
{
	std::string m_text;
	bool m_dim = false;
};
using OverlayLine = std::vector<OverlayRun>;
/// Lines per slot, in order (top slots read downward from the top edge, bottom
/// slots end at the bottom edge).
using OverlayText = std::array<std::vector<OverlayLine>, SLOT_COUNT>;

/// Whether no slot has any line.
bool overlayEmpty(const OverlayText& t);

/// One placed glyph, in the cell coordinates of the frame rect given to
/// layoutOverlay. Wide glyphs are followed by an empty-string cell.
struct OverlayCell
{
	int m_x = 0, m_y = 0;
	std::string m_glyph;
	bool m_dim = false;
};

/// Lay the slots out inside a frame of `w` x `h` cells whose top-left cell is
/// (x0, y0). The frame may be partly off-screen; the caller clips. Rules: a
/// 1-cell inset on axes of at least 3 cells; per row the corners win (each at
/// most half the width when the other corner is used), the centre gets the gap
/// between them (truncated with …, dropped under 3 cells); when the top and
/// bottom stacks collide the bottom wins and the top loses its last lines.
/// Nothing is placed outside the frame.
std::vector<OverlayCell>
layoutOverlay(int x0, int y0, int w, int h, const OverlayText& text);

} // namespace rega
