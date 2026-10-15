#include "image/ImageService.h"
#include "image/Loader.h"
#include "image/Render.h"

#include <Imath/half.h>
#include <gtest/gtest.h>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTiledOutputFile.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace
{

fs::path tmpDir()
{
	auto d = fs::temp_directory_path() / "rvtui-tests";
	fs::create_directories(d);
	return d;
}

// Value written for channel `c` at (x, y), so reads can be verified exactly.
float pattern(int c, int x, int y)
{
	return c * 100.0f + y * 10.0f + x * 0.5f;
}

// Write a scanline EXR with the given channel names (FLOAT) and a data window
// offset.
fs::path writeExr(const std::string& name,
                  const std::vector<std::string>& channels,
                  int w,
                  int h,
                  bool tiled = false)
{
	fs::path p = tmpDir() / name;
	Imf::Header header(w, h);
	for(auto& c : channels)
	{
		header.channels().insert(c, Imf::Channel(Imf::FLOAT));
	}
	std::vector<std::vector<float>> planes(channels.size(),
	                                       std::vector<float>(w * h));
	Imf::FrameBuffer fb;
	for(size_t c = 0; c < channels.size(); ++c)
	{
		for(int y = 0; y < h; ++y)
		{
			for(int x = 0; x < w; ++x)
			{
				planes[c][y * w + x] = pattern(static_cast<int>(c), x, y);
			}
		}
		fb.insert(channels[c],
		          Imf::Slice(Imf::FLOAT,
		                     reinterpret_cast<char*>(planes[c].data()),
		                     sizeof(float),
		                     sizeof(float) * w));
	}
	if(tiled)
	{
		header.setTileDescription(Imf::TileDescription(8, 8, Imf::ONE_LEVEL));
		Imf::TiledOutputFile out(p.string().c_str(), header);
		out.setFrameBuffer(fb);
		out.writeTiles(0, out.numXTiles() - 1, 0, out.numYTiles() - 1);
	}
	else
	{
		Imf::OutputFile out(p.string().c_str(), header);
		out.setFrameBuffer(fb);
		out.writePixels(h);
	}
	return p;
}

} // namespace

TEST(ExrLoader, GroupsLayersRootFirst)
{
	auto p = writeExr("layers.exr",
	                  {"R",
	                   "G",
	                   "B",
	                   "A",
	                   "diffuse.R",
	                   "diffuse.G",
	                   "diffuse.B",
	                   "depth.Z",
	                   "N.X",
	                   "N.Y",
	                   "N.Z"},
	                  16,
	                  8);
	auto info = rv::probeImage(p);
	ASSERT_EQ(info.m_layers.size(), 4u);
	EXPECT_EQ(info.m_layers[0].m_layer, "");
	EXPECT_EQ(info.m_layers[0].label(), "rgba");
	EXPECT_EQ(info.m_layers[0].m_channels,
	          (std::vector<std::string>{"R", "G", "B", "A"}));
	EXPECT_GE(info.findLayer("diffuse"), 0);
	EXPECT_GE(info.findLayer("depth"), 0);
	auto n = info.m_layers[info.findLayer("N")];
	EXPECT_EQ(n.m_channels, (std::vector<std::string>{"N.X", "N.Y", "N.Z"}));
}

TEST(ExrLoader, ReadsLayerPixelsExactly)
{
	auto p = writeExr("pixels.exr",
	                  {"R", "G", "B", "spec.R", "spec.G", "spec.B"},
	                  12,
	                  6);
	auto info = rv::probeImage(p);
	int li = info.findLayer("spec");
	ASSERT_GE(li, 0);
	auto img = rv::loadLayer(info, li);
	ASSERT_EQ(img.m_width, 12);
	ASSERT_EQ(img.m_height, 6);
	ASSERT_EQ(img.m_channelNames, (std::vector<std::string>{"R", "G", "B"}));
	// ChannelList is sorted: B,G,R,spec.B,spec.G,spec.R → writer indices 3,4,5
	// for spec.* were written as R=3, G=4, B=5 by insertion order.
	EXPECT_FLOAT_EQ(*img.at(0, 3, 2), pattern(3, 3, 2));
	EXPECT_FLOAT_EQ(*img.at(2, 11, 5), pattern(5, 11, 5));
	EXPECT_FALSE(img.at(0, 12, 0).has_value());
}

TEST(ExrLoader, ReadsTiled)
{
	auto p = writeExr("tiled.exr", {"R", "G", "B"}, 20, 13, /*tiled=*/true);
	auto info = rv::probeImage(p);
	EXPECT_TRUE(info.m_parts[0].m_tiled);
	auto img = rv::loadLayer(info, 0);
	EXPECT_FLOAT_EQ(*img.at(1, 19, 12), pattern(1, 19, 12));
}

