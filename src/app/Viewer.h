#pragma once

#include "app/AppContext.h"
#include "app/ColourPane.h"
#include "app/FilesPane.h"
#include "app/InspectorPane.h"
#include "app/LayersPane.h"
#include "app/LineEditor.h"
#include "app/MetaPane.h"
#include "app/Playbar.h"
#include "app/Player.h"
#include "app/Sheet.h"
#include "app/ViewerState.h"
#include "image/Overlay.h"
#include "term/ImageView.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// The image viewer: one image (or a contact sheet of tiles), display
/// controls, sequence playback and the side panes. Reports leaving through
/// `onClose`; knows nothing of the browser.
class Viewer
{
public:
	Viewer(AppContext& ctx, std::function<void()> onClose);
	Viewer(const Viewer&) = delete;
	Viewer& operator=(const Viewer&) = delete;

	/// Show `entries` (`tile`: as a contact sheet), from the first frame, fit.
	void open(std::vector<Entry> entries, bool tile = false);
	/// The same for sources made already (labels, diff pairs: see
	/// sourcesFor()); a diff pair opens tiled.
	void openSources(std::vector<Source> sources, bool tile = false);
	/// Images and diff pairs (see OpenItem).
	void openItems(std::vector<OpenItem> items)
	{
		openSources(sourcesFor(std::move(items), m_fillGaps));
	}
	/// Handle an event; true when consumed.
	[[nodiscard]] bool event(ftxui::Event e);
	[[nodiscard]] ftxui::Element render();
	/// A playback tick (the ticker woke the UI). True if the frame advanced.
	bool tick();

	bool playing() const noexcept
	{
		return m_player.playing();
	}
	/// Frames a second playback shows (averaged; 0 when not playing) and
	/// aims for.
	double shownFps() const noexcept
	{
		return m_player.playing() ? m_shownFps : 0.0;
	}
	double targetFps() const noexcept
	{
		return m_player.targetFps(std::nullopt);
	}
	/// An annotation line or a `:` frame number is being typed (keys are
	/// text, not commands).
	bool typing() const noexcept
	{
		return m_files.annotations().editing() || m_goto.has_value() ||
		       m_meta.typing();
	}
	/// Commit a line being typed (before the annotations are saved).
	void finishTyping()
	{
		m_files.annotations().finishEdit(true);
	}
	/// Where the mouse last was, for the inspector's live readout.
	void setMouse(int x, int y) noexcept
	{
		m_mouseX = x;
		m_mouseY = y;
	}

	/// Re-scan the open sequences for frames added or removed. True if any
	/// changed.
	bool rescan();
	/// The directories holding the open images.
	std::vector<std::filesystem::path> shownDirs() const;

	const ViewerState& state() const noexcept
	{
		return m_state;
	}
	bool tiled() const noexcept
	{
		return m_tile;
	}
	const Player& player() const noexcept
	{
		return m_player;
	}
	/// Cells the playbar's bar takes, as last drawn.
	int playbarWidth() const noexcept
	{
		return m_playbar.trackWidth();
	}
	/// The contact sheet as last drawn, and where it is zoomed / panned.
	const SheetLayout& sheet() const noexcept
	{
		return m_sheet;
	}
	const SheetView& sheetView() const noexcept
	{
		return m_sheetView;
	}
	/// How many tiles are on screen (have a slot).
	int tilesShown() const noexcept
	{
		return static_cast<int>(m_tiles.size());
	}
	/// `P`: whether playback renders below the paused resolution (default)
	/// to keep up, or at full resolution, as fast as it can.
	bool playbackCapped() const noexcept
	{
		return m_playbackCap;
	}
	/// Pixels each image slot may send per draw (kitty): the budget split
	/// between tiles, and less while playing (unless `P` lifted the cap).
	int pixelCap() const;
	/// The whole pixel budget while playing with the cap on (tiles share it).
	int cappedBudget() const;
	/// Over ssh, send the play range's frames for the terminal to keep
	/// (ImageSlot::store), one at a time while the link is idle, from the
	/// current frame on: paused, so the first pass already plays from the
	/// terminal; playing, ahead of the playhead. UI thread; cheap when all
	/// are kept. Re-runs itself (a wake every 100 ms) until they are.
	void fillTerminal();

private:
	/// `q`: leave the viewer (playback stops).
	void close();

