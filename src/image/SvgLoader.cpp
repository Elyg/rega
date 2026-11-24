#include "image/Loader.h"
#include "util/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

// nanosvg.h is two headers that each hold their implementation; defined here
// once. They use stdio / string / math without including them for C++.
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg.h>
#include <nanosvgrast.h>

namespace rega
{

// An SVG is drawn once, at load, into an 8-bit RGBA frame and from then on is
// an image like a PNG: display-referred, linearised from sRGB codes. A vector
// file has no pixels of its own, so it is drawn at its size in CSS px, scaled
// up until the longer side is SVG_MIN_SIDE (icons and diagrams are often
// 24 or 300 px wide) and down to at most SVG_MAX_SIDE.

namespace
{

constexpr float SVG_MIN_SIDE = 2048.0f;
constexpr float SVG_MAX_SIDE = 16384.0f;
// Bigger than any drawing anyone means to open; parsed in memory whole.
constexpr std::streamoff SVG_MAX_BYTES = 256LL << 20;
// Rows drawn per task: each task flattens every shape again.
constexpr int SVG_STRIP_ROWS = 256;

using SvgPtr = std::unique_ptr<NSVGimage, void (*)(NSVGimage*)>;

struct Svg
{
	SvgPtr m_image{nullptr, nsvgDelete};
	float m_scale = 1.0f;
	int m_width = 0, m_height = 0; ///< the frame drawn
	int m_shapes = 0;
};

Svg parseSvg(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary | std::ios::ate);
	if(!in)
	{
		throw LoadError("cannot read image: cannot open file");
	}
	const std::streamoff size = in.tellg();
	if(size < 0 || size > SVG_MAX_BYTES)
	{
		throw LoadError("cannot read image: SVG larger than 256 MB");
	}
	std::string text(static_cast<size_t>(size), '\0');
	in.seekg(0);
	if(!in.read(text.data(), size))
	{
		throw LoadError("cannot read image: short read");
	}
	// nanosvg makes an empty drawing of anything at all; say what it is not.
	if(text.find("<svg") == std::string::npos)
	{
		throw LoadError("cannot read image: not an SVG file");
	}

	Svg svg;
	// Parses in place: the text must end in a NUL (std::string's does).
	svg.m_image.reset(nsvgParse(text.data(), "px", 96.0f));
	if(!svg.m_image)
	{
		throw LoadError("cannot decode image: SVG parse failed");
	}
	const float w = svg.m_image->width, h = svg.m_image->height;
	if(!(w > 0.0f && h > 0.0f && std::isfinite(w) && std::isfinite(h)))
	{
		throw LoadError("image has no size");
	}
	const float longer = std::max(w, h);
	svg.m_scale = longer < SVG_MIN_SIDE   ? SVG_MIN_SIDE / longer
	              : longer > SVG_MAX_SIDE ? SVG_MAX_SIDE / longer
	                                      : 1.0f;
	svg.m_width = std::max(1, static_cast<int>(std::ceil(w * svg.m_scale)));
	svg.m_height = std::max(1, static_cast<int>(std::ceil(h * svg.m_scale)));
	for(const NSVGshape* s = svg.m_image->shapes; s; s = s->next)
	{
		++svg.m_shapes;
	}
	return svg;
}

std::string number(float v)
{
	char buf[32];
	std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
	return buf;
}

} // namespace

ImageInfo probeSvg(const std::filesystem::path& p)
{
	const Svg svg = parseSvg(p);
	ImageInfo info;
	info.m_path = p.string();
	info.m_format = "svg";
	info.m_displayReferred = true;

	PartInfo part;
	part.m_dataWindow = part.m_displayWindow =
	    Box2i{0, 0, svg.m_width - 1, svg.m_height - 1};
	part.m_compression = "svg";
	part.m_attributes.push_back(
	    {"documentSize",
	     "V2f",
	     number(svg.m_image->width) + "x" + number(svg.m_image->height)});
	part.m_attributes.push_back(
	    {"size",
	     "V2i",
	     std::to_string(svg.m_width) + "x" + std::to_string(svg.m_height)});
	part.m_attributes.push_back({"scale", "float", number(svg.m_scale)});
	part.m_attributes.push_back(
	    {"shapes", "int", std::to_string(svg.m_shapes)});
	info.m_parts.push_back(part);

	LayerInfo layer;
	for(const char* c : {"R", "G", "B", "A"})
	{
		layer.m_channels.emplace_back(c);
		layer.m_types.emplace_back("uint8");
	}
	info.m_layers.push_back(layer);
	return info;
}

LayerImage loadSvgLayer(const ImageInfo& info, int layerIndex)
{
	const Svg svg = parseSvg(info.m_path);
	const int w = svg.m_width, h = svg.m_height;
	checkFrameSize(w, h, 4);
	const size_t n = static_cast<size_t>(w) * h;
	std::vector<uint8_t> rgba(n * 4);

	// Strips in parallel, each with a rasterizer of its own, shifted up so
	// its first row lands on row 0 of its slice of the frame.
	const int strips = (h + SVG_STRIP_ROWS - 1) / SVG_STRIP_ROWS;
	parallelFor(strips,
	            1,
	            [&](int b, int e)
	            {
		            std::unique_ptr<NSVGrasterizer, void (*)(NSVGrasterizer*)>
		                rast(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
		            if(!rast)
		            {
			            return; // out of memory: the strip stays clear
		            }
		            for(int s = b; s < e; ++s)
		            {
			            const int y0 = s * SVG_STRIP_ROWS;
			            const int rows = std::min(SVG_STRIP_ROWS, h - y0);
			            nsvgRasterize(rast.get(),
			                          svg.m_image.get(),
			                          0.0f,
			                          -static_cast<float>(y0),
			                          svg.m_scale,
			                          rgba.data() +
			                              static_cast<size_t>(y0) * w * 4,
			                          w,
			                          rows,
			                          w * 4);
		            }
	            });

	LayerImage img;
	img.m_info = info.m_layers[layerIndex];
	img.m_dataWindow = img.m_displayWindow = Box2i{0, 0, w - 1, h - 1};
	img.m_width = w;
	img.m_height = h;
	// nanosvg hands back straight alpha, with the colour of a clear pixel
	// bled in from its neighbours. Premultiplied in sRGB codes, as a browser
	// composites, so the viewer's "colour over the backdrop" is right.
	float unit[256];
	for(int i = 0; i < 256; ++i)
	{
		unit[i] = static_cast<float>(i) / 255.0f;
	}
	std::vector<float> planes[4];
	for(auto& plane : planes)
	{
		plane.resize(n);
	}
	parallelFor(static_cast<int>((n + 65535) / 65536),
	            1,
	            [&](int b, int e)
	            {
		            for(size_t i = static_cast<size_t>(b) * 65536;
		                i < std::min(n, static_cast<size_t>(e) * 65536);
		                ++i)
		            {
			            const uint8_t* px = rgba.data() + i * 4;
			            const float a = unit[px[3]];
			            for(int c = 0; c < 3; ++c)
			            {
				            planes[c][i] = srgbToLinear(unit[px[c]] * a);
			            }
			            planes[3][i] = a;
		            }
	            });
	for(int c = 0; c < 4; ++c)
	{
		img.m_channelNames.push_back(img.m_info.m_channels[c]);
		img.m_planes.emplace_back(std::move(planes[c]));
	}
	return img;
}

} // namespace rega
