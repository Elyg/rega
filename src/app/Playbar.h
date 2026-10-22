#pragma once

#include "app/Player.h"
#include "image/Render.h"
#include "term/BitmapSlot.h"
#include "term/Caps.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rv
{

/// What the playbar shows.
struct PlaybarState
{
	int m_frames = 1;   ///< frame count
	int m_frame = 0;    ///< the current one
	FrameRange m_range; ///< in to out
	bool m_hasIn = false, m_hasOut = false;
	std::vector<bool> m_cached; ///< per frame: decoded (the green track)
	/// Per frame: kept in the terminal (cyan: plays without sending any
	/// pixels); empty when nothing is.
	std::vector<bool> m_stored;
	/// Per frame: kept in the terminal for some view (dim cyan where not for
	/// this one: still there for a pan back); empty when nothing is.
	std::vector<bool> m_storedElsewhere;
	/// The number shown for a frame index (from the file name).
	std::function<std::string(int)> m_label;
	bool m_playing = false;
	double m_fps = 24;     ///< the rate aimed at
	double m_measured = 0; ///< the rate shown, while playing
};

/// The frame cell `cell` of a `width`-cell track of `frames` frames stands
/// for: each frame gets an equal share of the cells, or each cell an equal
/// share of the frames (then the first of them).
int trackFrameAt(int cell, int width, int frames);

/// How many of `cells` the bar takes for `frames`: as many as give every
/// cell the same whole number of frames (or every frame the same number of
/// cells), so the playhead steps at an even pace, not 4, 5, 5, 4 frames
/// apart. All of them when that would leave more than 25% empty.
int evenTrackWidth(int frames, int cells);

/// The playbar's two rows as `width` x `height` pixels, for kitty: the
/// numbers, then the bar, in the text playbar's colours and shape (the top
/// half of its row; an eighth outside in / out). The current frame sits at
/// its exact place and its number (drawn with a small digit font) centred
/// over it, so both glide a few pixels a frame instead of jumping a cell.
Rgba8Image paintPlaybar(const PlaybarState& s, int width, int height);

/// The timeline under a sequence, after RV's, in two rows: the frame
/// numbers (the current one over the playhead, the range's ends at theirs),
/// then the frame count, the bar and the fps. The bar is half a cell high,
/// green where frames are decoded, cyan where the terminal keeps them (over
/// ssh), paler green at the current frame, and
/// thinner outside in / out. With kitty graphics it is a picture (see
/// paintPlaybar); otherwise text, as wide as gives every cell the same
/// number of frames (see evenTrackWidth).
class Playbar
{
public:
	/// Text only.
	Playbar() = default;
	/// A picture when `caps` has kitty graphics, sent through `tx`.
	Playbar(const TermCaps& caps, kitty::Transmitter& tx);

	[[nodiscard]] ftxui::Element render(const PlaybarState& s, int width);

	/// Whether cell (x, y) is on the playbar, as last drawn.
	bool contains(int x, int y) const noexcept
	{
		return x >= m_box.x_min && x <= m_box.x_max && y >= m_box.y_min &&
		       y <= m_box.y_max;
	}
	/// The frame under column `x`: a cell's first frame, the last cell's
	/// last; past the ends, the first / last frame.
	int frameAt(int x) const;
	/// Cells the bar takes, as last drawn.
	int trackWidth() const noexcept
	{
		return m_trackWidth;
	}

private:
	ftxui::Box m_box{};
	int m_trackX = 0; ///< columns before the track (the frame count)
	int m_trackWidth = 1;
	int m_frames = 1;
	std::unique_ptr<BitmapSlot> m_picture; ///< kitty: the bar as pixels
};

} // namespace rv