namespace
{

// Where the offset table of a single-part file with `chunks` chunks starts:
// its first entry points just past it.
size_t offsetTable(const std::string& bytes, int chunks)
{
	for(size_t i = 8; i + 8 <= bytes.size(); ++i)
	{
		uint64_t v;
		std::memcpy(&v, bytes.data() + i, 8);
		if(v == i + 8 * chunks)
		{
			return i;
		}
	}
	return 0;
}

std::string readBytes(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), {}};
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
	std::ofstream(p, std::ios::binary) << bytes;
}

} // namespace

TEST(ExrLoader, ReadsWhatACutShortCopyHas)
{
	// 40 lines of ZIP are three 16-line chunks; lose the end of the last.
	auto p = writeExr("cut.exr", {"R", "G", "B"}, 8, 40);
	std::string bytes = readBytes(p);
	writeBytes(p, bytes.substr(0, bytes.size() - 10));
	auto img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_TRUE(img.incomplete());
	EXPECT_EQ(img.m_chunksRead, 2);
	EXPECT_EQ(img.m_chunks, 3);
	EXPECT_FLOAT_EQ(*img.at(0, 3, 31), pattern(0, 3, 31));
	EXPECT_FLOAT_EQ(*img.at(0, 3, 32), 0.0f);
}

TEST(ExrLoader, ReadsARenderStillBeingWritten)
{
	// The writer zeroes the offset table and fills it in on close.
	auto p = writeExr("rendering.exr", {"R", "G", "B"}, 8, 40);
	std::string bytes = readBytes(p);
	const size_t table = offsetTable(bytes, 3);
	ASSERT_GT(table, 0u);
	std::fill_n(bytes.begin() + table, 8 * 3, '\0');
	writeBytes(p, bytes.substr(0, bytes.size() - 10));
	auto img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_EQ(img.m_chunksRead, 2);
	EXPECT_FLOAT_EQ(*img.at(2, 7, 20), pattern(2, 7, 20));

	// Only just opened: no chunks at all is an empty frame, not an error.
	writeBytes(p, bytes.substr(0, table + 8 * 3));
	img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_EQ(img.m_chunksRead, 0);
	EXPECT_EQ(img.m_chunks, 3);
	EXPECT_FLOAT_EQ(*img.at(0, 0, 0), 0.0f);
}

TEST(ExrLoader, ReadsTheTilesACutShortTiledFileHas)
{
	// 20x13 in 8x8 tiles: 3 x 2. The last one written, (2, 1), loses its end;
	// the others in its row stay.
	auto p = writeExr("cut-tiled.exr", {"R", "G", "B"}, 20, 13, /*tiled=*/true);
	std::string bytes = readBytes(p);
	writeBytes(p, bytes.substr(0, bytes.size() - 10));
	auto img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_EQ(img.m_chunksRead, 5);
	EXPECT_EQ(img.m_chunks, 6);
	EXPECT_FLOAT_EQ(*img.at(1, 19, 7), pattern(1, 19, 7));
	EXPECT_FLOAT_EQ(*img.at(1, 12, 10), pattern(1, 12, 10)); // tile (1, 1)
	EXPECT_FLOAT_EQ(*img.at(1, 19, 12), 0.0f);               // tile (2, 1)
}

TEST(ExrLoader, ReadsTheBucketsOfARenderStillGoing)
{
	// A bucket renderer killed mid-frame: its tile table still all zeros,
	// the first row of buckets on disk.
	auto p = writeExr("buckets.exr", {"R", "G", "B"}, 20, 13, /*tiled=*/true);
	std::string bytes = readBytes(p);
	const size_t table = offsetTable(bytes, 6);
	ASSERT_GT(table, 0u);
	uint64_t fourth;
	std::memcpy(&fourth, bytes.data() + table + 3 * 8, 8);
	std::fill_n(bytes.begin() + table, 6 * 8, '\0');
	writeBytes(p, bytes.substr(0, fourth));
	auto img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_EQ(img.m_chunksRead, 3);
	EXPECT_EQ(img.m_chunks, 6);
	EXPECT_FLOAT_EQ(*img.at(2, 19, 0), pattern(2, 19, 0));
	EXPECT_FLOAT_EQ(*img.at(2, 3, 10), 0.0f);
}

