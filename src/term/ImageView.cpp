#include "term/ImageView.h"

#include "term/Kitty.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/screen.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace rv
{

namespace
{

class ImageNode : public ftxui::Node
{
public:
	ImageNode(ImageSlot* slot,
	          LayerImagePtr img,
	          ViewParams view,
	          DisplayParams disp,
	          OverlayText overlay)
	    : m_slot(slot), m_img(std::move(img)), m_view(view), m_disp(disp),
	      m_overlay(std::move(overlay))
	{
	}

	void ComputeRequirement() override
	{
		requirement_ = {};
		requirement_.min_x = 1;
		requirement_.min_y = 1;
		requirement_.flex_grow_x = requirement_.flex_grow_y = 1;
		requirement_.flex_shrink_x = requirement_.flex_shrink_y = 1;
	}

	void Render(ftxui::Screen& screen) override
	{
		m_slot->draw(screen, box_, m_img, m_view, m_disp);
		m_slot->drawOverlay(screen, m_overlay);
	}

private:
	ImageSlot* m_slot;
	LayerImagePtr m_img;
	ViewParams m_view;
	DisplayParams m_disp;
	OverlayText m_overlay;
};

class AfterNode : public ftxui::Node
{
public:
	AfterNode(ftxui::Element child, std::function<void(ftxui::Screen&)> after)
	    : ftxui::Node({std::move(child)}), m_after(std::move(after))
	{
	}

	void ComputeRequirement() override
	{
		children_[0]->ComputeRequirement();
		requirement_ = children_[0]->requirement();
	}

	void SetBox(ftxui::Box box) override
	{
		ftxui::Node::SetBox(box);
		children_[0]->SetBox(box);
	}

	void Render(ftxui::Screen& screen) override
	{
		children_[0]->Render(screen);
		m_after(screen);
	}

private:
	std::function<void(ftxui::Screen&)> m_after;
};

class PlaceNode : public ftxui::Node
{
public:
	PlaceNode(ftxui::Elements children, std::vector<ftxui::Box> boxes)
	    : ftxui::Node(std::move(children)), m_boxes(std::move(boxes))
	{
	}

	void ComputeRequirement() override
	{
		for(auto& c : children_)
		{
			c->ComputeRequirement();
		}
		requirement_ = {};
		requirement_.min_x = 1;
		requirement_.min_y = 1;
		requirement_.flex_grow_x = requirement_.flex_grow_y = 1;
		requirement_.flex_shrink_x = requirement_.flex_shrink_y = 1;
	}

	void SetBox(ftxui::Box box) override
	{
		ftxui::Node::SetBox(box);
		for(size_t i = 0; i < children_.size() && i < m_boxes.size(); ++i)
		{
			const ftxui::Box& b = m_boxes[i];
			children_[i]->SetBox({box.x_min + b.x_min,
			                      box.x_min + b.x_max,
			                      box.y_min + b.y_min,
			                      box.y_min + b.y_max});
		}
	}

private:
	std::vector<ftxui::Box> m_boxes;
};

// "L|<ident>|<layer>|<reduce>" (ImageService::layerKey): the layer and
// reduction, whichever file. Frames of one playback share it.
std::string originSet(const std::string& origin)
{
	const size_t last = origin.rfind('|');
	const size_t layer = last == std::string::npos || last == 0
	                         ? std::string::npos
	                         : origin.rfind('|', last - 1);
	return layer == std::string::npos ? origin : origin.substr(layer);
}

// Per cell, the average RGB of `bmp` laid over cols x rows cells.
std::vector<uint8_t> cellColors(const Rgba8Image& bmp, int cols, int rows)
{
	std::vector<uint8_t> rgb(static_cast<size_t>(cols) * rows * 3, 0);
	if(bmp.m_width <= 0 || bmp.m_height <= 0)
	{
		return rgb;
	}
	// A 4x4 sample grid per cell is plenty for a background tint.
	constexpr int N = 4;
	for(int r = 0; r < rows; ++r)
	{
		for(int c = 0; c < cols; ++c)
		{
			int sum[3] = {0, 0, 0};
			for(int sy = 0; sy < N; ++sy)
			{
				const int y = std::min(bmp.m_height - 1,
				                       static_cast<int>((r + (sy + 0.5) / N) *
				                                        bmp.m_height / rows));
				for(int sx = 0; sx < N; ++sx)
				{
					const int x =
					    std::min(bmp.m_width - 1,
					             static_cast<int>((c + (sx + 0.5) / N) *
					                              bmp.m_width / cols));
					const uint8_t* p =
					    &bmp.m_pixels[(static_cast<size_t>(y) * bmp.m_width + x) *
					                  4];
					for(int k = 0; k < 3; ++k)
					{
						sum[k] += p[k] * p[3] / 255; // transparent = black
					}
				}
			}
			uint8_t* out = &rgb[(static_cast<size_t>(r) * cols + c) * 3];
			for(int k = 0; k < 3; ++k)
			{
				out[k] = static_cast<uint8_t>(sum[k] / (N * N));
			}
		}
	}
	return rgb;
}

} // namespace

ImageSlot::ImageSlot(const TermCaps& caps,
                     kitty::Transmitter& tx,
                     std::function<void()> ready)
    : m_caps(caps), m_tx(tx), m_ready(std::move(ready)), m_id(tx.allocId()),
      m_shownId(m_id)
{
}

ImageSlot::~ImageSlot()
{
	if(m_worker.joinable())
	{
		m_worker.request_stop();
		m_worker.join();
	}
	for(const Prepared& p : m_done)
	{
		releaseUnsent(p);
	}
	for(const auto* r : {&m_request, &m_store})
	{
		if(*r && (*r)->m_cache)
		{
			m_tx.cacheDropped((*r)->m_bytes);
		}
	}
	for(const Prepared& p : m_stored)
	{
		releaseUnsent(p);
	}
	while(!m_cache.empty())
	{
		drop(m_cache.begin());
	}
	if(m_transmitted)
	{
		m_tx.write(kitty::deleteImage(m_id, m_tx.tmux()), true);
	}
	m_tx.releaseId(m_id);
}

ftxui::Element ImageSlot::element(LayerImagePtr img,
                                  const ViewParams& view,
                                  const DisplayParams& disp,
                                  OverlayText overlay)
{
	return std::make_shared<ImageNode>(this,
	                                   std::move(img),
	                                   view,
	                                   disp,
	                                   std::move(overlay));
}

ftxui::Element drawAfter(ftxui::Element child,
                         std::function<void(ftxui::Screen&)> after)
{
	return std::make_shared<AfterNode>(std::move(child), std::move(after));
}

ftxui::Element placeAt(ftxui::Elements children, std::vector<ftxui::Box> boxes)
{
	return std::make_shared<PlaceNode>(std::move(children), std::move(boxes));
}

void compactPlaceholders(ftxui::Screen& screen)
{
	static const std::string BARE = kitty::barePlaceholderCell();
	const int last = screen.dimx() - 1;
	for(int y = 0; y < screen.dimy(); ++y)
	{
		std::optional<std::pair<int, int>> left;
		ftxui::Color leftFg;
		for(int x = 0; x < screen.dimx(); ++x)
		{
			auto& px = screen.PixelAt(x, y); // Pixel, or Cell in newer FTXUI
			const auto rc = kitty::placeholderRowCol(px.character);
			if(rc && left && px.foreground_color == leftFg &&
			   rc->first == left->first && rc->second == left->second + 1)
			{
				px.character = BARE;
			}
			else if(rc && x == last && x > 0)
			{
				// With autowrap off (FTXUI's ?7l), tmux hangs the diacritics
				// of a last-column cell on the cell before it: an "e" of
				// overlay text grew accents. Extend what is to the left
				// instead.
				const auto& prev = screen.PixelAt(x - 1, y);
				px.character = " ";
				px.background_color = prev.background_color;
			}
			left = rc;
			leftFg = px.foreground_color;
		}
	}
}

void paintOverlay(ftxui::Screen& screen,
                  const ftxui::Box& clip,
                  const std::vector<OverlayCell>& cells,
                  const CellColorFn& colorAt)
{
	constexpr double DARKEN = 0.4; // strip = image colour at 40%
	for(const auto& c : cells)
	{
		if(c.m_x < clip.x_min || c.m_x > clip.x_max || c.m_y < clip.y_min ||
		   c.m_y > clip.y_max)
		{
			continue;
		}
		std::array<uint8_t, 3> rgb{0, 0, 0};
		if(auto under = colorAt(c.m_x, c.m_y))
		{
			rgb = *under;
		}
		auto& px = screen.PixelAt(c.m_x, c.m_y);
		px.bold = px.dim = px.italic = px.inverted = px.underlined = false;
		px.character = c.m_glyph;
		px.foreground_color = c.m_dim ? ftxui::Color::RGB(150, 150, 150)
		                              : ftxui::Color::RGB(255, 255, 255);
		px.background_color =
		    ftxui::Color::RGB(static_cast<uint8_t>(rgb[0] * DARKEN),
		                      static_cast<uint8_t>(rgb[1] * DARKEN),
		                      static_cast<uint8_t>(rgb[2] * DARKEN));
	}
}

std::optional<ImageSlot::CellRect> ImageSlot::frameCells() const
{
	if(!m_last || m_frame.width() <= 0 || m_frame.height() <= 0)
	{
		return std::nullopt;
	}
	// Output px of the frame edges, then cells (cells hold
	// pxPerCell * quality output px).
	const double cw = pxPerCellX() * m_quality, ch = pxPerCellY() * m_quality;
	const double l =
	    (m_frame.m_x0 - m_mapping.m_originX) * m_mapping.m_scale / cw;
	const double r =
	    (m_frame.m_x1 + 1 - m_mapping.m_originX) * m_mapping.m_scale / cw;
	const double t =
	    (m_frame.m_y0 - m_mapping.m_originY) * m_mapping.m_scale / ch;
	const double b =
	    (m_frame.m_y1 + 1 - m_mapping.m_originY) * m_mapping.m_scale / ch;
	// Cells whose centre is inside.
	const int c0 = static_cast<int>(std::ceil(l - 0.5));
	const int c1 = static_cast<int>(std::floor(r - 0.5));
	const int r0 = static_cast<int>(std::ceil(t - 0.5));
	const int r1 = static_cast<int>(std::floor(b - 0.5));
	if(c1 < c0 || r1 < r0)
	{
		return std::nullopt;
	}
	return CellRect{m_box.x_min + c0,
	                m_box.y_min + r0,
	                c1 - c0 + 1,
	                r1 - r0 + 1};
}

std::optional<std::array<uint8_t, 3>> ImageSlot::cellColor(int x, int y) const
{
	const int c = x - m_box.x_min, r = y - m_box.y_min;
	if(c < 0 || r < 0 || c >= m_cols || r >= m_rows ||
	   m_cellRgb.size() < static_cast<size_t>(m_cols * m_rows * 3))
	{
		return std::nullopt;
	}
	const uint8_t* p = &m_cellRgb[(static_cast<size_t>(r) * m_cols + c) * 3];
	return std::array<uint8_t, 3>{p[0], p[1], p[2]};
}

void ImageSlot::drawOverlay(ftxui::Screen& screen,
                            const OverlayText& overlay) const
{
	if(overlayEmpty(overlay))
	{
		return;
	}
	const auto frame = frameCells();
	if(!frame)
	{
		return;
	}
	const ftxui::Box clip{m_box.x_min,
	                      m_box.x_min + m_cols - 1,
	                      m_box.y_min,
	                      m_box.y_min + m_rows - 1};
	paintOverlay(
	    screen,
	    clip,
	    layoutOverlay(frame->m_x, frame->m_y, frame->m_w, frame->m_h, overlay),
	    [this](int x, int y) { return cellColor(x, y); });
}

int ImageSlot::pxPerCellX() const
{
	return m_caps.m_graphics == GraphicsMode::KITTY ? m_caps.m_cellW : 1;
}
int ImageSlot::pxPerCellY() const
{
	return m_caps.m_graphics == GraphicsMode::KITTY ? m_caps.m_cellH : 2;
}

std::optional<std::pair<double, double>> ImageSlot::imageCoordAt(int cx,
                                                                 int cy) const
{
	if(!m_last || cx < m_box.x_min || cx > m_box.x_max || cy < m_box.y_min ||
	   cy > m_box.y_max)
	{
		return std::nullopt;
	}
	double ox = (cx - m_box.x_min + 0.5) * pxPerCellX() * m_quality;
	double oy = (cy - m_box.y_min + 0.5) * pxPerCellY() * m_quality;
	return std::make_pair(m_mapping.imageX(ox), m_mapping.imageY(oy));
}

std::tuple<int, int, double> ImageSlot::pictureSize(const Key& key)
{
	const bool kittyMode = key.m_mode == GraphicsMode::KITTY;
	int pw = key.m_cols * (kittyMode ? key.m_cellW : 1);
	int ph = key.m_rows * (kittyMode ? key.m_cellH : 2);
	double quality = 1.0;
	if(kittyMode && static_cast<double>(pw) * ph > key.m_maxPixels)
	{
		quality = std::sqrt(key.m_maxPixels / (static_cast<double>(pw) * ph));
		pw = std::max(1, static_cast<int>(pw * quality));
		ph = std::max(1, static_cast<int>(ph * quality));
	}
	return {pw, ph, quality};
}

ImageSlot::Prepared ImageSlot::prepare(Request req) const
{
	const Key& key = req.m_key;
	const bool kittyMode = key.m_mode == GraphicsMode::KITTY;
	Prepared p;
	const auto [pw, ph, quality] = pictureSize(key);
	p.m_quality = quality;
	ViewParams scaled = key.m_view;
	scaled.m_zoom *= p.m_quality;
	const LayerImage& img = *req.m_img;
	p.m_mapping = resolveView(key.m_view.m_fitFrame ? img.m_displayWindow
	                                                : img.fitBounds(),
	                          pw,
	                          ph,
	                          scaled);
	Rgba8Image bmp = renderLayer(img, pw, ph, scaled, key.m_disp);
	p.m_cellRgb = cellColors(bmp, key.m_cols, key.m_rows);
	spdlog::debug(
	    "slot {}: {}x{} cells, {}x{} px (cell {}x{}, q {:.3f}), "
	    "fit {} zoom {:.3f} centre {:.1f},{:.1f} origin {:.1f},{:.1f}",
	    m_id,
	    key.m_cols,
	    key.m_rows,
	    pw,
	    ph,
	    key.m_cellW,
	    key.m_cellH,
	    p.m_quality,
	    key.m_view.m_fit,
	    key.m_view.m_zoom,
	    key.m_view.m_centerX,
	    key.m_view.m_centerY,
	    p.m_mapping.m_originX,
	    p.m_mapping.m_originY);
	if(kittyMode)
	{
		kitty::TransmitOptions opt;
		opt.m_id = m_id;
		// Keep 32 ids for slots to come (tiles, the playbar).
		constexpr size_t KEEP_FREE_IDS = 32;
		if(req.m_cache)
		{
			if(const uint32_t id = m_tx.tryAllocId(KEEP_FREE_IDS))
			{
				opt.m_id = p.m_cacheId = id;
				p.m_bytes = req.m_bytes;
			}
			else
			{
				m_tx.cacheDropped(req.m_bytes); // no id left: sent as ever
			}
		}
		opt.m_cols = key.m_cols;
		opt.m_rows = key.m_rows;
		p.m_escapes = m_tx.encode(bmp, opt);
	}
	else
	{
		p.m_bitmap = std::move(bmp);
	}
	p.m_key = key;
	p.m_fromDraw = req.m_fromDraw;
	p.m_storeOnly = req.m_storeOnly;
	p.m_img = std::move(req.m_img);
	return p;
}

void ImageSlot::apply(Prepared p)
{
	if(p.m_key.m_mode == GraphicsMode::KITTY)
	{
		// Replaces the picture under m_id (and, by its placement id, the
		// placement). A new size in cells (tiles zoomed, a fold, a resize):
		// drop the old placement first, in case a terminal keeps it.
		// Written ahead of the frame FTXUI is about to print (same stream).
		if(m_transmitted &&
		   (p.m_key.m_cols != m_cols || p.m_key.m_rows != m_rows))
		{
			m_tx.write(kitty::deletePlacements(m_id, m_tx.tmux()));
		}
		m_tx.write(p.m_escapes);
		m_transmitted = true;
		m_shownId = p.m_cacheId ? p.m_cacheId : m_id;
		if(p.m_cacheId)
		{
			keep(p);
		}
	}
	else
	{
		m_halfblock = std::move(p.m_bitmap);
	}
	m_mapping = p.m_mapping;
	m_quality = p.m_quality;
	m_cellRgb = std::move(p.m_cellRgb);
	m_cols = p.m_key.m_cols;
	m_rows = p.m_key.m_rows;
	m_frame = p.m_img->m_displayWindow;
	m_last = p.m_key;
	m_keepAlive = std::move(p.m_img);
}

bool ImageSlot::sameView(Key a, const Key& b)
{
	a.m_img = b.m_img;
	return a == b;
}

bool ImageSlot::sameCachedView(Key a, const Key& b)
{
	a.m_img = b.m_img;
	a.m_maxPixels = b.m_maxPixels;
	return a == b;
}

bool ImageSlot::canKeep(const Key& key, const LayerImage& img) const
{
	return key.m_mode == GraphicsMode::KITTY &&
	       key.m_transfer == Transfer::DIRECT && m_tx.terminalCache() > 0 &&
	       !img.m_origin.empty();
}

bool ImageSlot::cacheable(const Key& key, const LayerImage& img) const
{
	return m_cacheOn && canKeep(key, img);
}

std::list<ImageSlot::Cached>::iterator
ImageSlot::findKept(const Key& key, const std::string& origin)
{
	return std::ranges::find_if(m_cache,
	                            [&](const Cached& c)
	                            {
		                            return c.m_origin == origin &&
		                                   sameCachedView(c.m_key, key);
	                            });
}

std::list<ImageSlot::Cached>::iterator
ImageSlot::findCached(const Key& key, const LayerImage& img)
{
	return cacheable(key, img) ? findKept(key, img.m_origin) : m_cache.end();
}

bool ImageSlot::store(LayerImagePtr img, int maxPixels)
{
	if(!m_ready || !m_lastDrawn || !img)
	{
		return false;
	}
	Key key = *m_lastDrawn;
	key.m_img = img.get();
	key.m_maxPixels = maxPixels;
	if(!canKeep(key, *img) || findKept(key, img->m_origin) != m_cache.end())
	{
		return false;
	}
	std::lock_guard lk(m_jobMu);
	if(m_store || (m_busy && m_busy->m_storeOnly) || !m_stored.empty())
	{
		return false; // one at a time: the link paces them
	}
	const auto [pw, ph, quality] = pictureSize(key);
	const size_t bytes = static_cast<size_t>(pw) * ph * 4;
	if(!makeRoom(key, *img, bytes))
	{
		return false;
	}
	m_store = Request{key, std::move(img), false, true, bytes, true};
	if(!m_worker.joinable())
	{
		m_worker = std::jthread([this](std::stop_token st) { encodeLoop(st); });
	}
	m_jobCv.notify_one();
	return true;
}

bool ImageSlot::flushStored()
{
	std::deque<Prepared> stored;
	{
		std::lock_guard lk(m_jobMu);
		stored.swap(m_stored);
	}
	bool wrote = false;
	for(Prepared& p : stored)
	{
		// Without an id of its own it was encoded under m_id: writing it
		// would replace what is on screen. And a draw may have sent the
		// same frame meanwhile.
		if(!p.m_cacheId ||
		   findKept(p.m_key, p.m_img->m_origin) != m_cache.end())
		{
			releaseUnsent(p);
			continue;
		}
		m_tx.write(p.m_escapes);
		keep(p);
		wrote = true;
	}
	return wrote;
}

bool ImageSlot::storing() const
{
	std::lock_guard lk(m_jobMu);
	return m_store || (m_busy && m_busy->m_storeOnly) || !m_stored.empty();
}

bool ImageSlot::stored(const std::string& origin) const
{
	return m_lastDrawn &&
	       std::ranges::any_of(m_cache,
	                           [&](const Cached& c)
	                           {
		                           return c.m_origin == origin &&
		                                  sameCachedView(c.m_key, *m_lastDrawn);
	                           });
}

bool ImageSlot::storedInAnyView(const std::string& pathPrefix) const
{
	return std::ranges::any_of(m_cache,
	                           [&](const Cached& c)
	                           {
		                           return c.m_origin.compare(2,
		                                                     pathPrefix.size(),
		                                                     pathPrefix) == 0;
	                           });
}

void ImageSlot::showCached(std::list<Cached>::iterator it,
                           const Key& key,
                           LayerImagePtr img)
{
	m_cache.splice(m_cache.begin(), m_cache, it); // most recently shown
	m_tx.countKeptShown();
	const Cached& c = m_cache.front();
	m_shownId = c.m_id;
	m_mapping = c.m_mapping;
	m_quality = c.m_quality;
	m_cellRgb = c.m_cellRgb;
	m_cols = key.m_cols;
	m_rows = key.m_rows;
	m_frame = c.m_frame;
	m_last = key;
	m_keepAlive = std::move(img);
}

bool ImageSlot::makeRoom(const Key& key, const LayerImage& img, size_t bytes)
{
	// Kept frames of another view, layer or reduction go only for room, the
	// least recently shown first: a pan away and back (or another AOV and
	// back) finds them still there. Dropping them at the first step of a pan
	// emptied the terminal each time.
	const std::string set = originSet(img.m_origin);
	while(!m_tx.reserveCache(bytes))
	{
		const auto other =
		    std::find_if(m_cache.rbegin(),
		                 m_cache.rend(),
		                 [&](const Cached& c)
		                 {
			                 return !sameCachedView(c.m_key, key) ||
			                        c.m_set != set;
		                 });
		if(other == m_cache.rend())
		{
			return false; // all of this view: they stay, this one goes as ever
		}
		drop(std::prev(other.base()));
	}
	return true;
}

void ImageSlot::keep(const Prepared& p)
{
	// Stored (store()) while a draw prepared it too: the one shown stays.
	if(const auto twin = findKept(p.m_key, p.m_img->m_origin);
	   twin != m_cache.end())
	{
		drop(twin);
	}
	Key key = p.m_key;
	key.m_img = nullptr;
	m_cache.push_front(Cached{p.m_img->m_origin,
	                          originSet(p.m_img->m_origin),
	                          key,
	                          p.m_cacheId,
	                          p.m_bytes,
	                          p.m_mapping,
	                          p.m_quality,
	                          p.m_cellRgb,
	                          p.m_img->m_displayWindow});
}

void ImageSlot::drop(std::list<Cached>::iterator it)
{
	m_tx.write(kitty::deleteImage(it->m_id, m_tx.tmux()));
	m_tx.releaseId(it->m_id);
	m_tx.cacheDropped(it->m_bytes);
	m_cache.erase(it);
}

void ImageSlot::releaseUnsent(const Prepared& p) const
{
	if(p.m_cacheId)
	{
		m_tx.releaseId(p.m_cacheId);
		m_tx.cacheDropped(p.m_bytes);
	}
}

void ImageSlot::requestLocked(const Key& key, LayerImagePtr img, bool fromDraw)
{
	const bool done =
	    std::ranges::any_of(m_done,
	                        [&](const Prepared& p) { return p.m_key == key; });
	if(done || (m_busy && m_busy->m_key == key) ||
	   (m_request && m_request->m_key == key))
	{
		return; // on its way, or done and waiting for its draw
	}
	bool cache = false;
	size_t bytes = 0;
	if(img && cacheable(key, *img))
	{
		const auto [pw, ph, quality] = pictureSize(key);
		bytes = static_cast<size_t>(pw) * ph * 4; // RGBA, as kept
		cache = makeRoom(key, *img, bytes);
	}
	if(m_request && m_request->m_cache)
	{
		m_tx.cacheDropped(m_request->m_bytes); // replaced before it went
	}
	m_request = Request{key, std::move(img), fromDraw, cache, bytes};
	if(!m_worker.joinable())
	{
		m_worker = std::jthread([this](std::stop_token st) { encodeLoop(st); });
	}
	m_jobCv.notify_one();
}

void ImageSlot::prepareAhead(LayerImagePtr img)
{
	if(!m_ready || !m_lastDrawn || !img ||
	   m_lastDrawn->m_mode != GraphicsMode::KITTY)
	{
		return;
	}
	Key key = *m_lastDrawn;
	key.m_img = img.get();
	if((m_last && *m_last == key) || findCached(key, *img) != m_cache.end())
	{
		return; // on screen, or kept in the terminal
	}
	std::lock_guard lk(m_jobMu);
	requestLocked(key, std::move(img));
}

bool ImageSlot::pending() const
{
	std::lock_guard lk(m_jobMu);
	return m_request || m_busy || !m_done.empty();
}

void ImageSlot::encodeLoop(std::stop_token stop)
{
	for(;;)
	{
		Request req;
		{
			std::unique_lock lk(m_jobMu);
			if(!m_jobCv.wait(lk,
			                 stop,
			                 [&] { return m_request.has_value() || m_store; }))
			{
				return; // stop requested
			}
			std::optional<Request>& next = m_request ? m_request : m_store;
			req = std::move(*next);
			next.reset();
			m_busy = Request{req.m_key, nullptr};
			m_busy->m_storeOnly = req.m_storeOnly;
		}
		Prepared p = prepare(std::move(req));
		if(p.m_storeOnly)
		{
			{
				std::lock_guard lk(m_jobMu);
				m_stored.push_back(std::move(p));
				m_busy.reset();
			}
			m_ready(); // for the UI to write it, and store the next
			continue;
		}
		bool notify;
		{
			std::lock_guard lk(m_jobMu);
			// Also an older picture a draw asked for: shown meanwhile.
			notify =
			    m_waitFor && (*m_waitFor == p.m_key ||
			                  (p.m_fromDraw && sameView(p.m_key, *m_waitFor)));
			m_done.push_back(std::move(p));
			if(m_done.size() > MAX_DONE)
			{
				releaseUnsent(m_done.front());
				m_done.pop_front();
			}
			m_busy.reset();
		}
		if(notify)
		{
			m_ready();
		}
	}
}

void ImageSlot::draw(ftxui::Screen& screen,
                     const ftxui::Box& box,
                     const LayerImagePtr& img,
                     const ViewParams& view,
                     const DisplayParams& disp)
{
	m_box = box;
	int cols = box.x_max - box.x_min + 1, rows = box.y_max - box.y_min + 1;
	if(cols <= 0 || rows <= 0 || !img)
	{
		return;
	}
	const bool kittyMode = m_caps.m_graphics == GraphicsMode::KITTY;
	if(kittyMode)
	{
		cols = std::min(cols, kitty::MAX_PLACEHOLDER_INDEX);
		rows = std::min(rows, kitty::MAX_PLACEHOLDER_INDEX);
	}

	Key key{img.get(),
	        view,
	        disp,
	        cols,
	        rows,
	        m_caps.m_cellW,
	        m_caps.m_cellH,
	        m_maxPixels,
	        m_caps.m_graphics,
	        m_caps.m_transfer,
	        m_caps.m_resend};
	m_areaW = cols * pxPerCellX();
	m_areaH = rows * pxPerCellY();
	if(!m_lastDrawn || !sameCachedView(*m_lastDrawn, key))
	{
		m_viewSince = std::chrono::steady_clock::now();
	}
	if(const auto kept = kittyMode ? findCached(key, *img) : m_cache.end();
	   kept != m_cache.end())
	{
		// Kept in the terminal: show it by its id, send nothing.
		if(!m_last || !(*m_last == key))
		{
			showCached(kept, key, img);
		}
		m_lastDrawn = key;
		std::lock_guard lk(m_jobMu);
		m_waitFor.reset();
	}
	else if(kittyMode && m_ready)
	{
		m_lastDrawn = key;
		// One lock from "is it done?" to "wake me when it is", or the
		// worker could finish in between and nobody would redraw.
		std::unique_lock lk(m_jobMu);
		auto it = std::ranges::find_if(m_done,
		                               [&](const Prepared& p)
		                               { return p.m_key == key; });
		if(it != m_done.end())
		{
			Prepared done = std::move(*it);
			m_done.erase(it); // older ones stay: one may be drawn next
			m_waitFor.reset();
			lk.unlock();
			apply(std::move(done)); // now on screen: nothing to wait for
		}
		else if(!m_last || !(*m_last == key))
		{
			requestLocked(key, img, true);
			m_waitFor = key; // redraw once it is done
			// Scrubbing can outrun encoding (inline over ssh is slow): each
			// picture came back after the draw had moved on, and nothing
			// showed until the mouse stopped. Meanwhile show the newest one
			// a draw asked for, while the link keeps up.
			constexpr double MAX_INTERIM_BACKLOG = 0.1; // seconds
			auto newer =
			    std::find_if(m_done.rbegin(),
			                 m_done.rend(),
			                 [&](const Prepared& p)
			                 {
				                 return p.m_fromDraw && sameView(p.m_key, key);
			                 });
			if(newer != m_done.rend() &&
			   m_tx.linkBacklog() < MAX_INTERIM_BACKLOG)
			{
				auto at = std::prev(newer.base());
				Prepared interim = std::move(*at);
				m_done.erase(at);
				lk.unlock();
				apply(std::move(interim));
			}
		}
		else
		{
			m_waitFor.reset();
		}
	}
	else if(!m_last || !(*m_last == key))
	{
		apply(prepare({key, img}));
	}

	if(kittyMode)
	{
		if(!m_transmitted)
		{
			return; // the first picture is still on its way
		}
		cols = std::min(cols, m_cols); // until a resized picture is out
		rows = std::min(rows, m_rows);
		const ftxui::Color fg =
		    ftxui::Color::Palette256(static_cast<uint8_t>(m_shownId));
		for(int r = 0; r < rows; ++r)
		{
			for(int c = 0; c < cols; ++c)
			{
				auto& px = screen.PixelAt(box.x_min + c, box.y_min + r);
				px.character = kitty::placeholderCell(r, c);
				px.foreground_color = fg;
				px.background_color = ftxui::Color::Default;
			}
		}
		return;
	}

	static const std::string UPPER = "▀"; // ▀
	for(int r = 0; r < rows; ++r)
	{
		for(int c = 0; c < cols; ++c)
		{
			const int hw = m_halfblock.m_width;
			const uint8_t* top =
			    &m_halfblock.m_pixels[(static_cast<size_t>(2 * r) * hw + c) * 4];
			const uint8_t* bot =
			    &m_halfblock
			         .m_pixels[(static_cast<size_t>(2 * r + 1) * hw + c) * 4];
			auto& px = screen.PixelAt(box.x_min + c, box.y_min + r);
			px.character = UPPER;
			px.foreground_color =
			    top[3] ? ftxui::Color::RGB(top[0], top[1], top[2])
			           : ftxui::Color::Default;
			px.background_color =
			    bot[3] ? ftxui::Color::RGB(bot[0], bot[1], bot[2])
			           : ftxui::Color::Default;
			if(!top[3] && !bot[3])
			{
				px.character = " ";
			}
		}
	}
}

} // namespace rv
