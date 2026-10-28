#include "image/Render.h"

#include "image/Colour.h"
#include "util/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace rega
{

const char* channelModeName(ChannelMode m)
{
	switch(m)
	{
		case ChannelMode::COLOR:
			return "RGB";
		case ChannelMode::RED:
			return "R";
		case ChannelMode::GREEN:
			return "G";
		case ChannelMode::BLUE:
			return "B";
		case ChannelMode::ALPHA:
			return "A";
		case ChannelMode::LUMA:
			return "Luma";
	}
	return "?";
}

ViewMapping resolveView(
    const Box2i& dispWin, int outW, int outH, const ViewParams& view, double)
{
	ViewMapping m;
	double iw = dispWin.width(), ih = dispWin.height();
	double cx = view.m_centerX, cy = view.m_centerY;
	if(view.m_fit || iw <= 0 || ih <= 0)
	{
		m.m_scale =
		    std::min(outW / std::max(1.0, iw), outH / std::max(1.0, ih));
		cx = dispWin.m_x0 + iw / 2.0;
		cy = dispWin.m_y0 + ih / 2.0;
	}
	else
	{
		m.m_scale = view.m_zoom;
	}
	m.m_originX = cx - outW / (2.0 * m.m_scale);
	m.m_originY = cy - outH / (2.0 * m.m_scale);
	return m;
}

float applyDisplay(float v, const DisplayParams& p)
{
	if(!std::isfinite(v))
	{
		return 0.0f;
	}
	v *= std::exp2(p.m_exposure);
	if(v <= 0.0f)
	{
		return 0.0f;
	}
	if(p.m_srgb)
	{
		v = v <= 0.0031308f ? v * 12.92f
		                    : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	}
	if(p.m_gamma != 1.0f)
	{
		v = std::pow(v, 1.0f / p.m_gamma);
	}
	return std::min(v, 1.0f);
}

std::array<int, 3> displayRgb8(float r, float g, float b, const DisplayParams& p)
{
	auto to8 = [](float v) { return static_cast<int>(v * 255 + 0.5f); };
	if(p.m_ocio && p.m_srgb)
	{
		const float gain = std::exp2(p.m_exposure);
		auto finite = [&](float v)
		{ return std::isfinite(v) ? v * gain : 0.0f; };
		r = finite(r);
		g = finite(g);
		b = finite(b);
		p.m_ocio->apply(r, g, b);
		DisplayParams gamma; // gamma only, on display values
		gamma.m_srgb = false;
		gamma.m_gamma = p.m_gamma;
		return {to8(applyDisplay(r, gamma)),
		        to8(applyDisplay(g, gamma)),
		        to8(applyDisplay(b, gamma))};
	}
	return {to8(applyDisplay(r, p)),
	        to8(applyDisplay(g, p)),
	        to8(applyDisplay(b, p))};
}

namespace
{

// 8-bit display transform as a lookup over [0, 1] (after exposure). 64K
// entries keep the steep sRGB toe accurate to well under one 8-bit step.
constexpr int LUT_SIZE = 65536;

struct DisplayLut
{
	float m_gain = 1.0f; // 2^exposure, applied before the lookup
	const std::vector<uint8_t>& m_table;

	explicit DisplayLut(const DisplayParams& p)
	    : m_gain(std::exp2(p.m_exposure)), m_table(table(p))
	{
	}

	// The table for `p` without exposure. Kept per thread while the params
	// stay the same: building it is 65536 powf, and playback renders the
	// same params dozens of times a second.
	static const std::vector<uint8_t>& table(const DisplayParams& p)
	{
		DisplayParams noExposure = p;
		noExposure.m_exposure = 0.0f;
		thread_local std::optional<DisplayParams> cachedFor;
		thread_local std::vector<uint8_t> cached(LUT_SIZE);
		if(cachedFor != noExposure)
		{
			for(int i = 0; i < LUT_SIZE; ++i)
			{
				float v = static_cast<float>(i) / (LUT_SIZE - 1);
				cached[i] = static_cast<uint8_t>(
				    applyDisplay(v, noExposure) * 255.0f + 0.5f);
			}
			cachedFor = noExposure;
		}
		return cached;
	}

	uint8_t operator()(float v) const
	{
		v *= m_gain;
		if(!(v > 0.0f)) // also NaN
		{
			return m_table[0];
		}
		if(v >= 1.0f)
		{
			return m_table[LUT_SIZE - 1];
		}
		return m_table[static_cast<int>(v * (LUT_SIZE - 1) + 0.5f)];
	}
};

// The source pixels (plane indices) covered by one output column or row.
struct Span
{
	int m_lo = 0, m_hi = 0, m_step = 1; // [lo, hi) stepping by step
	bool m_valid = false;
};

// Spans for `count` output pixels along one axis. `origin`/`scale` map output
// to image coordinates; `dataLo` is the data window start; `reduce` the plane
// downsampling; `planeLen` the plane size along this axis.
std::vector<Span> axisSpans(int count,
                            double origin,
                            double scale,
                            int dataLo,
                            int dataHi,
                            int reduce,
                            int planeLen)
{
	std::vector<Span> spans(count);
	for(int o = 0; o < count; ++o)
	{
		double i0 = origin + o / scale, i1 = origin + (o + 1) / scale;
		if(i1 <= dataLo || i0 >= dataHi + 1)
		{
			continue;
		}
		double p0 = (i0 - dataLo) / reduce, p1 = (i1 - dataLo) / reduce;
		int lo = static_cast<int>(std::floor(p0));
		int hi = std::max(lo + 1, static_cast<int>(std::ceil(p1)));
		lo = std::clamp(lo, 0, planeLen - 1);
		hi = std::clamp(hi, lo + 1, planeLen);
		// Cap the work per output pixel; stride through large footprints.
		spans[o] = {lo, hi, std::max(1, (hi - lo) / 6), true};
	}
	return spans;
}

// Outline every image pixel: a faint 1px line on the first output column /
// row of each one, nudging bright pixels darker and dark ones lighter so it
// reads on any content. It fades in with zoom (barely there at the
// threshold) to stay out of the way. Skips transparent output.
void drawPixelGrid(Rgba8Image& out, const ViewMapping& m)
{
	constexpr double MIN_STRENGTH = 0.10, MAX_STRENGTH = 0.22;
	const double ramp = std::clamp((m.m_scale - PIXEL_GRID_MIN_SCALE) /
	                                   (3 * PIXEL_GRID_MIN_SCALE),
	                               0.0,
	                               1.0);
	const int strength = static_cast<int>(
	    256 * (MIN_STRENGTH + (MAX_STRENGTH - MIN_STRENGTH) * ramp));
	auto edges = [](int count, double origin, double scale)
	{
		std::vector<char> e(count, 0);
		for(int o = 1; o < count; ++o)
		{
			e[o] = std::floor(origin + o / scale) !=
			       std::floor(origin + (o - 1) / scale);
		}
		return e;
	};
	const auto ex = edges(out.m_width, m.m_originX, m.m_scale);
	const auto ey = edges(out.m_height, m.m_originY, m.m_scale);
	for(int oy = 0; oy < out.m_height; ++oy)
	{
		for(int ox = 0; ox < out.m_width; ++ox)
		{
			if(!ex[ox] && !ey[oy])
			{
				continue;
			}
			uint8_t* px =
			    &out.m_pixels[(static_cast<size_t>(oy) * out.m_width + ox) * 4];
			if(!px[3])
			{
				continue;
			}
			const int luma = (px[0] * 54 + px[1] * 183 + px[2] * 19) >> 8;
			const int target = luma > 128 ? 0 : 255;
			for(int c = 0; c < 3; ++c)
			{
				px[c] = static_cast<uint8_t>(
				    px[c] + (((target - px[c]) * strength) >> 8));
			}
		}
	}
}

// Draw `box` (image coordinates) as a 1px rectangle one output pixel outside
// it (or on its own edge pixels if `inset`), in `rgb`, dashed if asked. Sides
// that fall off the output are skipped.
void drawBoxOutline(Rgba8Image& out,
                    const ViewMapping& m,
                    const Box2i& box,
                    const uint8_t (&rgb)[3],
                    bool dashed,
                    bool inset = false)
{
	auto toX = [&](double ix)
	{ return static_cast<int>(std::floor((ix - m.m_originX) * m.m_scale)); };
	auto toY = [&](double iy)
	{ return static_cast<int>(std::floor((iy - m.m_originY) * m.m_scale)); };
	const int in = inset ? 1 : 0;
	const int x0 = toX(box.m_x0) - 1 + in, x1 = toX(box.m_x1 + 1.0) - in;
	const int y0 = toY(box.m_y0) - 1 + in, y1 = toY(box.m_y1 + 1.0) - in;
	constexpr int DASH = 4; // output px on, then off
	auto put = [&](int x, int y, int along)
	{
		if(x < 0 || y < 0 || x >= out.m_width || y >= out.m_height ||
		   (dashed && (along / DASH) % 2))
		{
			return;
		}
		uint8_t* px =
		    &out.m_pixels[(static_cast<size_t>(y) * out.m_width + x) * 4];
		px[0] = rgb[0];
		px[1] = rgb[1];
		px[2] = rgb[2];
		px[3] = 255;
	};
	for(int x = x0; x <= x1; ++x)
	{
		put(x, y0, x - x0);
		put(x, y1, x - x0);
	}
	for(int y = y0; y <= y1; ++y)
	{
		put(x0, y, y - y0);
		put(x1, y, y - y0);
	}
}

// A solid ring just outside image pixel (px, py), so the pixel itself stays
// visible when zoomed in. Zoomed out, where the pixel is a speck, the ring
// grows about it to a size that shows (relative to the output, so in
// half-blocks and kitty pixels alike), and thickens with it; a dot in its
// middle then marks which of the pixels inside it is the one.
void drawMarker(Rgba8Image& out, const ViewMapping& m, int px, int py)
{
	auto toX = [&](double ix)
	{ return static_cast<int>(std::floor((ix - m.m_originX) * m.m_scale)); };
	auto toY = [&](double iy)
	{ return static_cast<int>(std::floor((iy - m.m_originY) * m.m_scale)); };
	auto put = [&](int x, int y)
	{
		if(x < 0 || y < 0 || x >= out.m_width || y >= out.m_height)
		{
			return;
		}
		uint8_t* p =
		    &out.m_pixels[(static_cast<size_t>(y) * out.m_width + x) * 4];
		p[0] = MARKER_RGB[0];
		p[1] = MARKER_RGB[1];
		p[2] = MARKER_RGB[2];
		p[3] = 255;
	};
	const int minSide = std::max(5, std::min(out.m_width, out.m_height) / 40);
	const int thick = std::max(1, minSide / 10);
	auto span = [&](int a0, int a1)
	{
		const int grow = std::max(0, minSide - (a1 - a0) + 1) / 2;
		return std::pair(a0 - grow, a1 + grow); // [a0, a1)
	};
	const int px0 = toX(px), px1 = toX(px + 1.0);
	const int py0 = toY(py), py1 = toY(py + 1.0);
	const auto [x0, x1] = span(px0, px1);
	const auto [y0, y1] = span(py0, py1);
	for(int y = y0 - thick; y < y1 + thick; ++y)
	{
		for(int x = x0 - thick; x < x1 + thick; ++x)
		{
			if(x < x0 || x >= x1 || y < y0 || y >= y1)
			{
				put(x, y);
			}
		}
	}
	if(x0 != px0 || y0 != py0) // grown: the ring holds more than the pixel
	{
		const int cx = (px0 + px1) / 2 - thick / 2;
		const int cy = (py0 + py1) / 2 - thick / 2;
		for(int y = cy; y < cy + thick; ++y)
		{
			for(int x = cx; x < cx + thick; ++x)
			{
				put(x, y);
			}
		}
	}
}

} // namespace

Rgba8Image renderLayer(const LayerImage& img,
                       int outW,
                       int outH,
                       const ViewParams& view,
                       const DisplayParams& disp)
{
	Rgba8Image out;
	out.m_width = std::max(1, outW);
	out.m_height = std::max(1, outH);
	out.m_pixels.assign(static_cast<size_t>(out.m_width) * out.m_height * 4, 0);
	if(img.m_planes.empty())
	{
		return out;
	}

	ChannelMap cm = mapChannels(img.m_channelNames);
	auto plane = [&](int idx) -> const Plane*
	{ return idx >= 0 ? &img.m_planes[idx] : nullptr; };
	const Plane* shown[4] = {plane(cm.m_r),
	                         plane(cm.m_g),
	                         plane(cm.m_b),
	                         plane(cm.m_a)};

	ViewMapping m =
	    resolveView(view.m_fitFrame ? img.m_displayWindow : img.fitBounds(),
	                out.m_width,
	                out.m_height,
	                view);
	const auto& dw = img.m_dataWindow;
	const auto cols = axisSpans(out.m_width,
	                            m.m_originX,
	                            m.m_scale,
	                            dw.m_x0,
	                            dw.m_x1,
	                            img.m_reduce,
	                            img.m_width);
	const auto rows = axisSpans(out.m_height,
	                            m.m_originY,
	                            m.m_scale,
	                            dw.m_y0,
	                            dw.m_y1,
	                            img.m_reduce,
	                            img.m_height);

	DisplayParams lutParams = disp;
	lutParams.m_ocio.reset(); // the table keys on the scalar settings only
	// OCIO maps colours, not channels: exposure, then its 3D LUT, then the
	// table does gamma alone.
	const ColourTransform* ocio =
	    disp.m_srgb && disp.m_mode != ChannelMode::ALPHA ? disp.m_ocio.get()
	                                                     : nullptr;
	const float gain = std::exp2(disp.m_exposure);
	if(ocio)
	{
		lutParams.m_exposure = 0.0f;
		lutParams.m_srgb = false;
	}
	if(disp.m_mode == ChannelMode::ALPHA)
	{
		lutParams = DisplayParams{};
		lutParams.m_srgb = false; // alpha is shown as-is
	}
	const DisplayLut lut(lutParams);
	const int planeW = img.m_width;

	// One pass over the output, reading the planes as `T` (half bits or
	// float): a single storage type keeps the type check out of the loop.
	auto paint =
	    [&]<class T>(const T* pr, const T* pg, const T* pb, const T* pa)
	{
		auto toFloat = [](T v)
		{
			if constexpr(std::is_same_v<T, uint16_t>)
			{
				return halfToFloat(v);
			}
			else
			{
				return v;
			}
		};
		parallelFor(
		    out.m_height,
		    16,
		    [&](int rowBegin, int rowEnd)
		    {
			    for(int oy = rowBegin; oy < rowEnd; ++oy)
			    {
				    const Span& ry = rows[oy];
				    if(!ry.m_valid)
				    {
					    continue;
				    }
				    uint8_t* dst =
				        &out.m_pixels[static_cast<size_t>(oy) * out.m_width * 4];
				    for(int ox = 0; ox < out.m_width; ++ox)
				    {
					    const Span& rx = cols[ox];
					    if(!rx.m_valid)
					    {
						    continue;
					    }
					    // Bit 0: a NaN went into this pixel, bit 1: an inf.
					    int bad = 0;
					    auto flag = [&bad](float v)
					    { bad |= std::isnan(v) ? 1 : 2; };
					    auto sample = [&](const T* p) -> float
					    {
						    if(!p)
						    {
							    return 0.0f;
						    }
						    if(rx.m_hi - rx.m_lo == 1 && ry.m_hi - ry.m_lo == 1)
						    {
							    float v = toFloat(
							        p[static_cast<size_t>(ry.m_lo) * planeW +
							          rx.m_lo]);
							    if(std::isfinite(v))
							    {
								    return v;
							    }
							    flag(v);
							    return 0.0f;
						    }
						    float sum = 0;
						    int n = 0;
						    for(int y = ry.m_lo; y < ry.m_hi; y += ry.m_step)
						    {
							    const T* row =
							        p + static_cast<size_t>(y) * planeW;
							    for(int x = rx.m_lo; x < rx.m_hi;
							        x += rx.m_step)
							    {
								    const float v = toFloat(row[x]);
								    if(std::isfinite(v))
								    {
									    sum += v;
									    ++n;
								    }
								    else
								    {
									    flag(v);
								    }
							    }
						    }
						    return n ? sum / n : 0.0f;
					    };
					    float r = 0, g = 0, b = 0;
					    switch(disp.m_mode)
					    {
						    case ChannelMode::COLOR:
							    r = sample(pr);
							    g = pg == pr ? r : sample(pg);
							    b = pb == pr ? r : sample(pb);
							    break;
						    case ChannelMode::RED:
							    r = g = b = sample(pr);
							    break;
						    case ChannelMode::GREEN:
							    r = g = b = sample(pg);
							    break;
						    case ChannelMode::BLUE:
							    r = g = b = sample(pb);
							    break;
						    case ChannelMode::ALPHA:
							    r = g = b = pa ? sample(pa) : 1.0f;
							    break;
						    case ChannelMode::LUMA:
						    {
							    float lr = sample(pr);
							    float lg = pg == pr ? lr : sample(pg);
							    float lb = pb == pr ? lr : sample(pb);
							    r = g = b =
							        0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
							    break;
						    }
					    }
					    if(ocio)
					    {
						    r *= gain;
						    g *= gain;
						    b *= gain;
						    ocio->apply(r, g, b);
					    }
					    uint8_t* px = dst + static_cast<size_t>(ox) * 4;
					    if(bad && disp.m_showNonFinite)
					    {
						    const uint8_t* c = (bad & 1) ? NAN_RGB : INF_RGB;
						    px[0] = c[0];
						    px[1] = c[1];
						    px[2] = c[2];
					    }
					    else
					    {
						    px[0] = lut(r);
						    px[1] = lut(g);
						    px[2] = lut(b);
					    }
					    px[3] = 255;
				    }
			    }
		    });
	};
	const bool allHalf =
	    std::ranges::all_of(shown,
	                        [](const Plane* p) { return !p || p->isHalf(); });
	const bool allFloat =
	    std::ranges::all_of(shown,
	                        [](const Plane* p) { return !p || !p->isHalf(); });
	if(allHalf)
	{
		auto h = [](const Plane* p) { return p ? p->halfData() : nullptr; };
		paint(h(shown[0]), h(shown[1]), h(shown[2]), h(shown[3]));
	}
	else if(allFloat)
	{
		auto f = [](const Plane* p) { return p ? p->floatData() : nullptr; };
		paint(f(shown[0]), f(shown[1]), f(shown[2]), f(shown[3]));
	}
	else
	{
		// Mixed (e.g. half RGB with a float alpha; rare): widen the half
		// ones for this render. Same plane twice stays one pointer.
		std::vector<std::vector<float>> widened;
		widened.reserve(4);
		const float* f[4] = {};
		for(int c = 0; c < 4; ++c)
		{
			const Plane* p = shown[c];
			if(!p)
			{
				continue;
			}
			const int same =
			    static_cast<int>(std::find(shown, shown + c, p) - shown);
			if(same < c)
			{
				f[c] = f[same];
			}
			else if(p->isHalf())
			{
				auto& w = widened.emplace_back(p->size());
				for(size_t i = 0; i < w.size(); ++i)
				{
					w[i] = (*p)[i];
				}
				f[c] = w.data();
			}
			else
			{
				f[c] = p->floatData();
			}
		}
		paint(f[0], f[1], f[2], f[3]);
	}
	// Inside the frame but outside the data window: black, not transparent,
	// so the frame's extent shows whether or not outlines are drawn.
	{
		const Box2i& fr = img.m_displayWindow;
		auto toX = [&](double ix)
		{
			return static_cast<int>(std::floor((ix - m.m_originX) * m.m_scale));
		};
		auto toY = [&](double iy)
		{
			return static_cast<int>(std::floor((iy - m.m_originY) * m.m_scale));
		};
		const int x0 = std::max(0, toX(fr.m_x0));
		const int x1 = std::min(out.m_width, toX(fr.m_x1 + 1.0));
		const int y0 = std::max(0, toY(fr.m_y0));
		const int y1 = std::min(out.m_height, toY(fr.m_y1 + 1.0));
		for(int y = y0; y < y1; ++y)
		{
			uint8_t* row =
			    &out.m_pixels[static_cast<size_t>(y) * out.m_width * 4];
			for(int x = x0; x < x1; ++x)
			{
				row[x * 4 + 3] = 255; // RGB already 0 where nothing was drawn
			}
		}
	}
	if(m.m_scale >= PIXEL_GRID_MIN_SCALE)
	{
		drawPixelGrid(out, m);
	}
	// Where pixels exist (dashed, only when it differs), then the frame
	// (solid, on top where the two share a side).
	const Box2i& frame = img.m_displayWindow;
	const Box2i& data = img.m_dataWindow;
	const bool differs = data.m_x0 != frame.m_x0 || data.m_y0 != frame.m_y0 ||
	                     data.m_x1 != frame.m_x1 || data.m_y1 != frame.m_y1;
	if(differs && disp.m_outlines == Outlines::FRAME_AND_DATA)
	{
		drawBoxOutline(out, m, data, DATA_OUTLINE_RGB, true);
	}
	if(disp.m_outlines != Outlines::NONE)
	{
		const uint8_t grey[3] = {OUTLINE_GREY, OUTLINE_GREY, OUTLINE_GREY};
		drawBoxOutline(out, m, frame, grey, false);
	}
	if(disp.m_selected) // inside the frame: tiles fill their box with it
	{
		drawBoxOutline(out, m, frame, SELECTED_RGB, true, true);
	}
	if(disp.m_marker)
	{
		drawMarker(out, m, disp.m_marker->first, disp.m_marker->second);
	}
	return out;
}

} // namespace rega