TEST(ExrLoader, MultiPart)
{
	fs::path p = tmpDir() / "multipart.exr";
	const int w = 4, h = 4;
	std::vector<Imf::Header> headers(2, Imf::Header(w, h));
	headers[0].setName("beauty");
	headers[0].channels().insert("R", Imf::Channel(Imf::FLOAT));
	headers[1].setName("aovs");
	headers[1].channels().insert("emission.R", Imf::Channel(Imf::FLOAT));
	headers[1].channels().insert("emission.G", Imf::Channel(Imf::FLOAT));
	for(auto& hd : headers)
	{
		hd.setType(Imf::SCANLINEIMAGE);
	}
	{
		Imf::MultiPartOutputFile out(p.string().c_str(), headers.data(), 2);
		std::vector<float> buf(w * h, 7.0f);
		for(int part = 0; part < 2; ++part)
		{
			Imf::OutputPart op(out, part);
			Imf::FrameBuffer fb;
			for(auto it = headers[part].channels().begin();
			    it != headers[part].channels().end();
			    ++it)
			{
				fb.insert(it.name(),
				          Imf::Slice(Imf::FLOAT,
				                     reinterpret_cast<char*>(buf.data()),
				                     sizeof(float),
				                     sizeof(float) * w));
			}
			op.setFrameBuffer(fb);
			op.writePixels(h);
		}
	}
	auto info = rv::probeImage(p);
	ASSERT_EQ(info.m_parts.size(), 2u);
	ASSERT_EQ(info.m_layers.size(), 2u);
	EXPECT_EQ(info.m_layers[0].label(), "beauty");
	EXPECT_EQ(info.m_layers[1].label(), "aovs/emission");
	auto img = rv::loadLayer(info, 1);
	EXPECT_FLOAT_EQ(*img.at(1, 2, 2), 7.0f);
}

TEST(ChannelMap, Mapping)
{
	auto m = rv::mapChannels({"R", "G", "B", "A"});
	EXPECT_EQ(m.m_r, 0);
	EXPECT_EQ(m.m_b, 2);
	EXPECT_EQ(m.m_a, 3);
	auto z = rv::mapChannels({"Z"});
	EXPECT_EQ(z.m_r, 0);
	EXPECT_EQ(z.m_g, 0);
	EXPECT_EQ(z.m_b, 0);
	auto xy = rv::mapChannels({"X", "Y", "Z"});
	EXPECT_EQ(xy.m_r, 0);
	EXPECT_EQ(xy.m_g, 1);
	EXPECT_EQ(xy.m_b, 2);
}

TEST(Display, ExposureAndSrgb)
{
	rv::DisplayParams p;
	p.m_srgb = false;
	EXPECT_FLOAT_EQ(rv::applyDisplay(0.25f, p), 0.25f);
	p.m_exposure = 1;
	EXPECT_FLOAT_EQ(rv::applyDisplay(0.25f, p), 0.5f);
	p.m_exposure = 0;
	p.m_srgb = true;
	EXPECT_NEAR(rv::applyDisplay(0.18f, p), 0.4613f, 1e-3);
	EXPECT_FLOAT_EQ(rv::applyDisplay(5.0f, p), 1.0f);
	EXPECT_FLOAT_EQ(rv::applyDisplay(-1.0f, p), 0.0f);
	EXPECT_FLOAT_EQ(rv::applyDisplay(NAN, p), 0.0f);
}

TEST(Render, FitLetterboxesAndAverages)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 1}; // 4x2
	img.m_width = 4;
	img.m_height = 2;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(8, 1.0f)));
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_outlines = rv::Outlines::FRAME_AND_DATA; // off by default
	// 4x2 image fit into 4x6 → image rows 2..3, the display-window outline
	// on rows 1 and 4, transparent letterbox beyond.
	auto out = rv::renderLayer(img, 4, 6, {}, d);
	auto alpha = [&](int x, int y)
	{ return out.m_pixels[(y * 4 + x) * 4 + 3]; };
	EXPECT_EQ(alpha(0, 0), 0);
	EXPECT_EQ(out.m_pixels[(1 * 4 + 1) * 4 + 0], rv::OUTLINE_GREY);
	EXPECT_EQ(alpha(0, 2), 255);
	EXPECT_EQ(alpha(3, 3), 255);
	EXPECT_EQ(out.m_pixels[(2 * 4 + 1) * 4 + 0], 255);
	EXPECT_EQ(alpha(0, 5), 0);

	auto half = rv::downsample(img, 2);
	EXPECT_EQ(half.m_width, 2);
	EXPECT_EQ(half.m_reduce, 2);
	EXPECT_FLOAT_EQ(*half.at(0, 3, 1), 1.0f);
}

