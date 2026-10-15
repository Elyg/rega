#pragma once

#include "image/Overlay.h"
#include "image/Render.h"
#include "term/Caps.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rv
{

namespace kitty
{
class Transmitter;
}

/// A persistent on-screen image area. The FTXUI element it produces renders the
/// layer at the exact pixel size of the box it is laid out in, then either
/// transmits it to the terminal as a kitty image displayed through Unicode
/// placeholder cells, or paints half-blocks. Re-renders only when the image,
/// view, display params or geometry change.
///
/// With kitty and a `ready` callback, rendering and encoding happen on the
/// slot's own thread: a draw that needs a new picture asks for it and keeps
/// showing the previous one; `ready` fires (on that thread) once it is
/// encoded, and the next draw sends it. Slots then encode in parallel and the
/// UI thread only writes finished escapes. prepareAhead() starts on the next
/// playback frame before it is due, so its draw finds it done. Without
/// `ready`, or in half-block mode, a draw renders in place.
class ImageSlot
{
public:
	ImageSlot(const TermCaps& caps,
	          kitty::Transmitter& tx,
	          std::function<void()> ready = {});
	~ImageSlot();
	ImageSlot(const ImageSlot&) = delete;
	ImageSlot& operator=(const ImageSlot&) = delete;

	/// Element drawing `img` with `view` and `disp`.
	/// @param overlay burn-in text laid out on the frame (display window) and
	///                drawn over the image cells.
	ftxui::Element element(LayerImagePtr img,
	                       const ViewParams& view,
	                       const DisplayParams& disp,
	                       OverlayText overlay = {});

	/// A rectangle of screen cells.
	struct CellRect
	{
		int m_x = 0, m_y = 0, m_w = 0, m_h = 0;
	};
	/// Screen cells of the frame (display window) in the last draw: the cells
	/// whose centre lies inside it, possibly beyond the drawn box.
	std::optional<CellRect> frameCells() const;
	/// Average display colour of the image under screen cell (x, y) in the last
	/// draw (transparent counts as black); nullopt outside the drawn area.
	std::optional<std::array<uint8_t, 3>> cellColor(int x, int y) const;

	/// Image-space coordinate under terminal cell (x, y) (cell centre), based
	/// on the last draw.
	std::optional<std::pair<double, double>> imageCoordAt(int cellX,
	                                                      int cellY) const;
	/// Screen box of the last draw.
	const ftxui::Box& box() const
	{
		return m_box;
	}
	/// Terminal pixels per cell along x/y (half-block: 1 x 2).
	int pxPerCellX() const;
	int pxPerCellY() const; ///< see pxPerCellX()
	/// Output pixel → image mapping of the last draw.
	const ViewMapping& mapping() const
	{
		return m_mapping;
	}
	/// Terminal pixels per image pixel of the last draw (useful when leaving
	/// fit mode).
	double effectiveZoom() const
	{
		return m_mapping.m_scale / m_quality;
	}
	/// Terminal-pixel size of the last drawn area.
	std::pair<int, int> areaPixels() const
	{
		return {m_areaW, m_areaH};
	}
	/// Whether anything was drawn yet.
	bool drawn() const
	{
		return m_last.has_value();
	}
	/// The image last put on screen; null before the first.
	const LayerImagePtr& shown() const
	{
		return m_keepAlive;
	}

	/// Cap on transmitted pixels (kitty). Larger areas are rendered smaller and
	/// scaled up by the terminal — keeps playback bandwidth sane.
	/// ViewParams::zoom stays in terminal px.
	void setMaxPixels(int n)
	{
		m_maxPixels = n;
	}
	[[nodiscard]] int maxPixels() const noexcept
	{
		return m_maxPixels;
	}
	/// True while a picture is being prepared on the slot's thread (or is
	/// ready and waiting for a draw).
	[[nodiscard]] bool pending() const;
	/// Start preparing `img` drawn exactly like the last draw (same box, view
	/// and display), for a draw that will ask for it soon. Quiet: `ready`
	/// only fires if a draw asks for it before it is done. Does nothing
	/// without a background thread or before the first draw.
	void prepareAhead(LayerImagePtr img);

	/// Draw `img` into `box`: placeholder cells (kitty) or half-blocks.
	void draw(ftxui::Screen& screen,
	          const ftxui::Box& box,
	          const LayerImagePtr& img,
	          const ViewParams& view,
	          const DisplayParams& disp);
	/// Lay `overlay` out on the frame of the last draw and paint it, clipped to
	/// the drawn area.
	void drawOverlay(ftxui::Screen& screen, const OverlayText& overlay) const;

private:
	struct Key
	{
		const LayerImage* m_img = nullptr;
		ViewParams m_view;
		DisplayParams m_disp;
		int m_cols = 0, m_rows = 0, m_cellW = 0, m_cellH = 0, m_maxPixels = 0;
		GraphicsMode m_mode = GraphicsMode::HALF_BLOCK;
		/// How it went out: a switch sends it again (a terminal attached
		/// over ssh can't read what went through files).
		Transfer m_transfer = Transfer::DIRECT;
		int m_resend = 0; ///< TermCaps::m_resend
		bool operator==(const Key&) const = default;
	};
	/// What a draw needs rendered, and what comes back.
	struct Request
	{
		Key m_key;
		LayerImagePtr m_img;
		bool m_fromDraw = false; ///< a draw wanted it (not prepareAhead)
	};
	struct Prepared
	{
		Key m_key;
		LayerImagePtr m_img;
		ViewMapping m_mapping;
		double m_quality = 1.0;
		std::vector<uint8_t> m_cellRgb;
		std::string m_escapes; ///< kitty
		Rgba8Image m_bitmap;   ///< half-block
		bool m_fromDraw = false;
	};
	/// Whether `a` and `b` differ in the image only.
	static bool sameView(Key a, const Key& b);

	/// Render (and with kitty, encode) a request. Any thread; reads only
	/// m_caps and the transmitter.
	[[nodiscard]] Prepared prepare(Request req) const;
	/// Make a prepared picture the one on screen (UI thread).
	void apply(Prepared p);
	/// Hand `key` to the slot's thread unless it is already asked for or
	/// done. m_jobMu held.
	void requestLocked(const Key& key,
	                   LayerImagePtr img,
	                   bool fromDraw = false);
	void encodeLoop(std::stop_token stop);

	const TermCaps& m_caps;
	kitty::Transmitter& m_tx;
	std::function<void()> m_ready;
	/// The slot's kitty id. Every picture replaces the image under it, so the
	/// placeholder cells (whose colour carries the id) stay as they are and a
	/// new frame costs only its transmit. A fresh id per picture rewrote every
	/// cell: ~50 KB a frame, past what tmux buffers for a slow client, so it
	/// dropped output (kitty transmits and deletes with it) and pictures went
	/// blank or stale.
	const uint32_t m_id;
	bool m_transmitted = false;
	LayerImagePtr
	    m_keepAlive; ///< the key compares raw pointers; keep the image alive
	std::optional<Key> m_last;
	Rgba8Image m_halfblock;
	/// Per drawn cell, average RGB of the rendered image (for overlay strips).
	std::vector<uint8_t> m_cellRgb;
	int m_cols = 0, m_rows = 0;
	Box2i m_frame{}; ///< display window of the last drawn image
	ftxui::Box m_box{};
	ViewMapping m_mapping;
	double m_quality = 1.0; ///< output px per terminal px
	int m_areaW = 0, m_areaH = 0;
	int m_maxPixels = 4'000'000;

	/// The slot's encode thread (kitty with `ready` only; started on first
	/// use). Newest request wins: one waiting, one being prepared, one done.
	std::optional<Key> m_lastDrawn; ///< the key the last draw wanted
	mutable std::mutex m_jobMu;
	std::optional<Key> m_waitFor; ///< what a draw waits for: `ready` when done
	std::condition_variable_any m_jobCv;
	std::optional<Request> m_request;
	/// Finished, newest last: the frame about to be drawn and the one ahead.
	std::deque<Prepared> m_done;
	static constexpr size_t MAX_DONE = 2;
	std::optional<Request> m_busy; ///< being prepared (m_img unset)
	std::jthread m_worker;         ///< last: stops before the rest goes
};

/// One owner per slot; on the heap because ImageNode keeps a raw ImageSlot*.
using ImageSlotPtr = std::unique_ptr<ImageSlot>;

/// Image colour under a screen cell, for overlay strips.
using CellColorFn =
    std::function<std::optional<std::array<uint8_t, 3>>(int x, int y)>;
/// Paint laid-out overlay cells (screen coordinates) inside `clip`: white text
/// (grey for unresolved placeholders) on the image colour under the cell,
/// darkened ~60%.
void paintOverlay(ftxui::Screen& screen,
                  const ftxui::Box& clip,
                  const std::vector<OverlayCell>& cells,
                  const CellColorFn& colorAt);
/// Renders `child`, then calls `after` to draw on top of it.
ftxui::Element drawAfter(ftxui::Element child,
                         std::function<void(ftxui::Screen&)> after);
/// Each child laid out in its own box, given relative to the top-left of the
/// element's box (parallel vectors); the element takes what it is given.
ftxui::Element placeAt(ftxui::Elements children, std::vector<ftxui::Box> boxes);
/// Drop the diacritics of every kitty placeholder cell whose left neighbour
/// is the same image one column back: the terminal infers them. Halves what a
/// picture costs to print (8 → 4 bytes a cell); FTXUI reprints every cell each
/// frame, and through tmux a burst past ~8 bytes a cell gets dropped (kitty
/// transmits with it). Run on the finished screen, after anything drawn over
/// the image: a cell right of a break keeps its diacritics, unless it is in
/// the last column, where tmux misplaces them: there it takes the background
/// of the cell to its left instead.
void compactPlaceholders(ftxui::Screen& screen);

} // namespace rv
