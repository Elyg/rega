#include "app/Viewer.h"

#include "util/Ui.h"

#include <ftxui/screen/terminal.hpp>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

namespace
{
const double FPS_CHOICES[] = {12, 23.976, 24, 25, 30, 48, 60};

// hjkl / arrows pan by a few cells, HJKL by more: (dx, dy) in cells.
std::optional<std::pair<double, double>> panStep(const Event& e)
{
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	if(ch("h") || e == Event::ArrowLeft)
	{
		return std::pair(-4.0, 0.0);
	}
	if(ch("l") || e == Event::ArrowRight)
	{
		return std::pair(4.0, 0.0);
	}
	if(ch("k") || e == Event::ArrowUp)
	{
		return std::pair(0.0, -2.0);
	}
	if(ch("j") || e == Event::ArrowDown)
	{
		return std::pair(0.0, 2.0);
	}
	if(ch("H"))
	{
		return std::pair(-16.0, 0.0);
	}
	if(ch("L"))
	{
		return std::pair(16.0, 0.0);
	}
	if(ch("K"))
	{
		return std::pair(0.0, -8.0);
	}
	if(ch("J"))
	{
		return std::pair(0.0, 8.0);
	}
	return std::nullopt;
}
} // namespace

Viewer::Viewer(AppContext& ctx, std::function<void()> onClose)
    : m_ctx(ctx), m_onClose(std::move(onClose)),
      m_playbar(ctx.m_caps, ctx.m_kitty), m_viewSlot(newSlot(ctx)),
      m_meta(m_state, ctx), m_files(m_state, ctx), m_inspector(m_state, ctx),
      m_layers(m_state), m_colour(m_state, ctx),
      m_player(
          [this]
          {
	          // From the ticker thread. A posted task, so a
	          // tick that shows nothing new costs no redraw.
	          m_ctx.m_post(
	              [this]
	              {
		              if(tick())
		              {
			              m_ctx.m_redraw();
		              }
	              });
          })
{
}

// --- open / close ---

void Viewer::open(std::vector<Entry> entries, bool tile)
{
	std::vector<Source> sources;
	for(auto& e : entries)
	{
		sources.push_back({std::move(e), m_fillGaps});
	}
	openSources(std::move(sources), tile);
}

void Viewer::openSources(std::vector<Source> sources, bool tile)
{
	m_state.m_sources = std::move(sources);
	for(const auto& s : m_state.m_sources)
	{
		m_ctx.m_ann.touch(sourceKey(s));
	}
	m_state.m_current = 0;
	m_state.m_frame = 0;
	m_state.m_in.reset();
	m_state.m_out.reset();
	m_state.m_view = ViewParams{};
	m_state.m_picked.reset();
	m_fitBesidePanel = false;
	// A diff pair opens as its three tiles.
	m_tile = tile || (m_state.grouped() && tilesBySource());
	m_sheetKey.clear(); // fit the sheet again
	m_meta.resetScroll();
	m_player.reset(); // fps from the file once known
}

void Viewer::close()
{
	m_player.pause();
	m_ctx.m_svc.clearPrefetch();
	m_tile = false;
	m_state.m_picked.reset(); // a pick belongs to this viewing session
	m_onClose();
}

bool Viewer::rescan()
{
	bool changed = false;
	for(auto& src : m_state.m_sources)
	{
		Entry& en = src.m_entry;
		if(en.m_kind != Entry::Kind::SEQUENCE)
		{
			continue;
		}
		// A sequence being rendered grows (or shrinks).
		Entry again = entryForPath(en.m_frames.front());
		if(again.m_kind == Entry::Kind::SEQUENCE &&
		   again.m_frames != en.m_frames)
		{
			spdlog::info("sequence {} now {} frames",
			             again.m_name,
			             again.m_frames.size());
			en = std::move(again);
			m_state.m_frame =
			    std::min(m_state.m_frame, m_state.frameCount() - 1);
			changed = true;
		}
	}
	return changed;
}

std::vector<fs::path> Viewer::shownDirs() const
{
	std::vector<fs::path> dirs;
	for(const auto& s : m_state.m_sources)
	{
		fs::path dir = s.frame(0).parent_path();
		if(std::ranges::find(dirs, dir) == dirs.end())
		{
			dirs.push_back(std::move(dir));
		}
	}
	return dirs;
}

DisplayParams Viewer::displayFor(int source, const fs::path& path)
{
	DisplayParams disp = m_state.m_disp;
	if(disp.m_srgb && source >= 0 &&
	   source < static_cast<int>(m_state.m_sources.size()))
	{
		const ImageInfoPtr info = m_ctx.m_svc.info(path);
		disp.m_ocio =
		    m_ctx.m_colour.transformFor(path,
		                                sourceKey(m_state.m_sources[source]),
		                                info && info->m_displayReferred);
	}
	return disp;
}

std::optional<double> Viewer::zoomOf(const ImageSlot& slot) const
{
	if(&slot == m_viewSlot.get())
	{
		return m_state.m_view.m_fit ? std::nullopt
		                            : std::optional(m_state.m_view.m_zoom);
	}
	for(const auto& [i, t] : m_tiles)
	{
		if(t.m_slot.get() == &slot && t.m_zoom > 0)
		{
			return t.m_zoom;
		}
	}
	return std::nullopt;
}

int Viewer::reduceFor(const ImageInfo& info, const ImageSlot& slot) const
{
	// While playing, decode up to 25% below the size sent: a 4K plate on a
	// ~2000 px wide view then caches at 1920 px (33 MB) instead of 4K
	// (132 MB), and a whole sequence fits in the cache instead of being
	// decoded again on every loop.
	constexpr double PLAYBACK_SLACK = 1.25;
	return m_ctx.reduceFor(slot,
	                       info.fitBounds(m_state.m_layerLabel),
	                       zoomOf(slot),
	                       pixelCap(),
	                       (m_player.playing() || scrubbing()) && m_playbackCap
	                           ? PLAYBACK_SLACK
	                           : 1.0);
}

int Viewer::playbackPixelCap() const
{
	return m_playbackCap ? cappedBudget() : TOTAL_PIXEL_BUDGET;
}

int Viewer::playbackReduce(const ImageInfo& info, const ImageSlot& slot) const
{
	constexpr double PLAYBACK_SLACK = 1.25; // see reduceFor
	return m_ctx.reduceFor(slot,
	                       info.fitBounds(m_state.m_layerLabel),
	                       zoomOf(slot),
	                       playbackPixelCap(),
	                       m_playbackCap ? PLAYBACK_SLACK : 1.0);
}

bool Viewer::fillsTerminal() const
{
	const auto& caps = m_ctx.m_caps;
	return !m_tile && m_state.frameCount() > 1 &&
	       m_state.m_sources.size() == 1 && !m_state.m_sources[0].m_diffA &&
	       caps.m_graphics == GraphicsMode::KITTY &&
	       caps.m_transfer == Transfer::DIRECT &&
	       m_ctx.m_kitty.terminalCache() > 0 && m_viewSlot->drawn();
}

void Viewer::fillTerminal()
{
	ImageSlot& slot = *m_viewSlot;
	if(!fillsTerminal())
	{
		m_fillWanted = false;
		return;
	}
	if(slot.flushStored())
	{
		m_ctx.m_kitty.probeLink(); // written outside a draw: ask anyway
	}
	m_fillWanted = true;
	if(!m_fillWake.joinable())
	{
		m_fillWake = std::jthread(
		    [this](std::stop_token stop)
		    {
			    std::mutex mu;
			    std::condition_variable_any cv;
			    std::unique_lock lock(mu);
			    while(!stop.stop_requested())
			    {
				    cv.wait_for(lock,
				                stop,
				                std::chrono::milliseconds(100),
				                [] { return false; });
				    if(m_fillWanted && !stop.stop_requested())
				    {
					    m_ctx.m_post([this] { fillTerminal(); });
				    }
			    }
		    });
	}
	// Behind the link a moment ago: what plays now goes first. And a view
	// still moving (a pan, a zoom) would drop what was stored for it.
	constexpr double FILL_BACKLOG = 0.05; // seconds
	constexpr auto SETTLED = std::chrono::seconds(1);
	if(slot.storing() || m_ctx.m_kitty.linkBacklog() > FILL_BACKLOG ||
	   std::chrono::steady_clock::now() - slot.viewSince() < SETTLED)
	{
		return;
	}
	auto& svc = m_ctx.m_svc;
	const Source& src = m_state.m_sources[0];
	const FrameRange range = m_state.playRange();
	const int start =
	    range.contains(m_state.m_frame) ? m_state.m_frame : range.m_first;
	auto info = svc.info(src.frame(start), ImageService::Priority::PREFETCH);
	if(!info)
	{
		return;
	}
	const int reduce = playbackReduce(*info, slot);
	for(int i = 0, f = start; i < range.count(); ++i, f = range.next(f))
	{
		const fs::path path = src.frame(f);
		if(slot.stored(svc.originOf(path, m_state.m_layerLabel, reduce)))
		{
			continue;
		}
		// One at a time: the next once this one is written (or decoded:
		// the decode wakes the UI, and the wake retries).
		if(auto img = svc.layer(path,
		                        m_state.m_layerLabel,
		                        reduce,
		                        ImageService::Priority::PREFETCH))
		{
			slot.store(std::move(img), playbackPixelCap());
		}
		return;
	}
	m_fillWanted = false; // all kept
}

std::vector<bool> Viewer::storedFrames(const ImageInfo& info)
{
	if(!fillsTerminal() || m_viewSlot->cachedFrames() == 0)
	{
		return {};
	}
	auto& svc = m_ctx.m_svc;
	const Source& src = m_state.m_sources[0];
	const int reduce = playbackReduce(info, *m_viewSlot);
	std::vector<bool> stored(static_cast<size_t>(m_state.frameCount()));
	for(int f = 0; f < m_state.frameCount(); ++f)
	{
		stored[f] = m_viewSlot->stored(
		    svc.originOf(src.frame(f), m_state.m_layerLabel, reduce));
	}
	return stored;
}

std::vector<bool> Viewer::storedElsewhere()
{
	if(!fillsTerminal() || m_viewSlot->cachedFrames() == 0)
	{
		return {};
	}
	const Source& src = m_state.m_sources[0];
	std::vector<bool> kept(static_cast<size_t>(m_state.frameCount()));
	for(int f = 0; f < m_state.frameCount(); ++f)
	{
		kept[f] = m_viewSlot->storedInAnyView(src.frame(f).string() + "|");
	}
	return kept;
}