TEST(Render, PixelGridOnlyWhenZoomedIn)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 1, 1}; // 2x2
	img.m_width = img.m_height = 2;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(4, 1.0f)));
	rv::DisplayParams d;
	d.m_srgb = false;
	auto red = [](const rv::Rgba8Image& o, int x, int y)
	{ return o.m_pixels[(y * o.m_width + x) * 4]; };

	rv::ViewParams v;
	v.m_fit = false;
	v.m_zoom = 8; // each image pixel → 8x8 output px, centred
	v.m_centerX = v.m_centerY = 1;
	auto out = rv::renderLayer(img, 16, 16, v, d);
	EXPECT_EQ(red(out, 4, 4), 255); // inside a pixel
	EXPECT_LT(red(out, 8, 4), 255); // the x = 1 boundary
	EXPECT_LT(red(out, 4, 8), 255); // the y = 1 boundary

	v.m_zoom = 4; // below the threshold: no grid
	out = rv::renderLayer(img, 16, 16, v, d);
	EXPECT_EQ(red(out, 8, 4), 255);
}

TEST(Render, OutlinesTheDisplayWindow)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 1, 1}; // 2x2
	img.m_width = img.m_height = 2;
	img.m_channelNames = {"R", "G", "B", "A"};
	img.m_planes.assign(4,
	                    rv::Plane(
	                        std::vector<float>(4, 0.0f))); // fully transparent
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_outlines = rv::Outlines::FRAME_AND_DATA; // off by default
	// Fit 2x2 into 8x4: scale 2, image at x 2..5, outline at x 1 and 6.
	auto out = rv::renderLayer(img, 8, 4, {}, d);
	auto px = [&](int x, int y) { return &out.m_pixels[(y * 8 + x) * 4]; };
	EXPECT_EQ(px(1, 1)[3], 255);
	EXPECT_EQ(px(1, 1)[0], rv::OUTLINE_GREY);
	EXPECT_EQ(px(6, 2)[0], rv::OUTLINE_GREY);
	EXPECT_EQ(px(0, 1)[3], 0); // outside the outline: untouched
	EXPECT_EQ(px(3, 1)[0], 0); // the image itself is not painted over
}

TEST(Render, DashesTheDataWindowWhenItDiffers)
{
	rv::LayerImage img;
	img.m_displayWindow = {0, 0, 7, 7}; // 8x8 frame
	img.m_dataWindow = {2, 2, 5, 5};    // 4x4 of pixels in the middle
	img.m_width = img.m_height = 4;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(16, 1.0f)));
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_outlines = rv::Outlines::FRAME_AND_DATA; // off by default
	rv::ViewParams v;
	v.m_fit = false;
	v.m_zoom = 4; // 4 output px per image px
	v.m_centerX = v.m_centerY = 4;
	auto out = rv::renderLayer(img, 40, 40, v, d);
	auto px = [&](int x, int y) { return &out.m_pixels[(y * 40 + x) * 4]; };
	// Frame starts at output 4; data at 4 + 2*4 = 12, its outline at 11.
	EXPECT_EQ(px(11, 13)[1], rv::DATA_OUTLINE_RGB[1]); // dash on
	EXPECT_NE(px(11, 17)[1], rv::DATA_OUTLINE_RGB[1]); // dash gap
	EXPECT_EQ(px(11, 17)[3], 255); // inside the frame: black, not clear
	EXPECT_EQ(px(3, 10)[0], rv::OUTLINE_GREY); // the frame
	EXPECT_EQ(px(13, 13)[0], 255);             // pixels untouched

	d.m_outlines = rv::Outlines::NONE; // `w` → off
	out = rv::renderLayer(img, 40, 40, v, d);
	EXPECT_EQ(px(11, 13)[1], 0); // no dash, just the black frame
	EXPECT_EQ(px(3, 10)[3], 0);  // outside the frame stays clear
}

TEST(Render, FitIncludesOverscan)
{
	rv::LayerImage img;
	img.m_displayWindow = {0, 0, 3, 3}; // 4x4 frame
	img.m_dataWindow = {2, 0, 7, 3};    // pixels run 4 past the right edge
	img.m_width = 6;
	img.m_height = 4;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(24, 1.0f)));
	EXPECT_EQ(img.fitBounds().m_x1, 7);
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_outlines = rv::Outlines::NONE;
	// Fit 8x4 of image into 16x8: scale 2, the rightmost pixel lands inside.
	auto out = rv::renderLayer(img, 16, 8, {}, d);
	EXPECT_EQ(out.m_pixels[(4 * 16 + 15) * 4 + 0], 255);
	// Frame left of the data: black (the frame), not transparent.
	EXPECT_EQ(out.m_pixels[(4 * 16 + 0) * 4 + 3], 255);
	EXPECT_EQ(out.m_pixels[(4 * 16 + 0) * 4 + 0], 0);
}

TEST(Render, SelectedTileGetsADashedInsetFrame)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 3}; // 4x4
	img.m_width = img.m_height = 4;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(16, 0.0f)));
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_selected = true;
	auto out = rv::renderLayer(img, 16, 16, {}, d); // fit: frame fills it
	auto px = [&](int x, int y) { return &out.m_pixels[(y * 16 + x) * 4]; };
	EXPECT_EQ(px(1, 0)[0], rv::SELECTED_RGB[0]); // on the edge, dash on
	EXPECT_EQ(px(5, 0)[0], 0);                   // dash off
	EXPECT_EQ(px(8, 8)[0], 0);                   // the image itself
}

