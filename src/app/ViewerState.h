#pragma once

#include "app/Annotations.h"
#include "app/Player.h"
#include "image/Render.h"
#include "image/Sequence.h"

#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rega
{

/// One image or sequence open in the viewer.
struct Source
{
	Entry m_entry;
	/// `G`: a frame per number from the first to the last, a missing one
	/// showing the nearest file, so 1001, 1050, 1100 plays 100 frames long.
	bool m_fillGaps = false;
	/// Labels given with the path (`a.exr:label=top:main`): for this session
	/// only, shown and hidden (`A`) with the annotations.
	AnnotationSet m_labels;
	/// A diff (|A - B|): this is A; m_entry is B, whose frames, header and
	/// metadata it goes by.
	std::optional<Entry> m_diffA;
	/// Sources sharing a group are shown together, tiled (a diff pair: A, B
	/// and their diff); -1: none, the sheet shows every source.
	int m_group = -1;
	/// Frame `i` (clamped); the file itself for a single image.
	std::filesystem::path frame(int i) const;
	/// A diff's A side at frame `i` (see frame()).
	std::filesystem::path diffFrame(int i) const;
	int frameCount() const;
	std::string frameLabel(int i) const; ///< its number ("" for an image)
	int frameNumber(int i) const;        ///< its number (0 for an image)
	/// Whether frame `i` has no file of its own (filled gaps only).
	bool held(int i) const;
	/// The frame numbered `number`, else the nearest one (the earlier on a
	/// tie); 0 for a single image.
	int indexForFrameNumber(int number) const;

private:
	bool filled() const; ///< m_fillGaps on a sequence with frames
	/// The file numbered `number`, else the nearest (the earlier on a tie).
	int nearestFile(int number) const;
};

/// Key of a source in the annotations state file: absolute dir / display name
/// ("/shots/a.####.exr"); a diff's adds " - " and A's.
std::string sourceKey(const Source& s);

/// Something to view: an image or sequence, or with `m_b`, a diff pair.
struct OpenItem
{
	Entry m_a;
	AnnotationSet m_labelsA;
	std::optional<Entry> m_b;
	AnnotationSet m_labelsB;
};
/// A command-line image: a path (labels allowed, see splitLabels()), or
/// with `m_diffWith`, a `--diff` pair, this being A.
struct OpenArg
{
	std::string m_path;
	std::optional<std::string> m_diffWith;
};
/// What `args` open, in order. A run of plain paths groups into sequences
/// as entriesForArgs() does, each keeping the labels given with it; a pair
/// one side of which is missing opens the other alone. Unmatched paths go
/// to `missing`.
std::vector<OpenItem> openItems(const std::vector<OpenArg>& args,
                                std::vector<std::string>& missing);
/// The browser's marks as pairs: 1st against 2nd, 3rd against 4th, …; an
/// odd last one alone.
std::vector<OpenItem> pairItems(std::vector<Entry> marked);

/// The viewer's sources for `items`, in order. With any pair, each item is a
/// group: a pair becomes A, B and their diff (labelled "diff"), anything
/// else a group of its own.
std::vector<Source> sourcesFor(std::vector<OpenItem> items, bool fillGaps);

/// Keyboard focus inside the viewer, lazygit style: the image [1] or a pane.
enum class Focus
{
	IMAGE,
	META,
	FILES,
	INSPECT,
	LAYERS,
	COLOUR
};

/// A pixel read from whatever image is under a terminal cell (the main view, or
/// a tile), for the inspector and the click colour picker.
struct Sample
{
	enum class State
	{
		NONE,    ///< no image under the cell
		LOADING, ///< no pixels decoded yet
		OUTSIDE, ///< outside the data window
		OK
	};
	State m_state = State::NONE;
	std::filesystem::path m_path; ///< the frame read
	int m_x = 0, m_y = 0; ///< image (file) coordinates: y down from the top
	Box2i m_frame;        ///< the display window, for nukeX() / nukeY()
	bool m_exact = false; ///< full resolution (else what is on screen, ≈)
	std::string m_layer;
	std::vector<std::pair<std::string, float>> m_values;
	float m_luma = 0.0f;
	int m_r = 0, m_g = 0, m_b = 0;  ///< display-transformed, 8-bit
	float m_rgba[4] = {0, 0, 0, 1}; ///< file values (linear), for readouts
	bool m_hasAlpha = false;

	/// Nuke's coordinates, which every readout shows: (0, 0) at the frame's
	/// bottom-left, y up. File coordinates when the frame is unknown.
	int nukeX() const noexcept
	{
		return m_frame.width() > 0 ? m_x - m_frame.m_x0 : m_x;
	}
	int nukeY() const noexcept
	{
		return m_frame.height() > 0 ? m_frame.m_y1 - m_y : m_y;
	}
};

/// What the viewer's panes read and change. Owned by Viewer; the panes hold a
/// reference, never a pointer back to the Viewer.
struct ViewerState
{
	std::vector<Source> m_sources;
	int m_current = 0; ///< active source
	int m_frame = 0;
	/// `I` / `O`: where playback starts and ends (frame indices).
	std::optional<int> m_in, m_out;
	std::string m_layerLabel;
	DisplayParams m_disp;
	ViewParams m_view;
	Focus m_focus = Focus::IMAGE;
	std::optional<Sample> m_picked; ///< last colour clicked (until viewer exit)
	/// The side columns' widths as fractions of the terminal, once their
	/// dividers have been dragged; 0 = the default (see leftPanelWidth()).
	double m_leftFrac = 0, m_rightFrac = 0;

	/// Width of the left column (layers / colour / files): 20% of the
	/// terminal, at least 26 cells, until its divider is dragged elsewhere.
	int leftPanelWidth() const;
	/// Width of the right column (inspector / metadata): 30%, at least 32
	/// cells, until dragged. Dragged columns leave the image some room; the
	/// left one wins when both want it.
	int sidePanelWidth() const;
	/// The current frame of the active source.
	std::filesystem::path currentFramePath() const;
	int frameCount() const; ///< the longest source's
	/// The frames playback loops over: in to out, each defaulting to the
	/// sequence's end; the whole sequence if they cross.
	FrameRange playRange() const;
	/// The sequence `:` frame numbers refer to: the active source, else the
	/// first sequence open; nullptr when none is a sequence.
	const Source* numberedSource() const;
	/// The lines source `i` adds (nullptr: none, or no such source).
	const AnnotationSet* sourceAnnotations(const Annotations& ann, int i) const;
	/// What the sheet tiles by source: the active source's group, else every
	/// source. One source tiles its layers instead.
	std::vector<int> sheetSources() const;
	/// Whether groups are in use (a diff session): n / p step by group.
	bool grouped() const;
	/// The groups in order, and which one source `i` is in (its index).
	int groupCount() const;
	int groupIndex(int i) const;
	/// The first source of the group `delta` groups on from the active one
	/// (wrapping).
	int stepGroup(int delta) const;
};

/// Whether cell (x, y) is inside `b`.
inline bool inside(const ftxui::Box& b, int x, int y) noexcept
{
	return x >= b.x_min && x <= b.x_max && y >= b.y_min && y <= b.y_max;
}

} // namespace rega