int Viewer::cappedBudget() const
{
	int cap = TOTAL_PIXEL_BUDGET;
	if(!m_tile)
	{
		cap /= 2;
	}
	// Inline transfer (ssh): every pixel crosses the link, compressed but
	// base64'd. Fit a frame period of the link (tick() waits for it), at what
	// the last pictures cost a pixel: film grain about 3 bytes, CG a tenth,
	// so a fixed guess blurs CG for nothing. Below a floor the picture is
	// too small to see; the frame rate gives instead. Unpaced, nothing is
	// known of the link: halve it to keep playback near frame rate.
	if(m_ctx.m_caps.m_transfer == Transfer::DIRECT)
	{
		constexpr int MIN_LINK_PIXELS = 100'000;
		if(const double rate = m_ctx.m_kitty.linkRate(); rate > 0)
		{
			const double fps = m_player.targetFps(std::nullopt);
			const double fit = rate / fps / m_ctx.m_kitty.linkBytesPerPixel();
			// Frames kept in the terminal: they were made at this size and
			// show again only at its decode reduction. The link moving it
			// let them all go and filled again, loop after loop.
			const bool kept = m_viewSlot->cachedFrames() > 0;
			// Both measures move a little every frame: followed closely,
			// every frame came out a new size, prepared again (and decoded
			// again at another reduction). Move only on a real change.
			constexpr double HYSTERESIS = 1.25;
			if(m_linkFit <= 0 || (!kept && (fit > m_linkFit * HYSTERESIS ||
			                                fit < m_linkFit / HYSTERESIS)))
			{
				m_linkFit = fit;
			}
			// Kept in the terminal, a loop crosses the link once: size it
			// so the whole range fits there, when that's sharper than what
			// the link carries in real time (the first pass plays slower,
			// unless fillTerminal stored it while paused).
			double best = m_linkFit;
			if(const size_t keep = m_ctx.m_kitty.terminalCache();
			   keep > 0 && !m_tile)
			{
				// Under half: the view before stays kept too, so a pan or a
				// zoom and back plays from the terminal again. The whole
				// budget for one view pushed the last one out at every step.
				constexpr double KEEP_SHARE = 0.45;
				best = std::max(best,
				                KEEP_SHARE * static_cast<double>(keep) / 4.0 /
				                    m_state.playRange().count());
			}
			cap = std::max(MIN_LINK_PIXELS,
			               static_cast<int>(std::min<double>(cap, best)));
		}
		else
		{
			cap /= 2;
		}
	}
	return cap;
}

int Viewer::pixelCap() const
{
	// Scrubbing too: frames go by as fast, and inline each crosses the link.
	const int cap = (m_player.playing() || scrubbing()) && m_playbackCap
	                    ? cappedBudget()
	                    : TOTAL_PIXEL_BUDGET;
	// Tiles share the budget: the ones on screen, so zooming into the sheet
	// makes each sharper.
	return m_tile ? cap / std::max<int>(1, static_cast<int>(m_tiles.size()))
	              : cap;
}

const ImageSlot& Viewer::playbackSlot() const
{
	return m_tile && !m_tiles.empty() ? *m_tiles.begin()->second.m_slot
	                                  : *m_viewSlot;
}

bool Viewer::showsSource(int i) const
{
	if(m_tile && tilesBySource())
	{
		// The tiles on screen (all of them before the first draw).
		const int t = tileOf(i);
		return t >= 0 && (m_tiles.empty() || m_tiles.contains(t));
	}
	return i == m_state.m_current;
}

bool Viewer::tilesBySource() const
{
	return m_state.sheetSources().size() > 1;
}

int Viewer::tileOf(int source) const
{
	const auto s = m_state.sheetSources();
	const auto it = std::ranges::find(s, source);
	return it == s.end() ? -1 : static_cast<int>(it - s.begin());
}

LayerImagePtr Viewer::layerOf(int source,
                              const fs::path& path,
                              const std::string& layer,
                              int reduce,
                              ImageService::Priority pr)
{
	const Source& src = m_state.m_sources[source];
	if(src.m_diffA)
	{
		return m_ctx.m_svc.diffLayer(src.diffFrame(m_state.m_frame),
		                             path,
		                             layer,
		                             reduce,
		                             pr);
	}
	return m_ctx.m_svc.layerBestEffort(path, layer, reduce, pr);
}

std::optional<std::string> Viewer::errorOf(int source, const fs::path& path)
{
	const Source& src = m_state.m_sources[source];
	return src.m_diffA
	           ? m_ctx.m_svc.diffError(src.diffFrame(m_state.m_frame), path)
	           : m_ctx.m_svc.error(path);
}

// --- actions ---

void Viewer::cycleLayer(const ImageInfoPtr& info, int delta)
{
	if(!info || info->m_layers.empty())
	{
		return;
	}
	int n = static_cast<int>(info->m_layers.size());
	int i = info->findLayer(m_state.m_layerLabel);
	i = ((i < 0 ? 0 : i) + delta + n) % n;
	m_state.m_layerLabel = info->m_layers[i].label();
}

void Viewer::zoomBy(double factor,
                    std::optional<std::pair<double, double>> anchor)
{
	m_fitBesidePanel = false; // zooming / panning takes over from fit
	if(m_state.m_view.m_fit)
	{
		if(!m_viewSlot->drawn())
		{
			return;
		}
		m_state.m_view.m_zoom = m_viewSlot->effectiveZoom();
		auto m = m_viewSlot->mapping();
		auto [aw, ah] = m_viewSlot->areaPixels();
		double q =
		    m.m_scale / m_state.m_view.m_zoom; // output px per terminal px
		m_state.m_view.m_centerX = m.imageX(aw * q / 2.0);
		m_state.m_view.m_centerY = m.imageY(ah * q / 2.0);
		m_state.m_view.m_fit = false;
	}
	m_state.m_view.m_zoom =
	    std::clamp(m_state.m_view.m_zoom * factor, 1.0 / 64, 256.0);
	if(anchor)
	{
		m_state.m_view.m_centerX =
		    anchor->first + (m_state.m_view.m_centerX - anchor->first) / factor;
		m_state.m_view.m_centerY =
		    anchor->second +
		    (m_state.m_view.m_centerY - anchor->second) / factor;
	}
}

void Viewer::fitView(const ImageInfoPtr& info)
{
	m_state.m_view.m_fit = true;
	m_fitBesidePanel = false;
	// Side panels are drawn over the image; fit into what is left of them.
	if(m_tile || !sidePanelOpen() || !info)
	{
		return;
	}
	// From the terminal size, not the last draw, so it is right straight
	// after a resize (the view spans the full width, minus HUD + status).
	const auto term = Terminal::Size();
	const int aw = term.dimx * m_viewSlot->pxPerCellX();
	const int ah = std::max(0, term.dimy - 2) * m_viewSlot->pxPerCellY();
	const int leftPx = coveredLeft() * m_viewSlot->pxPerCellX();
	const int visibleW =
	    aw - leftPx - coveredRight() * m_viewSlot->pxPerCellX();
	const Box2i dw = info->fitBounds(m_state.m_layerLabel);
	if(visibleW <= 0 || ah <= 0 || dw.width() <= 0 || dw.height() <= 0)
	{
		return;
	}
	m_state.m_view.m_fit = false;
	// 10% breathing room so the image does not butt against the panel.
	constexpr double PADDED = 0.9;
	m_state.m_view.m_zoom =
	    PADDED * std::min(visibleW / static_cast<double>(dw.width()),
	                      ah / static_cast<double>(dw.height()));
	// The view centre is the middle of the whole area; shift it so the image
	// sits in the middle of the uncovered part (between the columns).
	m_state.m_view.m_centerX =
	    dw.m_x0 + dw.width() / 2.0 +
	    (aw / 2.0 - (leftPx + visibleW / 2.0)) / m_state.m_view.m_zoom;
	m_state.m_view.m_centerY = dw.m_y0 + dw.height() / 2.0;
	m_fitBesidePanel = true; // re-done every draw until zoom / pan
}

void Viewer::pan(double dxCells, double dyCells)
{
	m_fitBesidePanel = false;
	if(m_state.m_view.m_fit)
	{
		zoomBy(1.0);
	}
	m_state.m_view.m_centerX +=
	    dxCells * m_viewSlot->pxPerCellX() / m_state.m_view.m_zoom;
	m_state.m_view.m_centerY +=
	    dyCells * m_viewSlot->pxPerCellY() / m_state.m_view.m_zoom;
}

// --- tiles ---

int Viewer::selectedTile(const ImageInfoPtr& info) const
{
	if(tilesBySource())
	{
		return std::max(0, tileOf(m_state.m_current));
	}
	return info ? std::max(0, info->findLayer(m_state.m_layerLabel)) : 0;
}

void Viewer::selectTile(int i, const ImageInfoPtr& info)
{
	if(tilesBySource())
	{
		const auto s = m_state.sheetSources();
		if(i >= 0 && i < static_cast<int>(s.size()))
		{
			m_state.m_current = s[i];
		}
	}
	else if(info && i >= 0 && i < static_cast<int>(info->m_layers.size()))
	{
		m_state.m_layerLabel = info->m_layers[i].label();
	}
}

void Viewer::zoomSheet(double factor,
                       const ImageInfoPtr& info,
                       std::optional<std::pair<double, double>> at)
{
	if(at)
	{
		// The wheel: about the mouse; then whatever lands in the middle is
		// what the panes describe.
		zoomSheetView(m_sheetView,
		              m_sheet,
		              m_sheetArea,
		              factor,
		              at->first,
		              at->second);
		followSheetCentre(info);
		return;
	}
	// A key: about the selected tile, which stays put and selected.
	double ax = m_sheetArea.m_w / 2.0, ay = m_sheetArea.m_h / 2.0;
	if(auto p =
	       placeTile(m_sheet, m_sheetView, m_sheetArea, selectedTile(info)))
	{
		ax = (p->m_boxX0 + p->m_boxX1) / 2.0;
		ay = (p->m_boxY0 + p->m_boxY1) / 2.0;
	}
	zoomSheetView(m_sheetView, m_sheet, m_sheetArea, factor, ax, ay);
}

void Viewer::panSheet(double dx, double dy, const ImageInfoPtr& info)
{
	panSheetView(m_sheetView, m_sheet, m_sheetArea, dx, dy);
	followSheetCentre(info);
}