TEST(Render, RingsTheMarkedPixelInRed)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 3}; // 4x4
	img.m_width = img.m_height = 4;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(16, 0.0f)));
	rv::DisplayParams d;
	d.m_srgb = false;
	d.m_marker = std::pair(1, 1);
	rv::ViewParams v;
	v.m_fit = false;
	v.m_zoom = 8; // pixel (1, 1) covers output 8..15
	v.m_centerX = v.m_centerY = 2;
	auto out = rv::renderLayer(img, 32, 32, v, d);
	auto red = [&](int x, int y) { return out.m_pixels[(y * 32 + x) * 4]; };
	EXPECT_EQ(red(7, 10), rv::MARKER_RGB[0]); // just outside it
	EXPECT_EQ(red(16, 10), rv::MARKER_RGB[0]);
	EXPECT_EQ(red(10, 7), rv::MARKER_RGB[0]);
	EXPECT_EQ(red(10, 10), 0); // the pixel itself shows, no dot
	EXPECT_EQ(red(11, 11), 0);
	EXPECT_EQ(red(6, 10), 0);

	// Zoomed out (fit into 8x8: the pixel is output 2..3), the ring grows
	// about it to show, and a dot marks the pixel in its middle.
	out = rv::renderLayer(img, 8, 8, {}, d);
	auto red8 = [&](int x, int y) { return out.m_pixels[(y * 8 + x) * 4]; };
	EXPECT_EQ(red8(6, 3), rv::MARKER_RGB[0]); // the ring
	EXPECT_EQ(red8(3, 3), rv::MARKER_RGB[0]); // the dot
	EXPECT_EQ(red8(2, 2), 0);                 // between them
}

TEST(LayerImage, ScanCountsNanAndInfAndTakesFiniteStats)
{
	rv::LayerImage img;
	img.m_width = 4;
	img.m_height = 2;
	rv::Plane r = rv::Plane::floats(8), g = rv::Plane::halves(8);
	r.set(0, std::nanf(""));
	g.set(0, INFINITY); // same pixel: counts as both
	r.set(3, -INFINITY);
	g.set(5, std::nanf(""));
	g.set(6, std::nanf(""));
	r.set(6, std::nanf("")); // NaN in two channels: one pixel
	img.m_planes = {r, g};
	rv::scanLayer(img);
	EXPECT_EQ(img.m_nanPixels, 3);
	EXPECT_EQ(img.m_infPixels, 2);
	// Stats over the finite values only: r is 0 but for 5 of its 8.
	ASSERT_EQ(img.m_stats.size(), 2u);
	EXPECT_EQ(img.m_stats[0].m_count, 5);
	EXPECT_EQ(img.m_stats[0].m_min, 0.0f);
	EXPECT_EQ(img.m_stats[0].m_max, 0.0f);
	EXPECT_EQ(img.m_stats[1].m_count, 5);
	// A reduced copy keeps the full-resolution counts.
	const auto half = rv::downsample(img, 2);
	EXPECT_EQ(half.m_nanPixels, 3);
	EXPECT_EQ(half.m_infPixels, 2);
	EXPECT_EQ(half.m_stats.size(), 2u);
	// Loading counts too.
	auto p = writeExr("finite.exr", {"R", "G", "B"}, 4, 4);
	img = rv::loadLayer(rv::probeImage(p), 0);
	EXPECT_EQ(img.m_nanPixels, 0);
	EXPECT_EQ(img.m_infPixels, 0);
	ASSERT_EQ(img.m_stats.size(), 3u);
	EXPECT_EQ(img.m_stats[0].m_count, 16);
	float lo = INFINITY, hi = -INFINITY;
	double sum = 0;
	for(int y = 0; y < 4; ++y)
	{
		for(int x = 0; x < 4; ++x)
		{
			const float v = *img.at(0, x, y);
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			sum += v;
		}
	}
	EXPECT_EQ(img.m_stats[0].m_min, lo);
	EXPECT_EQ(img.m_stats[0].m_max, hi);
	EXPECT_NEAR(img.m_stats[0].m_mean, sum / 16, 1e-6);
}