	// --- view ---
	int reduceFor(const ImageInfo& info, const ImageSlot& slot) const;
	/// What playback draws a slot at: its pixel cap and decode reduction,
	/// whether playing now or not (fillTerminal stores for later).
	int playbackPixelCap() const;
	int playbackReduce(const ImageInfo& info, const ImageSlot& slot) const;
	/// Whether fillTerminal applies: inline, a terminal budget, one sequence
	/// shown alone (no tiles, no diff).
	bool fillsTerminal() const;
	/// Per frame of the shown sequence: kept in the terminal at playback
	/// size (the playbar's cyan); empty when nothing is kept.
	std::vector<bool> storedFrames(const ImageInfo& info);
	/// Per frame: kept in the terminal for some view, this one or another
	/// (the playbar's dim cyan: a pan back plays them from there).
	std::vector<bool> storedElsewhere();
	/// The slot playback decodes for: the first tile, or the main view.
	const ImageSlot& playbackSlot() const;
	/// Whether source `i` is on screen (every source when tiled).
	bool showsSource(int i) const;
	/// Whether the sheet has a tile per source (else per layer), and which
	/// tile source `i` is (-1: not on the sheet).
	bool tilesBySource() const;
	int tileOf(int source) const;
	/// What source `source` shows for `path` (a frame of it): its layer, or
	/// for a diff, A against B; and why it could not be read.
	LayerImagePtr
	layerOf(int source,
	        const std::filesystem::path& path,
	        const std::string& layer,
	        int reduce,
	        ImageService::Priority pr = ImageService::Priority::NOW);
	std::optional<std::string> errorOf(int source,
	                                   const std::filesystem::path& path);
	void cycleLayer(const ImageInfoPtr& info, int delta);
	void zoomBy(double factor,
	            std::optional<std::pair<double, double>> anchor = std::nullopt);
	void pan(double dxCells, double dyCells);
	/// `f`: fit the image, into the area left of the side panes if open.
	void fitView(const ImageInfoPtr& info);
	void setFrame(int f);
	/// `I` / `O`: set the in / out point at the current frame; again on it,
	/// clear it.
	void setInOut(bool in);
	/// `:`: start typing a frame number (sequences only).
	void openGoto();
	/// A key while typing the frame number; Enter goes to it.
	void gotoEvent(const ftxui::Event& e);
	void togglePlay();
	/// `G`: missing frames held (the nearest shown), or skipped.
	void toggleFillGaps();
	void prefetchFrame(int f);
	/// Start the slots on frame `f` (decoded already) before it is due.
	void prepareAhead(int f);
	/// The pixel under terminal cell (x, y), in the view or a tile.
	Sample sampleAt(int cellX, int cellY);
	/// The display settings for frame `path` of source `source`: the shared
	/// ones, with that image's OCIO transform (when a config is in use).
	DisplayParams displayFor(int source, const std::filesystem::path& path);
	/// The layer the HUD and inspector describe: on screen in the single
	/// view, the selected tile's in a sheet. Null until drawn.
	LayerImagePtr describedImage(const ImageInfoPtr& info) const;
	/// The picked pixel, ringed in red while the inspector is open.
	std::optional<std::pair<int, int>> pickedMarker() const
	{
		if(!m_inspector.isOpen() || !m_state.m_picked)
		{
			return std::nullopt;
		}
		return std::pair(m_state.m_picked->m_x, m_state.m_picked->m_y);
	}
	/// Zoom (terminal px per image px) `slot` shows its image at; nullopt
	/// when fitted.
	std::optional<double> zoomOf(const ImageSlot& slot) const;

	// --- tiles: a contact sheet, zoomed and panned as one image ---
	/// The tile the HUD and panes describe: the active source (by source) or
	/// the shown layer.
	int selectedTile(const ImageInfoPtr& info) const;
	void selectTile(int i, const ImageInfoPtr& info);
	/// Zoom the sheet by `factor` about area cell `at` (default: the
	/// selected tile, which then stays put).
	void zoomSheet(double factor,
	               const ImageInfoPtr& info,
	               std::optional<std::pair<double, double>> at = std::nullopt);
	/// Pan the sheet by area cells; the tile under the middle gets selected.
	void panSheet(double dx, double dy, const ImageInfoPtr& info);
	/// `z` on the sheet: the selected tile's pixels 1:1, centred on it.
	void sheetOneToOne(const ImageInfoPtr& info);
	/// Bring tile `i` to the middle (when zoomed in).
	void centreSheetOn(int i);
	/// Zoomed in: select the tile under the middle of the area.
	void followSheetCentre(const ImageInfoPtr& info);
	/// Enter: the selected tile alone, at the zoom and place it had.
	void openSelectedTile(const ImageInfoPtr& info);

	// --- panes ---
	/// Right column: inspector / metadata. Left column: layers / colour /
	/// files.
	bool rightPanelOpen() const noexcept
	{
		return m_inspector.isOpen() || m_meta.isOpen();
	}
	bool leftPanelOpen() const noexcept
	{
		return m_files.isOpen() || m_layers.isOpen() || m_colour.isOpen();
	}
	bool sidePanelOpen() const noexcept
	{
		return rightPanelOpen() || leftPanelOpen();
	}
	/// Terminal columns the open side columns cover (incl. their separator).
	int coveredLeft() const
	{
		return leftPanelOpen() ? m_state.leftPanelWidth() + 1 : 0;
	}
	int coveredRight() const
	{
		return rightPanelOpen() ? m_state.sidePanelWidth() + 1 : 0;
	}
	/// `Tab`: hide every open pane; the next `Tab` brings them back.
	void togglePanes();