void Viewer::sheetOneToOne(const ImageInfoPtr& info)
{
	const int sel = selectedTile(info);
	if(sel < 0 || sel >= static_cast<int>(m_tileRefs.size()))
	{
		return;
	}
	const ImageInfoPtr ti = m_ctx.m_svc.info(m_tileRefs[sel].m_path);
	if(!ti)
	{
		return;
	}
	// Terminal px per image px of the fitted sheet; zoom until that is 1.
	const Box2i dw = ti->displayWindow();
	const double fitted =
	    std::min(m_sheet.m_tileW * m_viewSlot->pxPerCellX() /
	                 static_cast<double>(std::max(1, dw.width())),
	             m_sheet.m_tileH * m_viewSlot->pxPerCellY() /
	                 static_cast<double>(std::max(1, dw.height())));
	m_sheetView.m_zoom = 1.0 / std::max(1e-9, fitted);
	centreSheetOn(sel);
}

void Viewer::centreSheetOn(int i)
{
	if(i < 0 || i >= m_sheet.m_count)
	{
		return;
	}
	const auto [x, y] = m_sheet.origin(i);
	m_sheetView.m_cx = x + m_sheet.m_tileW / 2.0;
	m_sheetView.m_cy = y + m_sheet.m_tileH / 2.0;
	clampSheetView(m_sheetView, m_sheet, m_sheetArea);
}

void Viewer::followSheetCentre(const ImageInfoPtr& info)
{
	if(m_sheetView.m_zoom <= 1.0)
	{
		return; // fitted: the selection stays where it was clicked
	}
	const int i = m_sheet.tileAt(m_sheetView.m_cx, m_sheetView.m_cy);
	if(i >= 0)
	{
		selectTile(i, info);
	}
}

void Viewer::openSelectedTile(const ImageInfoPtr& info)
{
	// Zoomed in, the single view starts at the same zoom on the same place;
	// a fitted tile opens fitted.
	auto it = m_tiles.find(selectedTile(info));
	m_state.m_view = ViewParams{};
	if(it != m_tiles.end() && it->second.m_zoom > 0)
	{
		m_state.m_view = it->second.m_view;
		m_state.m_view.m_fitFrame = false;
	}
	m_fitBesidePanel = false;
	m_tile = false;
}

void Viewer::setFrame(int f)
{
	int n = m_state.frameCount();
	m_state.m_frame = ((f % n) + n) % n;
}

void Viewer::setInOut(bool in)
{
	if(m_state.frameCount() < 2)
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	auto& point = in ? m_state.m_in : m_state.m_out;
	auto& other = in ? m_state.m_out : m_state.m_in;
	const int f = m_state.m_frame;
	const char* name = in ? "in" : "out";
	if(point == f)
	{
		point.reset();
		m_ctx.m_message = fmt::format("{} point cleared", name);
	}
	else
	{
		point = f;
		if(other && (in ? *other < f : *other > f))
		{
			other.reset(); // they would cross
		}
		const std::string label =
		    m_state.m_sources[m_state.m_current].frameLabel(f);
		m_ctx.m_message =
		    fmt::format("{} point: {}",
		                name,
		                label.empty() ? std::to_string(f + 1) : label);
	}
	m_ctx.m_svc.clearPrefetch(); // read ahead for the new range instead
}

void Viewer::openGoto()
{
	if(!m_state.numberedSource())
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	m_goto.emplace();
}

void Viewer::gotoEvent(const Event& e)
{
	if(e.is_character())
	{
		// Digits only: anything else would not be a frame number.
		const std::string& c = e.character();
		if(c.size() == 1 && std::isdigit(static_cast<unsigned char>(c[0])) &&
		   m_goto->glyphs().size() < 9)
		{
			(void)m_goto->event(e, {});
		}
		return;
	}
	const auto r = m_goto->event(e, {});
	if(r == LineEditor::Result::CANCEL)
	{
		m_goto.reset();
		return;
	}
	if(r != LineEditor::Result::COMMIT)
	{
		return;
	}
	const std::string typed = m_goto->text();
	m_goto.reset();
	const Source* src = m_state.numberedSource();
	if(typed.empty() || !src)
	{
		return;
	}
	const int number = std::stoi(typed);
	const int idx = src->indexForFrameNumber(number);
	setFrame(idx);
	const std::string shown = src->frameLabel(idx);
	if(shown != std::to_string(number))
	{
		m_ctx.m_message = fmt::format("no frame {}: showing {}", number, shown);
	}
}

void Viewer::toggleFillGaps()
{
	const Source* src = m_state.numberedSource();
	if(!src)
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	// The frame and in / out stay at their numbers: the indices change.
	auto number = [&](int f) { return src->frameNumber(f); };
	const int frame = number(m_state.m_frame);
	const std::optional<int> in =
	    m_state.m_in ? std::optional(number(*m_state.m_in)) : std::nullopt;
	const std::optional<int> out =
	    m_state.m_out ? std::optional(number(*m_state.m_out)) : std::nullopt;
	m_fillGaps = !m_fillGaps;
	for(auto& s : m_state.m_sources)
	{
		s.m_fillGaps = m_fillGaps;
	}
	m_state.m_frame = src->indexForFrameNumber(frame);
	if(in)
	{
		m_state.m_in = src->indexForFrameNumber(*in);
	}
	if(out)
	{
		m_state.m_out = src->indexForFrameNumber(*out);
	}
	m_ctx.m_message =
	    m_fillGaps ? "missing frames: hold the nearest (timing as numbered)"
	               : "missing frames: skipped";
}

void Viewer::togglePlay()
{
	if(m_state.frameCount() < 2)
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	if(m_player.playing())
	{
		m_player.pause();
		m_ctx.m_svc.clearPrefetch();
	}
	else
	{
		m_player.play();
	}
}

void Viewer::prefetchFrame(int f)
{
	auto& svc = m_ctx.m_svc;
	const ImageSlot& slot = playbackSlot();
	for(int i = 0; i < static_cast<int>(m_state.m_sources.size()); ++i)
	{
		if(!showsSource(i) || m_state.m_sources[i].m_diffA)
		{
			continue; // a diff decodes from A and B, read ahead already
		}
		const fs::path path = m_state.m_sources[i].frame(f);
		auto info = svc.info(path, ImageService::Priority::PREFETCH);
		if(!info)
		{
			continue;
		}
		svc.layer(path,
		          m_state.m_layerLabel,
		          reduceFor(*info, slot),
		          ImageService::Priority::PREFETCH);
	}
}

void Viewer::prepareAhead(int f)
{
	auto& svc = m_ctx.m_svc;
	auto ahead = [&](ImageSlot& slot, const fs::path& path, const auto& layer)
	{
		auto info = svc.info(path);
		if(!info)
		{
			return;
		}
		const int reduce = reduceFor(*info, slot);
		if(svc.hasLayer(path, layer, reduce))
		{
			slot.prepareAhead(svc.layer(path, layer, reduce));
		}
	};
	if(!m_tile)
	{
		const Source& src = m_state.m_sources[m_state.m_current];
		if(!src.m_diffA)
		{
			ahead(*m_viewSlot, src.frame(f), m_state.m_layerLabel);
		}
		return;
	}
	const bool bySource = tilesBySource();
	const auto sheet = m_state.sheetSources();
	for(const auto& [i, t] : m_tiles)
	{
		if(i >= static_cast<int>(m_tileRefs.size()) ||
		   (bySource && i >= static_cast<int>(sheet.size())))
		{
			continue;
		}
		const auto& src =
		    m_state.m_sources[bySource ? sheet[i] : m_state.m_current];
		if(!src.m_diffA)
		{
			ahead(*t.m_slot, src.frame(f), m_tileRefs[i].m_layer);
		}
	}
}

bool Viewer::tick()
{
	if(!m_player.playing())
	{
		return false;
	}
	auto& svc = m_ctx.m_svc;
	auto info = svc.info(m_state.currentFramePath());
	if(!info)
	{
		return false;
	}
	const int reduce = reduceFor(*info, playbackSlot());
	// Read ahead: as many frames as fit comfortably in the cache.
	const auto& dw = info->displayWindow();
	const double frameBytes = static_cast<double>(dw.width()) / reduce *
	                          dw.height() / reduce * 4 * sizeof(float);
	const int ahead =
	    Player::readAhead(svc.budget(), frameBytes, m_state.m_sources.size());
	const FrameRange range = m_state.playRange();
	// Inline over ssh, a frame waits until the link is through the last
	// one: tmux would take them all and play them out long after a pause.
	const bool linkBusy =
	    m_ctx.m_kitty.linkBacklog() >= 1.0 / m_player.targetFps(std::nullopt);
	auto next = m_player.tick(
	    m_state.m_frame,
	    range,
	    ahead,
	    [&](int f) { prefetchFrame(f); },
	    [&](int f)
	    {
		    bool ready = true;
		    for(int i = 0; i < static_cast<int>(m_state.m_sources.size()); ++i)
		    {
			    const fs::path path = m_state.m_sources[i].frame(f);
			    if(showsSource(i) &&
			       !svc.hasLayer(path, m_state.m_layerLabel, reduce))
			    {
				    svc.layer(path, m_state.m_layerLabel, reduce); // urgent
				    ready = false;
			    }
		    }
		    return ready && !linkBusy;
	    });
	if(!next)
	{
		return false;
	}
	// The frame just left is needed again only a whole loop later: let it
	// go first when the cache is full (see ImageService::demote).
	for(int i = 0; i < static_cast<int>(m_state.m_sources.size()); ++i)
	{
		if(showsSource(i))
		{
			svc.demote(m_state.m_sources[i].frame(m_state.m_frame),
			           m_state.m_layerLabel,
			           reduce);
		}
	}
	m_state.m_frame = *next;
	prepareAhead(range.next(*next));
	const auto now = std::chrono::steady_clock::now();
	if(const double dt =
	       std::chrono::duration<double>(now - m_lastAdvance).count();
	   dt > 0 && dt < 2.0)
	{
		m_shownFps = m_shownFps > 0 ? 0.8 * m_shownFps + 0.2 / dt : 1.0 / dt;
	}
	m_lastAdvance = now;
	return true;
}

// --- annotations ---