TEST(Render, BangPaintsNanAndInfPixels)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 0}; // 4x1
	img.m_width = 4;
	img.m_height = 1;
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, rv::Plane(std::vector<float>(4, 0.5f)));
	img.m_planes[0].set(1, std::nanf(""));
	img.m_planes[2].set(2, -INFINITY);
	rv::DisplayParams d;
	d.m_srgb = false;
	rv::ViewParams v;
	v.m_fit = false;
	v.m_zoom = 1;
	v.m_centerX = 2;
	v.m_centerY = 0.5;
	auto px = [](const rv::Rgba8Image& o, int x)
	{
		const uint8_t* p = &o.m_pixels[static_cast<size_t>(x) * 4];
		return std::array<int, 3>{p[0], p[1], p[2]};
	};
	auto out = rv::renderLayer(img, 4, 1, v, d); // off: NaN / inf as 0
	EXPECT_EQ(px(out, 1)[0], 0);
	EXPECT_EQ(px(out, 2)[2], 0);
	d.m_showNonFinite = true;
	out = rv::renderLayer(img, 4, 1, v, d);
	EXPECT_EQ(px(out, 0), (std::array<int, 3>{128, 128, 128}));
	EXPECT_EQ(px(out, 1), (std::array<int, 3>{255, 0, 255})); // NaN
	EXPECT_EQ(px(out, 2), (std::array<int, 3>{0, 255, 255})); // inf
	// Averaged when zoomed out: one NaN among its pixels still shows.
	v.m_zoom = 0.5;
	out = rv::renderLayer(img, 2, 1, v, d);
	EXPECT_EQ(px(out, 0), (std::array<int, 3>{255, 0, 255}));
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

TEST(StbLoader, PngRoundTripsThroughSrgbView)
{
	const int w = 3, h = 2;
	uint8_t px[w * h * 4];
	for(int i = 0; i < w * h * 4; ++i)
	{
		px[i] = static_cast<uint8_t>(i * 10);
	}
	fs::path p = tmpDir() / "rt.png";
	ASSERT_TRUE(stbi_write_png(p.string().c_str(), w, h, 4, px, w * 4));
	auto info = rv::probeImage(p);
	EXPECT_EQ(info.m_format, "png");
	EXPECT_TRUE(info.m_displayReferred);
	ASSERT_EQ(info.m_layers[0].m_channels,
	          (std::vector<std::string>{"R", "G", "B", "A"}));
	auto img = rv::loadLayer(info, 0);
	// Alpha stays linear; colour is linearised and the default sRGB view
	// restores it.
	EXPECT_FLOAT_EQ(*img.at(3, 1, 0), px[7] / 255.0f);
	rv::DisplayParams d;
	for(int c = 0; c < 3; ++c)
	{
		float back = rv::applyDisplay(*img.at(c, 2, 1), d) * 255.0f;
		EXPECT_NEAR(back, px[(1 * w + 2) * 4 + c], 0.51f);
	}
}