	// --- annotations ---
	/// Placeholder values for one image (frame path, its header, layer).
	KeyLookup keyLookup(int source,
	                    const std::filesystem::path& frame,
	                    const ImageInfoPtr& info,
	                    const std::string& layer) const;
	/// Resolved lines of `sets` (in order, appended per slot); empty when the
	/// overlay is hidden.
	OverlayText overlayText(std::initializer_list<const AnnotationSet*> sets,
	                        const KeyLookup& lookup) const;
	/// Note on the picture that `img` is missing chunks (top right).
	static void markIncomplete(OverlayText& over, const LayerImagePtr& img);
	/// The labels source `source` was given (hidden with the annotations);
	/// a diff's also says how far apart A and B are, once `decoded` (its own
	/// pixels, not a stand-in) is in.
	void addLabels(OverlayText& over,
	               int source,
	               const KeyLookup& lookup,
	               const LayerImagePtr& decoded) const;

	// --- events / drawing ---
	bool mouseEvent(ftxui::Event e, const ImageInfoPtr& info);
	bool keyEvent(const ftxui::Event& e, const ImageInfoPtr& info);
	ftxui::Element renderHud(const ImageInfoPtr& info);
	ftxui::Element renderTiles(const ImageInfoPtr& info, int width);
	ftxui::Element renderStatus(const ImageInfoPtr& info);
	/// The timeline under a sequence (frameCount() > 1).
	ftxui::Element renderPlaybar(const ImageInfoPtr& info);
	/// The `:` prompt, in place of the status bar.
	ftxui::Element renderGoto() const;

	AppContext& m_ctx;
	std::function<void()> m_onClose;
	ViewerState m_state;
	bool m_tile = false;
	bool m_tileSelection = true; ///< dashed highlight on the selected tile
	bool m_fillGaps = false;     ///< `G`: see Source::m_fillGaps
	bool m_names = false; ///< `T`: the layer (a tile of images: file) name
	bool m_fitBesidePanel = false; ///< `f` with a pane open: keep re-fitting
	bool m_playbackCap = true;     ///< `P`: lower resolution while playing
	/// Pixels a frame period of the link carries, as cappedBudget() last
	/// settled on (0: not yet).
	mutable double m_linkFit = 0;
	int m_mouseX = -1, m_mouseY = -1;
	std::optional<LineEditor> m_goto; ///< `:` frame number being typed
	Playbar m_playbar;
	bool m_scrubbing = false; ///< the playbar is being dragged
	/// Scrubbing on the playbar or the image: frames go by as in playback.
	bool scrubbing() const noexcept
	{
		return m_scrubbing || m_scrubMoved;
	}

	ImageSlotPtr m_viewSlot;
	/// A tile on screen: its slot and how it shows its image.
	struct Tile
	{
		ImageSlotPtr m_slot;
		double m_zoom = 0;   ///< terminal px per image px; 0 = fitted
		ViewParams m_view{}; ///< what it drew last
	};
	/// Slots of the tiles on screen only, by tile index: off-screen tiles
	/// give theirs (and its kitty image id) back.
	std::map<int, Tile> m_tiles;
	/// What every tile shows, for the inspector and read-ahead.
	struct TileRef
	{
		std::filesystem::path m_path;
		std::string m_layer;
		int m_source = 0; ///< the source it shows
	};
	std::vector<TileRef> m_tileRefs;
	SheetLayout m_sheet;
	SheetView m_sheetView;
	SheetArea m_sheetArea;
	std::string m_sheetKey;  ///< what the sheet shows: a change fits it again
	ftxui::Box m_sheetBox{}; ///< where the sheet was drawn

	MetaPane m_meta;
	FilesPane m_files;
	InspectorPane m_inspector;
	LayersPane m_layers;
	ColourPane m_colour;
	ftxui::Box m_sideBox{}; ///< the right column (clicks there skip the image)
	ftxui::Box m_leftBox{}; ///< the left column
	int m_dragDivider = 0;  ///< being dragged: 1 = left column's, 2 = right's
	bool m_dragFiles = false; ///< the files pane's title is being dragged
	bool m_dragMeta = false;  ///< selecting metadata rows with a drag
	/// Panning with a right / middle drag: where the mouse last was.
	std::optional<std::pair<int, int>> m_dragPan;
	/// Scrubbing with a left drag on the image: the column the mouse was
	/// at, and the part of a frame not stepped yet.
	std::optional<int> m_scrubX;
	bool m_scrubMoved = false; ///< that drag has stepped a frame
	double m_scrubCarry = 0;
	struct HiddenPanes
	{
		bool m_meta = false, m_files = false, m_inspector = false,
		     m_layers = false, m_colour = false;
		Focus m_focus = Focus::IMAGE;
	};
	std::optional<HiddenPanes> m_hiddenPanes;

	double m_shownFps = 0; ///< see shownFps()
	std::chrono::steady_clock::time_point m_lastAdvance;
	std::atomic<bool> m_fillWanted{false}; ///< fillTerminal has more to do
	std::jthread m_fillWake; ///< posts fillTerminal while m_fillWanted
	Player m_player;         ///< last: its ticker stops before the rest goes
};

} // namespace rv