KeyLookup Viewer::keyLookup(int source,
                            const fs::path& frame,
                            const ImageInfoPtr& info,
                            const std::string& layer) const
{
	return
	    [this, source, frame, info, layer](const std::string& key, bool builtin)
	        -> std::optional<std::string>
	{
		if(builtin)
		{
			if(key == "file")
			{
				return frame.filename().string();
			}
			if(key == "frame")
			{
				return source >= 0 && source < static_cast<int>(
				                                   m_state.m_sources.size())
				           ? m_state.m_sources[source].frameLabel(
				                 m_state.m_frame)
				           : std::string();
			}
			if(key == "layer")
			{
				return layer.empty() ? std::string("rgba") : layer;
			}
			if(key == "res" && info)
			{
				const Box2i& d = info->displayWindow();
				return fmt::format("{}x{}", d.width(), d.height());
			}
			if(key == "fps")
			{
				return fmt::format("{:.3g}",
				                   m_player.targetFps(info ? info->m_fps
				                                           : std::nullopt));
			}
			if(key == "date")
			{
				const std::time_t t = std::time(nullptr);
				std::tm tm{};
				localtime_r(&t, &tm);
				return fmt::format("{:%Y-%m-%d}", tm);
			}
			return std::nullopt;
		}
		if(!info)
		{
			return std::nullopt;
		}
		// The shown layer's part first, then part 0.
		const int l = info->findLayer(layer);
		const size_t part =
		    l >= 0 ? static_cast<size_t>(info->m_layers[l].m_part) : 0;
		for(size_t p : {part, size_t{0}})
		{
			if(p >= info->m_parts.size())
			{
				continue;
			}
			for(const auto& a : info->m_parts[p].m_attributes)
			{
				if(a.m_name == key)
				{
					return a.m_value;
				}
			}
		}
		return std::nullopt;
	};
}

void Viewer::markIncomplete(OverlayText& over, const LayerImagePtr& img)
{
	// Shown whether or not annotations are: the frame is missing pixels.
	if(img && img->incomplete())
	{
		over[static_cast<int>(Slot::TR)].push_back({{"incomplete image"}});
	}
	if(img && img->damaged())
	{
		over[static_cast<int>(Slot::TR)].push_back({{"damaged image"}});
	}
}

void Viewer::addLabels(OverlayText& over,
                       int source,
                       const KeyLookup& lookup,
                       const LayerImagePtr& decoded) const
{
	if(!m_ctx.m_ann.m_visible)
	{
		return; // `A` hides them with the annotations
	}
	const Source& src = m_state.m_sources[source];
	for(int s = 0; s < SLOT_COUNT; ++s)
	{
		for(const auto& line : src.m_labels.m_slots[s])
		{
			over[s].push_back(resolveLine(line, lookup));
		}
	}
	if(src.m_diffA && decoded)
	{
		const float most = maxValue(*decoded);
		over[static_cast<int>(Slot::BC)].push_back(
		    {{most == 0 && decoded->m_nanPixels == 0
		          ? std::string("identical")
		          : fmt::format("max Δ {:.4g}", most)}});
	}
}

OverlayText Viewer::overlayText(std::initializer_list<const AnnotationSet*> sets,
                                const KeyLookup& lookup) const
{
	OverlayText out;
	if(!m_ctx.m_ann.m_visible)
	{
		return out;
	}
	for(const AnnotationSet* set : sets)
	{
		if(!set)
		{
			continue;
		}
		for(int s = 0; s < SLOT_COUNT; ++s)
		{
			for(const auto& line : set->m_slots[s])
			{
				out[s].push_back(resolveLine(line, lookup));
			}
		}
	}
	return out;
}

// --- events ---

void Viewer::togglePanes()
{
	if(sidePanelOpen())
	{
		m_hiddenPanes = HiddenPanes{m_meta.isOpen(),
		                            m_files.isOpen(),
		                            m_inspector.isOpen(),
		                            m_layers.isOpen(),
		                            m_colour.isOpen(),
		                            m_state.m_focus};
		m_colour.setOpen(false);
		m_meta.setOpen(false);
		m_files.setOpen(false);
		m_inspector.setOpen(false);
		m_layers.setOpen(false);
		m_state.m_focus = Focus::IMAGE;
	}
	else if(m_hiddenPanes)
	{
		m_meta.setOpen(m_hiddenPanes->m_meta);
		m_files.setOpen(m_hiddenPanes->m_files);
		m_inspector.setOpen(m_hiddenPanes->m_inspector);
		m_layers.setOpen(m_hiddenPanes->m_layers);
		m_colour.setOpen(m_hiddenPanes->m_colour);
		m_state.m_focus = m_hiddenPanes->m_focus;
		m_hiddenPanes.reset();
	}
	else
	{
		m_ctx.m_message = "no panes to show";
	}
}

bool Viewer::event(Event e)
{
	if(m_goto && !e.is_mouse())
	{
		gotoEvent(e);
		return true;
	}
	ImageInfoPtr info = m_ctx.m_svc.info(m_state.currentFramePath());
	Focus& focus = m_state.m_focus;
	if((focus == Focus::META && !m_meta.isOpen()) ||
	   (focus == Focus::FILES && !m_files.isOpen()) ||
	   (focus == Focus::INSPECT && !m_inspector.isOpen()) ||
	   (focus == Focus::LAYERS && !m_layers.isOpen()) ||
	   (focus == Focus::COLOUR && !m_colour.isOpen()))
	{
		focus = Focus::IMAGE;
	}
	if(focus != Focus::META)
	{
		m_meta.stopTyping(); // clicked away mid-filter: keys are keys again
	}
	// Letter aliases for the pane numbers (RV-ish): m metadata, o / x
	// files, i inspector, / layers. Not while typing, nor `x` in the
	// inspector, where it clears the pick, nor `/` in the metadata, where
	// it filters.
	if(!typing() && e.is_character())
	{
		static const std::pair<const char*, const char*> ALIASES[] =
		    {{"m", "2"}, {"o", "3"}, {"x", "3"}, {"i", "4"}, {"/", "5"}};
		for(const auto& [letter, number] : ALIASES)
		{
			if(e == Event::Character(letter) &&
			   !(focus == Focus::INSPECT && *letter == 'x') &&
			   !(focus == Focus::META && *letter == '/'))
			{
				e = Event::Character(number);
				break;
			}
		}
	}
	if((focus == Focus::META && m_meta.event(e, info)) ||
	   (focus == Focus::FILES && m_files.event(e)) ||
	   (focus == Focus::INSPECT && m_inspector.event(e)) ||
	   (focus == Focus::LAYERS && m_layers.event(e, info)) ||
	   (focus == Focus::COLOUR &&
	    m_colour.event(e,
	                   m_state.currentFramePath(),
	                   sourceKey(m_state.m_sources[m_state.m_current]))))
	{
		return true;
	}
	if(e == Event::CtrlL)
	{
		if(m_meta.isOpen() && focus == Focus::IMAGE)
		{
			focus = Focus::META; // into the metadata column
		}
		else
		{
			m_ctx.tmuxSelectPane('R');
		}
		return true;
	}
	if(e == Event::CtrlH)
	{
		m_ctx.tmuxSelectPane('L');
		return true;
	}
	if(e == Event::CtrlK)
	{
		m_ctx.tmuxSelectPane('U');
		return true;
	}
	return e.is_mouse() ? mouseEvent(e, info) : keyEvent(e, info);
}