TEST(ImageService, ReloadsFileChangedOnDisk)
{
	auto p = writeExr("changing.exr", {"R", "G", "B"}, 8, 4);
	rv::ImageService svc(nullptr, size_t(64) << 20, 1);
	auto waitInfo = [&]
	{
		for(int i = 0; i < 500; ++i)
		{
			if(auto info = svc.info(p))
			{
				return info;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return rv::ImageInfoPtr{};
	};
	auto before = waitInfo();
	ASSERT_TRUE(before);
	EXPECT_EQ(before->displayWindow().width(), 8);
	EXPECT_FALSE(svc.checkForChanges({p})); // untouched

	// Overwrite with a different image (size differs, so the stamp does too).
	writeExr("changing.exr", {"R", "G", "B", "A"}, 16, 6);
	EXPECT_TRUE(svc.checkForChanges({p}));
	auto after = waitInfo();
	ASSERT_TRUE(after);
	EXPECT_EQ(after->displayWindow().width(), 16);
	EXPECT_EQ(after->m_layers[0].m_channels.size(), 4u);
}

TEST(ImageService, DemotedFramesGoFirstSoALoopKeepsSome)
{
	// Five 64x64 RGB float frames (48 KiB each) in room for three: a loop
	// bigger than the cache.
	std::vector<fs::path> frames;
	for(int i = 0; i < 5; ++i)
	{
		frames.push_back(writeExr("loop." + std::to_string(i) + ".exr",
		                          {"R", "G", "B"},
		                          64,
		                          64));
	}
	const size_t frame = 64 * 64 * 3 * sizeof(float);
	// Misses in the second loop, playing each frame then (maybe) demoting it.
	auto secondLoopMisses = [&](bool demote)
	{
		rv::ImageService svc(nullptr, 3 * frame + 16 * 4096, 1);
		int misses = 0;
		for(int loop = 0; loop < 2; ++loop)
		{
			for(const auto& p : frames)
			{
				misses += loop == 1 && !svc.hasLayer(p, "rgba", 1);
				for(int i = 0; i < 500 && !svc.layer(p, "rgba", 1); ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				if(demote)
				{
					svc.demote(p, "rgba", 1);
				}
			}
		}
		return misses;
	};
	// Plain LRU evicts each frame just before it comes round again.
	EXPECT_EQ(secondLoopMisses(false), 5);
	EXPECT_LT(secondLoopMisses(true), 5);
}

TEST(ImageService, RetriesAfterBrokenFileIsFixed)
{
	fs::path p = tmpDir() / "half-written.exr";
	{
		std::ofstream(p) << "not an exr yet";
	}
	rv::ImageService svc(nullptr, size_t(64) << 20, 1);
	for(int i = 0; i < 500 && !svc.error(p); ++i)
	{
		svc.info(p);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(svc.error(p).has_value());

	writeExr("half-written.exr", {"R", "G", "B"}, 4, 4); // render finished
	EXPECT_TRUE(svc.checkForChanges({p}));
	EXPECT_FALSE(svc.error(p).has_value());
	rv::ImageInfoPtr info;
	for(int i = 0; i < 500 && !info; ++i)
	{
		info = svc.info(p);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(info);
}

namespace
{

// R, G, B as HALF and depth.Z as FLOAT, each pixel x + y / 8 (exact in half).
fs::path writeMixedExr(const std::string& name, int w, int h)
{
	fs::path p = tmpDir() / name;
	Imf::Header header(w, h);
	for(const char* c : {"R", "G", "B"})
	{
		header.channels().insert(c, Imf::Channel(Imf::HALF));
	}
	header.channels().insert("depth.Z", Imf::Channel(Imf::FLOAT));
	std::vector<half> rgb(static_cast<size_t>(w) * h);
	std::vector<float> z(rgb.size());
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			rgb[y * w + x] = half(x + y / 8.0f);
			z[y * w + x] = 1000.0f + x + y / 1024.0f; // needs float
		}
	}
	Imf::FrameBuffer fb;
	for(const char* c : {"R", "G", "B"})
	{
		fb.insert(c,
		          Imf::Slice(Imf::HALF,
		                     reinterpret_cast<char*>(rgb.data()),
		                     sizeof(half),
		                     sizeof(half) * w));
	}
	fb.insert("depth.Z",
	          Imf::Slice(Imf::FLOAT,
	                     reinterpret_cast<char*>(z.data()),
	                     sizeof(float),
	                     sizeof(float) * w));
	Imf::OutputFile out(p.string().c_str(), header);
	out.setFrameBuffer(fb);
	out.writePixels(h);
	return p;
}

} // namespace

TEST(ExrLoader, KeepsHalfChannelsHalfAndFloatOnesFloat)
{
	auto info = rv::probeImage(writeMixedExr("mixed.exr", 8, 4));
	auto rgb = rv::loadLayer(info, info.findLayer(""));
	ASSERT_EQ(rgb.m_planes.size(), 3u);
	for(const auto& p : rgb.m_planes)
	{
		EXPECT_TRUE(p.isHalf());
	}
	EXPECT_EQ(rgb.bytes(), 3u * 8 * 4 * sizeof(uint16_t)); // half the memory
	EXPECT_FLOAT_EQ(*rgb.at(0, 5, 3), 5 + 3 / 8.0f);       // lossless

	auto z = rv::loadLayer(info, info.findLayer("depth"));
	ASSERT_EQ(z.m_planes.size(), 1u);
	EXPECT_FALSE(z.m_planes[0].isHalf());
	EXPECT_FLOAT_EQ(*z.at(0, 7, 3), 1000.0f + 7 + 3 / 1024.0f);
}

TEST(Plane, DownsampleKeepsTheStorageType)
{
	auto info = rv::probeImage(writeMixedExr("mixed-down.exr", 8, 4));
	auto img = rv::downsample(rv::loadLayer(info, info.findLayer("")), 2);
	ASSERT_EQ(img.m_width, 4);
	EXPECT_TRUE(img.m_planes[0].isHalf());
	// Pixels (2..3, 0..1): mean of x + y/8 = 2.5 + 0.0625.
	EXPECT_FLOAT_EQ(*img.at(0, 2, 0), 2.5f + 0.0625f);
}

TEST(Render, HalfAndMixedPlanesRenderLikeFloat)
{
	auto make = [](bool halfRgb, bool halfA)
	{
		rv::LayerImage img;
		img.m_width = 6;
		img.m_height = 4;
		img.m_dataWindow = img.m_displayWindow = {0, 0, 5, 3};
		img.m_channelNames = {"R", "G", "B", "A"};
		for(int c = 0; c < 4; ++c)
		{
			const bool h = c < 3 ? halfRgb : halfA;
			rv::Plane p = h ? rv::Plane::halves(24) : rv::Plane::floats(24);
			for(size_t i = 0; i < 24; ++i)
			{
				p.set(i, c == 3 ? 1.0f : (i % 6) / 8.0f + c / 4.0f);
			}
			img.m_planes.push_back(std::move(p));
		}
		return img;
	};
	const auto ref = rv::renderLayer(make(false, false), 3, 2, {}, {});
	EXPECT_EQ(rv::renderLayer(make(true, true), 3, 2, {}, {}).m_pixels,
	          ref.m_pixels);
	EXPECT_EQ(rv::renderLayer(make(true, false), 3, 2, {}, {}).m_pixels,
	          ref.m_pixels);
}

TEST(ImageService, PrefetchesLandQuietly)
{
	auto p = writeExr("quiet.exr", {"R", "G", "B"}, 8, 4);
	std::atomic<int> notified{0};
	rv::ImageService svc([&] { ++notified; }, size_t(64) << 20, 1);
	svc.layer(p, "rgba", 1, rv::ImageService::Priority::PREFETCH);
	for(int i = 0; i < 500 && !svc.hasLayer(p, "rgba", 1); ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(svc.hasLayer(p, "rgba", 1));
	while(svc.pendingJobs() > 0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(notified.load(), 0); // nothing on screen waited for it

	svc.layer(p, "rgba", 2); // NOW: the screen waits
	for(int i = 0; i < 500 && notified == 0; ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	EXPECT_GE(notified.load(), 1);
}

namespace
{

// A layer covering `dw`, every channel `v` (full resolution).
rv::LayerImage flat(rv::Box2i dw, std::vector<std::string> names, float v)
{
	rv::LayerImage img;
	img.m_dataWindow = img.m_displayWindow = dw;
	img.m_width = dw.width();
	img.m_height = dw.height();
	img.m_channelNames = std::move(names);
	for(size_t c = 0; c < img.m_channelNames.size(); ++c)
	{
		rv::Plane p =
		    rv::Plane::floats(static_cast<size_t>(img.m_width) * img.m_height);
		for(size_t i = 0; i < p.size(); ++i)
		{
			p.set(i, v);
		}
		img.m_planes.push_back(std::move(p));
	}
	return img;
}

} // namespace

TEST(Diff, AbsoluteDifferenceOverTheUnionOfWindowsAndChannels)
{
	const auto a = flat({0, 0, 3, 1}, {"R", "G"}, 1.0f);
	const auto b = flat({2, 0, 5, 1}, {"R", "B"}, 0.25f);
	const auto d = rv::diffLayers(a, b);
	EXPECT_EQ(d.m_dataWindow, (rv::Box2i{0, 0, 5, 1}));
	ASSERT_EQ(d.m_channelNames, (std::vector<std::string>{"R", "B", "G"}));
	EXPECT_FLOAT_EQ(*d.at(0, 0, 0), 1.0f);  // only A: against 0
	EXPECT_FLOAT_EQ(*d.at(0, 2, 1), 0.75f); // both
	EXPECT_FLOAT_EQ(*d.at(1, 5, 0), 0.25f); // B's channel, B's pixel
	EXPECT_FLOAT_EQ(*d.at(2, 5, 0), 0.0f);  // A's channel, B's pixel
	EXPECT_FLOAT_EQ(rv::maxValue(d), 1.0f);
}

TEST(Diff, IdenticalIncludingNanIsZeroOneNanStays)
{
	auto a = flat({0, 0, 1, 0}, {"R"}, 0.5f);
	auto b = a;
	a.m_planes[0].set(0, NAN);
	b.m_planes[0].set(0, NAN);
	auto same = rv::diffLayers(a, b);
	EXPECT_FLOAT_EQ(rv::maxValue(same), 0.0f);
	EXPECT_EQ(same.m_nanPixels, 0);
	b.m_planes[0].set(0, 0.5f);
	EXPECT_EQ(rv::diffLayers(a, b).m_nanPixels, 1);
}

TEST(ImageService, DiffsTwoFilesAndReportsAMissingSide)
{
	auto a = writeExr("diff-a.exr", {"R", "G", "B"}, 6, 4);
	auto b = writeExr("diff-b.exr", {"R", "G", "B"}, 6, 4);
	rv::ImageService svc(nullptr, size_t(64) << 20, 1);
	rv::LayerImagePtr d;
	for(int i = 0; i < 500 && !d; ++i)
	{
		d = svc.diffLayer(a, b, "", 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(d);
	EXPECT_FLOAT_EQ(rv::maxValue(*d), 0.0f);

	const fs::path gone = tmpDir() / "diff-gone.exr";
	fs::remove(gone);
	for(int i = 0; i < 500 && !svc.diffError(gone, b); ++i)
	{
		EXPECT_FALSE(svc.diffLayer(gone, b, "", 1));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	EXPECT_TRUE(svc.diffError(gone, b).has_value());
}