bool Viewer::mouseEvent(Event e, const ImageInfoPtr& info)
{
	auto m = e.mouse();
	// Drag a side column's divider (the separator between it and the image)
	// to resize it, as the browser's columns do.
	if(m.button == Mouse::Left && m.motion == Mouse::Pressed)
	{
		auto onDivider = [&](int x, const Box& col)
		{
			return std::abs(m.x - x) <= 1 && m.y >= col.y_min &&
			       m.y <= col.y_max;
		};
		// Where the widths put them (a drag moves them before the next draw).
		const int right = Terminal::Size().dimx - coveredRight();
		m_dragDivider =
		    leftPanelOpen() && onDivider(coveredLeft() - 1, m_leftBox) ? 1
		    : rightPanelOpen() && onDivider(right, m_sideBox)          ? 2
		                                                               : 0;
		if(m_dragDivider)
		{
			return true;
		}
	}
	if(m_dragDivider)
	{
		if(m.motion == Mouse::Released)
		{
			m_dragDivider = 0;
		}
		else if(m.motion == Mouse::Moved)
		{
			// The divider follows the mouse: the left column ends just
			// before it, the right one starts just after.
			const double w = std::max(1, Terminal::Size().dimx);
			if(m_dragDivider == 1)
			{
				m_state.m_leftFrac = m.x / w;
			}
			else
			{
				m_state.m_rightFrac = (w - 1 - m.x) / w;
			}
		}
		return true;
	}
	// Click or drag on the playbar to scrub (playback pauses); the wheel
	// over it steps a frame.
	const bool onPlaybar =
	    m_state.frameCount() > 1 && m_playbar.contains(m.x, m.y);
	if(m_scrubbing ||
	   (onPlaybar && m.button == Mouse::Left && m.motion == Mouse::Pressed))
	{
		if(m.motion == Mouse::Released)
		{
			m_scrubbing = false;
			return true;
		}
		if(!m_scrubbing && m_player.playing())
		{
			togglePlay();
		}
		m_scrubbing = true;
		setFrame(m_playbar.frameAt(m.x));
		return true;
	}
	if(onPlaybar &&
	   (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown))
	{
		const FrameRange range = m_state.playRange();
		setFrame(m.button == Mouse::WheelDown ? range.next(m_state.m_frame)
		                                      : range.prev(m_state.m_frame));
		return true;
	}
	// Drag the files pane's title up or down: it sits at the bottom, so the
	// title follows the mouse and the list fills the rows below it.
	if(m_files.isOpen() && m.button == Mouse::Left &&
	   m.motion == Mouse::Pressed && m.y == m_files.titleRow() &&
	   m.x < coveredLeft() - 1)
	{
		m_dragFiles = true;
		return true;
	}
	if(m_dragFiles)
	{
		if(m.motion == Mouse::Released)
		{
			m_dragFiles = false;
		}
		else if(m.motion == Mouse::Moved)
		{
			// Leave each pane above it a title and a row (and a separator).
			const int above = m_layers.isOpen() + m_colour.isOpen();
			const int most = m_leftBox.y_max - m_leftBox.y_min + 1 - 2 -
			                 (above ? 3 * above : 0);
			m_files.setRows(std::min(most, m_files.bottomRow() - 1 - m.y));
		}
		return true;
	}
	// Drag over the metadata rows: select them (as `v`), for y / Y. Shift
	// +click rarely arrives (terminals keep it for their own selection).
	if(m_dragMeta)
	{
		if(m.motion == Mouse::Released)
		{
			m_dragMeta = false;
		}
		else if(m.motion == Mouse::Moved)
		{
			m_meta.dragTo(m.y);
		}
		return true;
	}
	// Drag on the image with the left button to scrub the timeline, at the
	// playbar's pace (a bar's width of dragging goes the whole sequence);
	// playback pauses. It stops at the ends rather than wrapping round.
	if(m_scrubX)
	{
		if(m.motion == Mouse::Released)
		{
			m_scrubX.reset();
			m_scrubMoved = false; // full resolution again
		}
		else if(m.motion == Mouse::Moved && m.x != *m_scrubX)
		{
			if(m_player.playing())
			{
				togglePlay();
			}
			const int n = m_state.frameCount();
			m_scrubCarry += static_cast<double>(m.x - *m_scrubX) * n /
			                std::max(1, m_playbar.trackWidth());
			const int step = static_cast<int>(m_scrubCarry); // toward 0
			m_scrubCarry -= step;
			setFrame(std::clamp(m_state.m_frame + step, 0, n - 1));
			m_scrubX = m.x;
			m_scrubMoved = true;
		}
		return true;
	}
	// Drag the image (or the sheet) with the right or middle button to pan,
	// as hjkl do: it follows the mouse.
	if(m_dragPan)
	{
		if(m.motion == Mouse::Released)
		{
			m_dragPan.reset();
		}
		else if(m.motion == Mouse::Moved)
		{
			const int dx = m.x - m_dragPan->first, dy = m.y - m_dragPan->second;
			if(dx || dy)
			{
				if(m_tile)
				{
					panSheet(-dx, -dy, info);
				}
				else
				{
					pan(-dx, -dy);
				}
			}
			m_dragPan = std::pair(m.x, m.y);
		}
		return true;
	}
	const bool overInfo = m_meta.isOpen() && m_meta.contains(m.x, m.y);
	const bool overFiles = m_files.isOpen() && m_files.contains(m.x, m.y);
	const bool overSide = (rightPanelOpen() && inside(m_sideBox, m.x, m.y)) ||
	                      (leftPanelOpen() && inside(m_leftBox, m.x, m.y));
	const bool wheel =
	    m.button == Mouse::WheelUp || m.button == Mouse::WheelDown;
	if(wheel && overInfo)
	{
		m_meta.scroll(m.button == Mouse::WheelUp ? -3 : 3);
		return true;
	}
	if(wheel)
	{
		if(overSide)
		{
			return true;
		}
		double f = m.button == Mouse::WheelUp ? 1.25 : 1.0 / 1.25;
		if(m_tile)
		{
			zoomSheet(f,
			          info,
			          std::pair(m.x - m_sheetBox.x_min + 0.5,
			                    m.y - m_sheetBox.y_min + 0.5));
			return true;
		}
		zoomBy(f, m_viewSlot->imageCoordAt(m.x, m.y));
		return true;
	}
	// Click selects (a tile, or which pane has focus) and, dragged on a
	// sequence, scrubs; Ctrl+click picks the colour; right / middle drag
	// pans.
	const bool pressed = m.motion == Mouse::Pressed;
	const bool pick = pressed && m.button == Mouse::Left && m.control;
	if(pressed && !overSide &&
	   (m.button == Mouse::Right || m.button == Mouse::Middle))
	{
		m_dragPan = std::pair(m.x, m.y);
		m_state.m_focus = Focus::IMAGE;
		return true;
	}
	if(pressed && !overSide && m.button == Mouse::Left && !m.control &&
	   m_state.frameCount() > 1)
	{
		m_scrubX = m.x;
		m_scrubCarry = 0;
	}
	if(!pressed || (m.button != Mouse::Left && !pick))
	{
		return m_inspector.isOpen(); // redraw for the readout on mouse move
	}
	if(overSide)
	{
		if(overFiles)
		{
			m_files.click(m.y);
		}
		else if(overInfo)
		{
			m_meta.click(m.y, m.shift || m.control);
			m_dragMeta = true;
		}
		else if(m_inspector.isOpen() && m_inspector.contains(m.x, m.y))
		{
			m_state.m_focus = Focus::INSPECT;
		}
		else if(m_colour.isOpen() && m_colour.contains(m.x, m.y))
		{
			m_state.m_focus = Focus::COLOUR;
		}
		else if(m_layers.isOpen() && m_layers.contains(m.x, m.y))
		{
			m_layers.click(m.y, info);
		}
		return true;
	}
	m_state.m_focus = Focus::IMAGE;
	// A click on a tile selects it (Enter opens it).
	if(m_tile && !pick)
	{
		for(const auto& [i, t] : m_tiles)
		{
			if(inside(t.m_slot->box(), m.x, m.y))
			{
				selectTile(i, info);
				break;
			}
		}
	}
	Sample s = pick ? sampleAt(m.x, m.y) : Sample{};
	if(s.m_state == Sample::State::OK)
	{
		m_ctx.m_message = "picked " + sampleCoord(s) + "  " + s.m_layer;
		m_state.m_picked = std::move(s);
	}
	return true;
}

bool Viewer::keyEvent(const Event& e, const ImageInfoPtr& info)
{
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	if(m_inspector.isOpen() && e == Event::Escape)
	{
		// Esc backs out of the innermost mode first.
		m_inspector.setOpen(false);
	}
	else if(m_tile && e == Event::Return)
	{
		openSelectedTile(info);
	}
	else if(ch("q") || e == Event::Escape || e == Event::Backspace)
	{
		if(m_tile)
		{
			m_tile = false;
		}
		else
		{
			close();
		}
	}
	else if(ch("]") || ch("[") || ch("n") || ch("p") || ch("N"))
	{
		if(ch("]") || ch("["))
		{
			cycleLayer(info, ch("]") ? 1 : -1);
		}
		else if(m_state.grouped())
		{
			// A diff session: pair by pair, each tiled (a lone image not).
			m_state.m_current = m_state.stepGroup(ch("n") ? 1 : -1);
			m_tile = tilesBySource();
		}
		else
		{
			const int n = static_cast<int>(m_state.m_sources.size());
			m_state.m_current =
			    (m_state.m_current + (ch("n") ? 1 : -1) + n) % n;
		}
		if(m_tile && m_sheetView.m_zoom > 1.0)
		{
			centreSheetOn(selectedTile(info)); // keep the selection in view
		}
	}
	else if(ch("c"))
	{
		m_state.m_disp.m_mode = ChannelMode::COLOR;
	}
	else if(ch("r"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::RED
		                            ? ChannelMode::COLOR
		                            : ChannelMode::RED;
	}
	else if(ch("g"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::GREEN
		                            ? ChannelMode::COLOR
		                            : ChannelMode::GREEN;
	}
	else if(ch("b"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::BLUE
		                            ? ChannelMode::COLOR
		                            : ChannelMode::BLUE;
	}
	else if(ch("a"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::ALPHA
		                            ? ChannelMode::COLOR
		                            : ChannelMode::ALPHA;
	}
	else if(ch("u"))
	{
		m_state.m_disp.m_mode = m_state.m_disp.m_mode == ChannelMode::LUMA
		                            ? ChannelMode::COLOR
		                            : ChannelMode::LUMA;
	}
	else if(ch("e"))
	{
		m_state.m_disp.m_exposure += 0.5f;
	}
	else if(ch("E"))
	{
		m_state.m_disp.m_exposure -= 0.5f;
	}
	else if(ch("y"))
	{
		m_state.m_disp.m_gamma += 0.1f;
	}
	else if(ch("Y"))
	{
		m_state.m_disp.m_gamma = std::max(0.1f, m_state.m_disp.m_gamma - 0.1f);
	}
	else if(ch("s"))
	{
		m_state.m_disp.m_srgb = !m_state.m_disp.m_srgb;
	}
	else if(ch("w") && m_tile)
	{
		// Tiles show the frame and the selected tile's dashed highlight:
		// selection → frame + selection → frame → off → selection.
		const bool frame = m_state.m_disp.m_outlines != Outlines::NONE;
		const bool sel = m_tileSelection;
		if(sel && !frame)
		{
			m_state.m_disp.m_outlines = Outlines::FRAME_AND_DATA;
		}
		else if(sel)
		{
			m_tileSelection = false;
		}
		else if(frame)
		{
			m_state.m_disp.m_outlines = Outlines::NONE;
		}
		else
		{
			m_tileSelection = true;
		}
		const bool f = m_state.m_disp.m_outlines != Outlines::NONE;
		m_ctx.m_message = f && m_tileSelection ? "outlines: windows + selection"
		                  : f               ? "outlines: display + data window"
		                  : m_tileSelection ? "outlines: selection"
		                                    : "outlines: off";
	}
	else if(ch("w"))
	{
		// Window outlines: off (default) → frame + data → frame only → off.
		m_state.m_disp.m_outlines =
		    m_state.m_disp.m_outlines == Outlines::FRAME_AND_DATA
		        ? Outlines::FRAME
		    : m_state.m_disp.m_outlines == Outlines::FRAME
		        ? Outlines::NONE
		        : Outlines::FRAME_AND_DATA;
		m_ctx.m_message =
		    m_state.m_disp.m_outlines == Outlines::FRAME_AND_DATA
		        ? "outlines: display window + data window (dashed)"
		    : m_state.m_disp.m_outlines == Outlines::FRAME
		        ? "outlines: display window only"
		        : "outlines: off";
	}
	else if(ch("0"))
	{
		m_state.m_disp = DisplayParams{};
	}
	else if(ch("!"))
	{
		auto& on = m_state.m_disp.m_showNonFinite;
		on = !on;
		m_ctx.m_message = on ? "NaN / inf shown: NaN magenta, inf cyan"
		                     : "NaN / inf shown as black";
	}
	else if(ch("+") || ch("=") || ch("-") || ch("_"))
	{
		const double f = ch("+") || ch("=") ? 1.25 : 1.0 / 1.25;
		if(m_tile)
		{
			zoomSheet(f, info);
		}
		else
		{
			zoomBy(f);
		}
	}
	else if(ch("f"))
	{
		if(m_tile)
		{
			m_sheetView = fitSheetView(m_sheet, m_sheetArea);
		}
		else
		{
			fitView(info);
		}
	}
	else if(ch("z"))
	{
		if(m_tile)
		{
			sheetOneToOne(info);
		}
		else
		{
			zoomBy(1.0);
			m_state.m_view.m_zoom = 1.0;
		}
	}
	else if(auto step = panStep(e))
	{
		if(m_tile)
		{
			panSheet(step->first, step->second, info);
		}
		else
		{
			pan(step->first, step->second);
		}
	}
	else if(ch(" "))
	{
		togglePlay();
	}
	else if(ch("."))
	{
		setFrame(m_state.playRange().next(m_state.m_frame));
	}
	else if(ch(","))
	{
		setFrame(m_state.playRange().prev(m_state.m_frame));
	}
	else if(ch("<"))
	{
		setFrame(m_state.playRange().m_first);
	}
	else if(ch(">"))
	{
		setFrame(m_state.playRange().m_last);
	}
	else if(ch("I") || ch("O"))
	{
		setInOut(ch("I"));
	}
	else if(ch(":"))
	{
		openGoto();
	}
	else if(ch("P"))
	{
		m_playbackCap = !m_playbackCap;
		// Tiles on this machine keep the pixels: only the decode is softer.
		m_ctx.m_message =
		    !m_playbackCap ? std::string("playback: full res")
		    : cappedBudget() < TOTAL_PIXEL_BUDGET
		        ? fmt::format("playback: capped at {:g} Mpx (full {:g})",
		                      cappedBudget() / 1e6,
		                      TOTAL_PIXEL_BUDGET / 1e6)
		        : std::string("playback: capped (softer decode)");
	}
	else if(ch("F"))
	{
		const double cur =
		    m_player.targetFps(info ? info->m_fps : std::nullopt);
		double next = FPS_CHOICES[0];
		for(double c : FPS_CHOICES)
		{
			if(c > cur + 1e-3)
			{
				next = c;
				break;
			}
		}
		m_player.setFps(next);
	}
	else if(ch("t"))
	{
		m_tile = !m_tile;
		m_sheetKey.clear(); // a fresh sheet starts fitted
	}
	else if(ch("G"))
	{
		toggleFillGaps();
	}
	else if(ch("T"))
	{
		m_names = !m_names;
		m_ctx.m_message = m_names ? "names: on" : "names: off";
	}
	else if(ch("1"))
	{
		m_state.m_focus = Focus::IMAGE;
	}
	// Pane numbers: open + focus (pressed again while focused, the pane's
	// own handler closes it).
	else if(ch("2"))
	{
		m_meta.setOpen(true);
		m_state.m_focus = Focus::META;
	}
	else if(ch("4"))
	{
		m_inspector.setOpen(true);
		m_state.m_focus = Focus::INSPECT;
	}
	else if(ch("5"))
	{
		m_layers.show(info);
	}
	else if(ch("3"))
	{
		m_files.show();
	}
	else if(ch("6"))
	{
		m_colour.show();
	}
	else if(e == Event::Tab)
	{
		togglePanes();
	}
	else if(ch("A"))
	{
		m_ctx.m_ann.m_visible = !m_ctx.m_ann.m_visible;
		m_ctx.m_message =
		    m_ctx.m_ann.m_visible ? "annotations: on" : "annotations: off";
	}
	else if(ch("}"))
	{
		m_meta.scroll(5);
	}
	else if(ch("{"))
	{
		m_meta.scroll(-5);
	}
	else if(m_meta.isOpen() && e == Event::PageDown)
	{
		m_meta.scroll(20);
	}
	else if(m_meta.isOpen() && e == Event::PageUp)
	{
		m_meta.scroll(-20);
	}
	else
	{
		return false;
	}
	return true;
}

// --- rendering ---

Element Viewer::renderHud(const ImageInfoPtr& info)
{
	const Source& src = m_state.m_sources[m_state.m_current];
	std::string name =
	    src.m_diffA ? "diff " + src.m_entry.m_name : src.m_entry.m_name;
	if(m_state.grouped())
	{
		name = fmt::format("[{}/{}] {}",
		                   m_state.groupIndex(m_state.m_current) + 1,
		                   m_state.groupCount(),
		                   name);
	}
	else if(m_state.m_sources.size() > 1)
	{
		name = fmt::format("[{}/{}] {}",
		                   m_state.m_current + 1,
		                   m_state.m_sources.size(),
		                   name);
	}
	Elements parts;
	// lazygit-style pane number: `1` focuses the image.
	const bool imageFocused = m_state.m_focus == Focus::IMAGE;
	parts.push_back(text(" [1]") |
	                (imageFocused ? color(Color::Green) | bold : dim));
	parts.push_back(text(" " + name + " ") | bold);
	if(info)
	{
		int li = std::max(0, info->findLayer(m_state.m_layerLabel));
		parts.push_back(text(fmt::format(" {} ({}/{}) ",
		                                 info->m_layers[li].label(),
		                                 li + 1,
		                                 info->m_layers.size())) |
		                color(Color::Cyan) | bold);
	}
	// Channels as coloured letters: lit = shown, dim = hidden.
	const ChannelMode mode = m_state.m_disp.m_mode;
	auto letter = [](const char* l, Color c, bool lit)
	{ return text(l) | color(c) | (lit ? bold : dim); };
	const bool rgb = mode == ChannelMode::COLOR;
	parts.push_back(text(" "));
	parts.push_back(letter("R", Color::Red, rgb || mode == ChannelMode::RED));
	parts.push_back(
	    letter("G", Color::Green, rgb || mode == ChannelMode::GREEN));
	parts.push_back(letter("B", Color::Blue, rgb || mode == ChannelMode::BLUE));
	parts.push_back(letter("A", Color::White, mode == ChannelMode::ALPHA));
	if(mode == ChannelMode::LUMA)
	{
		parts.push_back(text(" luma") | bold);
	}
	parts.push_back(text(" "));
	// Display settings only once they differ from the defaults.
	const DisplayParams defaults;
	std::string disp;
	if(m_state.m_disp.m_exposure != defaults.m_exposure)
	{
		disp += fmt::format(" {:+.1f}ev", m_state.m_disp.m_exposure);
	}
	if(m_state.m_disp.m_gamma != defaults.m_gamma)
	{
		disp += fmt::format(" γ{:.1f}", m_state.m_disp.m_gamma);
	}
	if(m_ctx.m_colour.active() && m_state.m_disp.m_srgb)
	{
		disp += " " + m_ctx.m_colour.view(); // the OCIO view in use
	}
	else if(m_state.m_disp.m_srgb != defaults.m_srgb)
	{
		disp += m_state.m_disp.m_srgb ? " sRGB" : " raw";
	}
	if(m_state.m_disp.m_showNonFinite)
	{
		disp += " !NaN";
	}
	if(!disp.empty())
	{
		parts.push_back(text(disp + " ") | color(Color::Yellow));
	}
	if(info && !info->m_parts.empty())
	{
		// The shown layer's part (multi-part EXRs can differ); the data
		// window too when it isn't the frame (overscan, a crop).
		const int li = std::max(0, info->findLayer(m_state.m_layerLabel));
		const int pi =
		    info->m_layers.empty()
		        ? 0
		        : std::clamp(info->m_layers[li].m_part,
		                     0,
		                     static_cast<int>(info->m_parts.size()) - 1);
		const PartInfo& part = info->m_parts[pi];
		const Box2i& d = part.m_displayWindow;
		const Box2i& w = part.m_dataWindow;
		std::string res = fmt::format(" {}×{}", d.width(), d.height());
		if(w != d)
		{
			res += fmt::format(" (data {}×{})", w.width(), w.height());
		}
		parts.push_back(text(res) | dim);
	}
	std::string zoom =
	    m_state.m_view.m_fit
	        ? " 🔍 fit "
	        : fmt::format(" 🔍 {:.0f}% ", m_state.m_view.m_zoom * 100);
	if(m_tile)
	{
		// The sheet's zoom, against fitted.
		zoom = m_sheetView.m_zoom == 1.0
		           ? " 🔍 fit "
		           : fmt::format(" 🔍 ×{:.2g} ", m_sheetView.m_zoom);
	}
	parts.push_back(text(zoom) | dim);
	if(m_state.m_picked)
	{
		parts.push_back(text(" ██ ") |
		                color(Color::RGB(m_state.m_picked->m_r,
		                                 m_state.m_picked->m_g,
		                                 m_state.m_picked->m_b)));
		parts.push_back(text(fmt::format("[{}, {}] ",
		                                 m_state.m_picked->nukeX(),
		                                 m_state.m_picked->nukeY())) |
		                dim);
		parts.push_back(rgbaValues(*m_state.m_picked));
		parts.push_back(text(" "));
	}
	parts.push_back(filler());
	// The layer has broken pixels: say so, inspector open or not.
	if(const LayerImagePtr shown = describedImage(info))
	{
		if(const std::string sum = nonFiniteSummary(*shown); !sum.empty())
		{
			parts.push_back(text(" " + sum + " ") | nonFiniteStyle());
			parts.push_back(text(" "));
		}
	}
	if(m_tile)
	{
		parts.push_back(text(" TILE ") | inverted);
	}
	if(m_inspector.isOpen())
	{
		parts.push_back(text(" INSPECT · Esc ") | inverted |
		                color(Color::Yellow));
	}
	return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
}

LayerImagePtr Viewer::describedImage(const ImageInfoPtr& info) const
{
	if(!m_tile)
	{
		return m_viewSlot->shown();
	}
	const auto it = m_tiles.find(selectedTile(info));
	return it != m_tiles.end() ? it->second.m_slot->shown() : nullptr;
}

Sample Viewer::sampleAt(int cellX, int cellY)
{
	Sample out;
	// Which image is under the cell: a tile, or the main view.
	const ImageSlot* slot = nullptr;
	fs::path path;
	std::string layer;
	int source = m_state.m_current;
	if(m_tile)
	{
		for(const auto& [i, t] : m_tiles)
		{
			if(i < static_cast<int>(m_tileRefs.size()) &&
			   t.m_slot->imageCoordAt(cellX, cellY))
			{
				slot = t.m_slot.get();
				path = m_tileRefs[i].m_path;
				layer = m_tileRefs[i].m_layer;
				source = m_tileRefs[i].m_source;
				break;
			}
		}
	}
	else
	{
		slot = m_viewSlot.get();
		path = m_state.currentFramePath();
		layer = m_state.m_layerLabel;
	}
	auto coord = slot ? slot->imageCoordAt(cellX, cellY) : std::nullopt;
	if(!coord)
	{
		return out;
	}
	out.m_state = Sample::State::LOADING;
	if(const ImageInfoPtr i = m_ctx.m_svc.info(path))
	{
		out.m_frame = i->displayWindow(); // before the pixels land
	}
	out.m_path = path;
	out.m_layer = layer;
	out.m_x = static_cast<int>(std::floor(coord->first));
	out.m_y = static_cast<int>(std::floor(coord->second));
	// Exact values come from full resolution, fetched for the readout only;
	// until it lands, read whatever resolution is on screen (marked ≈).
	const Source& from = m_state.m_sources[source];
	LayerImagePtr full =
	    from.m_diffA ? m_ctx.m_svc.diffLayer(from.diffFrame(m_state.m_frame),
	                                         path,
	                                         layer,
	                                         1)
	                 : m_ctx.m_svc.layer(path, layer, 1);
	LayerImagePtr img = full ? full
	                    : from.m_diffA
	                        ? slot->shown()
	                        : m_ctx.m_svc.layerBestEffort(path, layer, 1);
	if(!img)
	{
		return out;
	}
	out.m_exact = full != nullptr;
	out.m_frame = img->m_displayWindow;
	bool inside = false;
	for(size_t c = 0; c < img->m_channelNames.size(); ++c)
	{
		auto v = img->at(static_cast<int>(c), out.m_x, out.m_y);
		inside |= v.has_value();
		out.m_values.emplace_back(img->m_channelNames[c], v.value_or(0.0f));
	}
	if(!inside)
	{
		out.m_state = Sample::State::OUTSIDE;
		return out;
	}
	ChannelMap cm = mapChannels(img->m_channelNames);
	auto get = [&](int idx)
	{ return idx >= 0 ? img->at(idx, out.m_x, out.m_y).value_or(0.0f) : 0.0f; };
	float lr = get(cm.m_r), lg = get(cm.m_g), lb = get(cm.m_b);
	out.m_rgba[0] = lr;
	out.m_rgba[1] = lg;
	out.m_rgba[2] = lb;
	out.m_hasAlpha = cm.m_a >= 0;
	out.m_rgba[3] = out.m_hasAlpha ? get(cm.m_a) : 1.0f;
	const auto rgb8 = displayRgb8(lr, lg, lb, displayFor(source, path));
	out.m_r = rgb8[0];
	out.m_g = rgb8[1];
	out.m_b = rgb8[2];
	out.m_luma = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
	out.m_state = Sample::State::OK;
	return out;
}

Element Viewer::renderTiles(const ImageInfoPtr& info, int width)
{
	const bool bySource = tilesBySource();
	const auto sheetSources = m_state.sheetSources();
	const int n = bySource
	                  ? static_cast<int>(sheetSources.size())
	                  : (info ? static_cast<int>(info->m_layers.size()) : 0);
	if(n == 0)
	{
		m_tiles.clear();
		return text("");
	}

	// Contact sheet: one tile per source (or per layer), each sized to the
	// image's aspect so neighbours nearly touch, filled left to right then
	// down. No captions unless `T` asks for them: the HUD names the selected
	// one. The sheet zooms and pans as one image; only the tiles on screen
	// are drawn.
	const auto dims = Terminal::Size();
	const SheetArea area{std::max(1, width),
	                     std::max(1, dims.dimy - 2)}; // minus HUD + status
	const double pxX = m_viewSlot->pxPerCellX(), pxY = m_viewSlot->pxPerCellY();
	// What each tile shows, and its frame (display window: tiles frame that
	// only) size.
	m_tileRefs.resize(n);
	std::vector<ImageInfoPtr> infos(n);
	std::vector<std::pair<double, double>> frames(n);
	std::string key =
	    bySource ? std::string("sources")
	             : "layers " + sourceKey(m_state.m_sources[m_state.m_current]);
	for(int i = 0; i < n; ++i)
	{
		if(bySource)
		{
			const Source& src = m_state.m_sources[sheetSources[i]];
			m_tileRefs[i] = {src.frame(m_state.m_frame),
			                 m_state.m_layerLabel,
			                 sheetSources[i]};
			infos[i] = m_ctx.m_svc.info(m_tileRefs[i].m_path);
			key += "|" + sourceKey(src);
		}
		else
		{
			m_tileRefs[i] = {m_state.currentFramePath(),
			                 info->m_layers[i].label(),
			                 m_state.m_current};
			infos[i] = info;
			key += "|" + m_tileRefs[i].m_layer;
		}
		const Box2i b = (infos[i] ? infos[i] : info)->displayWindow();
		frames[i] = {std::max(1, b.width()), std::max(1, b.height())};
	}
	m_sheet =
	    layoutSheet(frames, area.m_w, area.m_h, pxX, pxY, MAX_VISIBLE_TILES);
	m_sheetArea = area;
	if(key != m_sheetKey)
	{
		m_sheetKey = key; // other tiles: start over, fitted
		m_sheetView = fitSheetView(m_sheet, area);
	}
	else
	{
		clampSheetView(m_sheetView, m_sheet, area); // resized, headers in
	}

	// The tiles on screen keep (or get) a slot; the rest give theirs back.
	std::vector<std::pair<int, TilePlacement>> visible;
	for(int i = 0; i < n; ++i)
	{
		if(auto p = placeTile(m_sheet, m_sheetView, area, i))
		{
			visible.emplace_back(i, *p);
		}
	}
	std::map<int, Tile> kept;
	for(const auto& [i, p] : visible)
	{
		auto it = m_tiles.find(i);
		kept.emplace(i,
		             it != m_tiles.end() ? std::move(it->second)
		                                 : Tile{newSlot(m_ctx)});
	}
	m_tiles = std::move(kept);

	const int sel = selectedTile(info);
	Elements children;
	std::vector<Box> boxes;
	for(const auto& [i, p] : visible)
	{
		Tile& t = m_tiles.at(i);
		ImageSlot& slot = *t.m_slot;
		slot.setMaxPixels(pixelCap());
		slot.setTerminalCache(m_player.playing() || scrubbing());
		const TileRef& ref = m_tileRefs[i];
		const ImageInfoPtr& ti = infos[i];
		// All of it on a fitted sheet: fit, as ever. Otherwise the zoom
		// that fits the frame in the whole tile, centred on where the
		// visible part's middle falls.
		ViewParams view;
		view.m_fitFrame = true;
		t.m_zoom = 0;
		const bool whole = m_sheetView.m_zoom <= 1.0 &&
		                   p.m_boxX0 == std::lround(p.m_x0) &&
		                   p.m_boxX1 == std::lround(p.m_x1) &&
		                   p.m_boxY0 == std::lround(p.m_y0) &&
		                   p.m_boxY1 == std::lround(p.m_y1);
		if(!whole && ti)
		{
			const Box2i dw = ti->displayWindow();
			const double k =
			    std::min((p.m_x1 - p.m_x0) * pxX / std::max(1, dw.width()),
			             (p.m_y1 - p.m_y0) * pxY / std::max(1, dw.height()));
			const double dx =
			    (p.m_boxX0 + p.m_boxX1 - p.m_x0 - p.m_x1) / 2.0 * pxX / k;
			const double dy =
			    (p.m_boxY0 + p.m_boxY1 - p.m_y0 - p.m_y1) / 2.0 * pxY / k;
			view.m_fit = false;
			view.m_zoom = k;
			view.m_centerX = dw.m_x0 + dw.width() / 2.0 + dx;
			view.m_centerY = dw.m_y0 + dw.height() / 2.0 + dy;
			t.m_zoom = k;
		}
		t.m_view = view;
		LayerImagePtr li = ti ? layerOf(ref.m_source,
		                                ref.m_path,
		                                ref.m_layer,
		                                reduceFor(*ti, slot))
		                      : nullptr;
		const LayerImagePtr decoded = li;
		const auto err = li ? std::nullopt : errorOf(ref.m_source, ref.m_path);
		if(!li && !err)
		{
			// Still decoding: keep the tile's last picture up, as the
			// single view does (see render()).
			li = slot.shown();
		}
		Element img = err ? text("error") | color(Color::Red) | center
		                  : text("…") | dim | center;
		if(li)
		{
			// A contact sheet shows the frame outline at most.
			const int src = ref.m_source;
			DisplayParams disp = displayFor(src, ref.m_path);
			if(disp.m_outlines == Outlines::FRAME_AND_DATA)
			{
				disp.m_outlines = Outlines::FRAME;
			}
			disp.m_selected = i == sel && n > 1 && m_tileSelection;
			if(m_state.m_picked && m_state.m_picked->m_path == ref.m_path &&
			   m_state.m_picked->m_layer == ref.m_layer)
			{
				disp.m_marker = pickedMarker();
			}
			const KeyLookup lookup =
			    keyLookup(src, ref.m_path, ti, ref.m_layer);
			OverlayText over =
			    overlayText({m_state.sourceAnnotations(m_ctx.m_ann, src)},
			                lookup);
			if(m_names)
			{
				// `T`: what the tile is, under any annotations there.
				const std::string name =
				    bySource ? m_state.m_sources[src].m_entry.m_name
				    : ref.m_layer.empty() ? std::string("rgba")
				                          : ref.m_layer;
				over[static_cast<int>(Slot::BL)].push_back({{name}});
			}
			addLabels(over, src, lookup, decoded);
			markIncomplete(over, li);
			img = slot.element(li, view, disp, over);
		}
		children.push_back(img);
		boxes.push_back(
		    Box{p.m_boxX0, p.m_boxX1 - 1, p.m_boxY0, p.m_boxY1 - 1});
	}
	Element grid = placeAt(std::move(children), std::move(boxes)) |
	               size(WIDTH, EQUAL, area.m_w) |
	               size(HEIGHT, EQUAL, area.m_h) | reflect(m_sheetBox);

	// Global lines once over the whole sheet (the union of the tile frames
	// on screen), resolved against the selected tile.
	const TileRef& selRef = m_tileRefs[std::clamp(sel, 0, n - 1)];
	OverlayText sheet = overlayText({&m_ctx.m_ann.global()},
	                                keyLookup(m_state.m_current,
	                                          selRef.m_path,
	                                          m_ctx.m_svc.info(selRef.m_path),
	                                          selRef.m_layer));
	if(!overlayEmpty(sheet))
	{
		grid = drawAfter(
		    grid,
		    [this, sheet = std::move(sheet)](Screen& screen)
		    {
			    std::optional<ImageSlot::CellRect> u;
			    for(const auto& [i, t] : m_tiles)
			    {
				    auto f = t.m_slot->frameCells();
				    if(!f)
				    {
					    continue;
				    }
				    if(!u)
				    {
					    u = f;
					    continue;
				    }
				    const int x1 = std::max(u->m_x + u->m_w, f->m_x + f->m_w);
				    const int y1 = std::max(u->m_y + u->m_h, f->m_y + f->m_h);
				    u->m_x = std::min(u->m_x, f->m_x);
				    u->m_y = std::min(u->m_y, f->m_y);
				    u->m_w = x1 - u->m_x;
				    u->m_h = y1 - u->m_y;
			    }
			    if(!u)
			    {
				    return;
			    }
			    paintOverlay(
			        screen,
			        m_sheetBox,
			        layoutOverlay(u->m_x, u->m_y, u->m_w, u->m_h, sheet),
			        [this](int x,
			               int y) -> std::optional<std::array<uint8_t, 3>>
			        {
				        for(const auto& [i, t] : m_tiles)
				        {
					        if(auto c = t.m_slot->cellColor(x, y))
					        {
						        return c;
					        }
				        }
				        return std::nullopt;
			        });
		    });
	}
	return grid;
}

Element Viewer::render()
{
	if(m_state.m_sources.empty())
	{
		return text("");
	}
	// Each draw may make fillTerminal apply (the first one drawn, a new
	// view): start it after this draw; its wake keeps it going from there.
	if(!m_fillWanted)
	{
		m_ctx.m_post([this] { fillTerminal(); });
	}
	fs::path path = m_state.currentFramePath();
	ImageInfoPtr info = m_ctx.m_svc.info(path);
	if(info)
	{
		if(info->findLayer(m_state.m_layerLabel) < 0)
		{
			m_state.m_layerLabel = info->m_layers.front().label();
		}
		if(m_player.fps() <= 0 && info->m_fps)
		{
			m_player.setFps(*info->m_fps);
		}
	}

	Element main;
	if(!info)
	{
		auto err = m_ctx.m_svc.error(path);
		main = (err ? paragraph("error: " + *err) | color(Color::Red)
		            : text("loading…") | dim) |
		       center | flex;
	}
	else if(m_tile)
	{
		main =
		    renderTiles(info,
		                Terminal::Size().dimx - coveredLeft() - coveredRight());
	}
	else
	{
		if(m_fitBesidePanel)
		{
			fitView(info); // follow terminal resizes and pane changes
		}
		m_viewSlot->setMaxPixels(pixelCap());
		m_viewSlot->setTerminalCache(m_player.playing() || scrubbing());
		int reduce = reduceFor(*info, *m_viewSlot);
		LayerImagePtr img =
		    layerOf(m_state.m_current, path, m_state.m_layerLabel, reduce);
		const LayerImagePtr decoded = img;
		const auto err = img ? std::nullopt : errorOf(m_state.m_current, path);
		if(!img && !err)
		{
			// Still decoding: keep the last picture up. Blanking it would also
			// make kitty redraw every placeholder cell once the new one is in,
			// a burst tmux may drop output over (and the picture with it).
			img = m_viewSlot->shown();
		}
		if(!m_player.playing() && m_state.frameCount() > 1)
		{
			// Smooth stepping with '.'.
			const FrameRange range = m_state.playRange();
			for(int i = 1, f = range.next(m_state.m_frame);
			    i <= 2 && i < range.count();
			    ++i, f = range.next(f))
			{
				prefetchFrame(f);
			}
		}
		// The main view rings the picked pixel whichever image it came from.
		DisplayParams disp = displayFor(m_state.m_current, path);
		disp.m_marker = pickedMarker();
		const KeyLookup lookup =
		    keyLookup(m_state.m_current, path, info, m_state.m_layerLabel);
		OverlayText over =
		    overlayText({&m_ctx.m_ann.global(),
		                 m_state.sourceAnnotations(m_ctx.m_ann,
		                                           m_state.m_current)},
		                lookup);
		if(m_names && info && !info->m_layers.empty())
		{
			// `T`: the layer on screen, resolved as the HUD does it.
			const std::string layer =
			    info->m_layers[std::max(0,
			                            info->findLayer(m_state.m_layerLabel))]
			        .label();
			over[static_cast<int>(Slot::BL)].push_back(
			    {{layer.empty() ? std::string("rgba") : layer}});
		}
		addLabels(over, m_state.m_current, lookup, decoded);
		markIncomplete(over, img);
		main = img   ? m_viewSlot->element(img, m_state.m_view, disp, over)
		       : err ? paragraph("error: " + *err) | color(Color::Red) | center
		             : text("decoding…") | dim | center;
		main = main | flex;
	}

	// Side panels are drawn *over* the single image (dbox + clear_under) so
	// opening or closing them never refits or moves the picture; tiles are
	// laid out beside them instead, so none is hidden.
	Element body = main | flex;
	Element leftCol, rightCol;
	if(leftPanelOpen())
	{
		// Layers and colour on top, files pinned to the bottom left. Files
		// made taller (its title dragged up) squeeze the panes above it.
		Elements panes;
		auto above = [&](Element el)
		{
			if(!panes.empty())
			{
				panes.push_back(separator());
			}
			panes.push_back(std::move(el) | yflex_shrink);
		};
		if(m_layers.isOpen())
		{
			above(m_layers.render(info));
		}
		if(m_colour.isOpen())
		{
			above(m_colour.render(path,
			                      sourceKey(
			                          m_state.m_sources[m_state.m_current])));
		}
		const bool anyAbove = !panes.empty();
		panes.push_back(filler());
		if(m_files.isOpen())
		{
			if(anyAbove)
			{
				panes.push_back(separator());
			}
			panes.push_back(m_files.render());
		}
		const bool leftFocused = m_state.m_focus == Focus::LAYERS ||
		                         m_state.m_focus == Focus::FILES ||
		                         m_state.m_focus == Focus::COLOUR;
		leftCol = hbox({vbox(std::move(panes)) |
		                    size(WIDTH, EQUAL, m_state.leftPanelWidth()),
		                separator() |
		                    (leftFocused ? color(Color::Green) : nothing)}) |
		          reflect(m_leftBox);
	}
	if(rightPanelOpen())
	{
		Elements side;
		auto add = [&](Element el)
		{
			if(!side.empty())
			{
				side.push_back(separator());
			}
			side.push_back(std::move(el));
		};
		if(m_inspector.isOpen())
		{
			add(m_inspector.render(info ? sampleAt(m_mouseX, m_mouseY)
			                            : Sample{},
			                       describedImage(info).get()));
		}
		if(m_meta.isOpen())
		{
			add(m_meta.render(info) | flex);
		}
		const bool rightFocused =
		    m_state.m_focus == Focus::META || m_state.m_focus == Focus::INSPECT;
		rightCol =
		    hbox({separator() | (rightFocused ? color(Color::Green) : nothing),
		          vbox(std::move(side)) |
		              size(WIDTH, EQUAL, m_state.sidePanelWidth())}) |
		    reflect(m_sideBox);
	}
	if(m_tile)
	{
		Elements row;
		if(leftCol)
		{
			row.push_back(leftCol);
		}
		row.push_back(body);
		if(rightCol)
		{
			row.push_back(rightCol);
		}
		body = hbox(std::move(row));
	}
	else if(leftCol || rightCol)
	{
		Elements over;
		if(leftCol)
		{
			over.push_back(leftCol | clear_under);
		}
		over.push_back(filler());
		if(rightCol)
		{
			over.push_back(rightCol | clear_under);
		}
		body = dbox({body, hbox(std::move(over))});
	}
	return vbox(
	    {renderHud(info),
	     body | flex,
	     m_state.frameCount() > 1 ? renderPlaybar(info) : emptyElement(),
	     m_goto                            ? renderGoto()
	     : m_files.annotations().editing() ? m_files.annotations().renderInput()
	                                       : renderStatus(info)});
}

Element Viewer::renderPlaybar(const ImageInfoPtr& info)
{
	const Source& src = m_state.m_sources[m_state.m_current];
	const int n = m_state.frameCount();
	std::vector<fs::path> frames;
	frames.reserve(n);
	for(int i = 0; i < n; ++i)
	{
		frames.push_back(src.frame(i));
	}
	return m_playbar.render(
	    {.m_frames = n,
	     .m_frame = m_state.m_frame,
	     .m_range = m_state.playRange(),
	     .m_hasIn = m_state.m_in.has_value(),
	     .m_hasOut = m_state.m_out.has_value(),
	     .m_cached = m_ctx.m_svc.cachedLayers(frames, m_state.m_layerLabel),
	     .m_stored = info ? storedFrames(*info) : std::vector<bool>{},
	     .m_storedElsewhere = storedElsewhere(),
	     .m_label = [&src](int f) { return src.frameLabel(f); },
	     .m_playing = m_player.playing(),
	     .m_fps = m_player.targetFps(info ? info->m_fps : std::nullopt),
	     .m_measured = m_player.measuredFps()},
	    Terminal::Size().dimx);
}

Element Viewer::renderGoto() const
{
	// Like the annotation input line: the prompt, the digits with a block
	// cursor, then the sequence's range and the keys while they fit.
	const auto& g = m_goto->glyphs();
	const int cur = m_goto->cursor();
	std::string before, after;
	for(int k = 0; k < cur; ++k)
	{
		before += g[k];
	}
	for(int k = cur + 1; k < static_cast<int>(g.size()); ++k)
	{
		after += g[k];
	}
	const Source* src = m_state.numberedSource();
	const std::string hints =
	    fmt::format("  {}  Enter go  Esc cancel ",
	                src ? src->m_entry.rangeString() : std::string());
	return hbox({
	           text(" frame › ") | color(Color::Yellow) | bold,
	           text(before),
	           text(cur < static_cast<int>(g.size()) ? g[cur] : " ") | inverted,
	           text(after),
	           filler(),
	           text(hints) | dim,
	       }) |
	       bgcolor(Color::GrayDark);
}

Element Viewer::renderStatus(const ImageInfoPtr& info)
{
	std::string right;
	if(m_inspector.isOpen())
	{
		right = fmt::format("Esc close inspector  ctrl+click pick  ? help  {}",
		                    m_tile ? "q untile" : "q exit viewer");
	}
	else if(m_tile)
	{
		right = "wheel zoom  click select  Enter open  ? help  q untile";
	}
	else
	{
		right =
		    "2 meta  3 files  4 inspect  5 layers  6 colour  t tile  ? help  "
		    "q exit viewer";
	}
	if(m_state.m_sources.size() > 1 && !m_inspector.isOpen())
	{
		right = "n/p next/prev  " + right;
	}
	// Frame and fps are on the playbar above; the cache stays here.
	Elements playback;
	std::string cache;
	if(m_state.frameCount() > 1)
	{
		if(!m_playbackCap)
		{
			playback.push_back(text("full res ") | color(Color::Yellow));
		}
		cache = fmt::format("cache {}/{}",
		                    humanSize(m_ctx.m_svc.bytesUsed()),
		                    humanSize(m_ctx.m_svc.budget()));
		// Over ssh both ends hold frames: decoded here (remote, where rvtui
		// runs) and kept by the terminal (local, where it shows them).
		if(m_ctx.m_caps.m_graphics == GraphicsMode::KITTY &&
		   m_ctx.m_caps.m_transfer == Transfer::DIRECT &&
		   m_ctx.m_kitty.terminalCache() > 0)
		{
			cache = fmt::format("cache remote {}/{} local {}/{}",
			                    humanSize(m_ctx.m_svc.bytesUsed()),
			                    humanSize(m_ctx.m_svc.budget()),
			                    humanSize(m_ctx.m_kitty.cacheUse().m_bytes),
			                    humanSize(m_ctx.m_kitty.terminalCache()));
		}
	}
	return m_ctx.statusLine(right, std::move(playback), cache);
}

} // namespace rv
