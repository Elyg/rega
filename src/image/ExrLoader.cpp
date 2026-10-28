#include "image/Loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <ImfBoxAttribute.h>
#include <ImfChannelList.h>
#include <ImfChannelListAttribute.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfCompression.h>
#include <ImfCompressionAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfEnvmapAttribute.h>
#include <ImfFloatAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfIntAttribute.h>
#include <ImfKeyCodeAttribute.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfRationalAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfStringVectorAttribute.h>
#include <ImfThreading.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTiledInputPart.h>
#include <ImfTimeCodeAttribute.h>
#include <ImfVecAttribute.h>
#include <algorithm>
#include <libdeflate.h>
#include <map>
#include <mutex>
#include <openexr.h>
#include <thread>

namespace rega
{

namespace
{

void initThreads()
{
	static std::once_flag once;
	std::call_once(once,
	               []
	               {
		               Imf::setGlobalThreadCount(static_cast<int>(
		                   std::max(2u, std::thread::hardware_concurrency())));
	               });
}

Box2i toBox(const Imath::Box2i& b)
{
	return {b.min.x, b.min.y, b.max.x, b.max.y};
}

std::string fmtFloat(double v)
{
	return fmt::format("{:g}", v);
}

std::string pixelTypeName(Imf::PixelType t)
{
	switch(t)
	{
		case Imf::HALF:
			return "half";
		case Imf::FLOAT:
			return "float";
		case Imf::UINT:
			return "uint";
		default:
			return "?";
	}
}

std::string attrValue(const Imf::Attribute& a)
{
	using namespace Imf;
	auto f = fmtFloat;
	if(auto* x = dynamic_cast<const IntAttribute*>(&a))
	{
		return fmt::format("{}", x->value());
	}
	if(auto* x = dynamic_cast<const FloatAttribute*>(&a))
	{
		return f(x->value());
	}
	if(auto* x = dynamic_cast<const DoubleAttribute*>(&a))
	{
		return f(x->value());
	}
	if(auto* x = dynamic_cast<const StringAttribute*>(&a))
	{
		return x->value();
	}
	if(auto* x = dynamic_cast<const StringVectorAttribute*>(&a))
	{
		return fmt::format("{}", fmt::join(x->value(), ", "));
	}
	if(auto* x = dynamic_cast<const Box2iAttribute*>(&a))
	{
		auto b = x->value();
		return fmt::format("[{}, {}]-[{}, {}]  {}x{}",
		                   b.min.x,
		                   b.min.y,
		                   b.max.x,
		                   b.max.y,
		                   b.max.x - b.min.x + 1,
		                   b.max.y - b.min.y + 1);
	}
	if(auto* x = dynamic_cast<const Box2fAttribute*>(&a))
	{
		auto b = x->value();
		return fmt::format("[{}, {}]-[{}, {}]",
		                   f(b.min.x),
		                   f(b.min.y),
		                   f(b.max.x),
		                   f(b.max.y));
	}
	if(auto* x = dynamic_cast<const V2iAttribute*>(&a))
	{
		return fmt::format("[{}, {}]", x->value().x, x->value().y);
	}
	if(auto* x = dynamic_cast<const V2fAttribute*>(&a))
	{
		return fmt::format("[{}, {}]", f(x->value().x), f(x->value().y));
	}
	if(auto* x = dynamic_cast<const V3fAttribute*>(&a))
	{
		return fmt::format("{}, {}, {}",
		                   f(x->value().x),
		                   f(x->value().y),
		                   f(x->value().z));
	}
	if(auto* x = dynamic_cast<const V3iAttribute*>(&a))
	{
		return fmt::format("{}, {}, {}",
		                   x->value().x,
		                   x->value().y,
		                   x->value().z);
	}
	if(auto* x = dynamic_cast<const CompressionAttribute*>(&a))
	{
		std::string name;
		getCompressionNameFromId(x->value(), name);
		return name;
	}
	if(auto* x = dynamic_cast<const LineOrderAttribute*>(&a))
	{
		switch(x->value())
		{
			case INCREASING_Y:
				return "increasing y";
			case DECREASING_Y:
				return "decreasing y";
			default:
				return "random y";
		}
	}
	if(auto* x = dynamic_cast<const ChannelListAttribute*>(&a))
	{
		std::vector<std::string> parts;
		for(auto it = x->value().begin(); it != x->value().end(); ++it)
		{
			parts.push_back(fmt::format("{}({})",
			                            it.name(),
			                            pixelTypeName(it.channel().type)));
		}
		return fmt::format("{}", fmt::join(parts, ", "));
	}
	if(auto* x = dynamic_cast<const ChromaticitiesAttribute*>(&a))
	{
		auto c = x->value();
		return fmt::format("r[{}, {}] g[{}, {}] b[{}, {}] w[{}, {}]",
		                   f(c.red.x),
		                   f(c.red.y),
		                   f(c.green.x),
		                   f(c.green.y),
		                   f(c.blue.x),
		                   f(c.blue.y),
		                   f(c.white.x),
		                   f(c.white.y));
	}
	if(auto* x = dynamic_cast<const RationalAttribute*>(&a))
	{
		return fmt::format("{}/{} ({})",
		                   x->value().n,
		                   x->value().d,
		                   f(double(x->value())));
	}
	if(auto* x = dynamic_cast<const TimeCodeAttribute*>(&a))
	{
		auto t = x->value();
		return fmt::format("{:02}:{:02}:{:02}:{:02}",
		                   t.hours(),
		                   t.minutes(),
		                   t.seconds(),
		                   t.frame());
	}
	if(auto* x = dynamic_cast<const KeyCodeAttribute*>(&a))
	{
		auto k = x->value();
		return fmt::format("{} {} {} {}+{}",
		                   k.filmMfcCode(),
		                   k.filmType(),
		                   k.prefix(),
		                   k.count(),
		                   k.perfOffset());
	}
	if(auto* x = dynamic_cast<const TileDescriptionAttribute*>(&a))
	{
		auto t = x->value();
		return fmt::format("{}x{} {}",
		                   t.xSize,
		                   t.ySize,
		                   t.mode == ONE_LEVEL       ? "one level"
		                   : t.mode == MIPMAP_LEVELS ? "mipmap"
		                                             : "ripmap");
	}
	if(auto* x = dynamic_cast<const EnvmapAttribute*>(&a))
	{
		return x->value() == ENVMAP_LATLONG ? "latlong" : "cube";
	}
	if(auto* x = dynamic_cast<const M44fAttribute*>(&a))
	{
		auto m = x->value();
		std::vector<std::string> rows;
		for(int r = 0; r < 4; ++r)
		{
			rows.push_back(fmt::format("{} {} {} {}",
			                           f(m[r][0]),
			                           f(m[r][1]),
			                           f(m[r][2]),
			                           f(m[r][3])));
		}
		return fmt::format("{}", fmt::join(rows, " | "));
	}
	if(auto* x = dynamic_cast<const M33fAttribute*>(&a))
	{
		auto m = x->value();
		std::vector<std::string> rows;
		for(int r = 0; r < 3; ++r)
		{
			rows.push_back(
			    fmt::format("{} {} {}", f(m[r][0]), f(m[r][1]), f(m[r][2])));
		}
		return fmt::format("{}", fmt::join(rows, " | "));
	}
	return fmt::format("<{}>", a.typeName());
}

// Group a part's channels into layers by the prefix before the last '.'.
void collectLayers(const Imf::Header& h,
                   int part,
                   const std::string& partName,
                   std::vector<LayerInfo>& out)
{
	std::map<std::string, LayerInfo> byLayer;
	std::vector<std::string> order;
	for(auto it = h.channels().begin(); it != h.channels().end(); ++it)
	{
		std::string full = it.name();
		auto dot = full.rfind('.');
		std::string layer = dot == std::string::npos ? "" : full.substr(0, dot);
		auto [li, inserted] = byLayer.try_emplace(layer);
		if(inserted)
		{
			order.push_back(layer);
			li->second.m_part = part;
			li->second.m_partName = partName;
			li->second.m_layer = layer;
		}
		li->second.m_channels.push_back(full);
		li->second.m_types.push_back(pixelTypeName(it.channel().type));
	}
	// Root channels first, then alphabetical (ChannelList iteration is already
	// sorted).
	std::stable_sort(order.begin(),
	                 order.end(),
	                 [](const std::string& a, const std::string& b)
	                 { return a.empty() && !b.empty(); });
	// Order channels within a layer R,G,B,A first, then the rest.
	auto rank = [](const std::string& full)
	{
		static const char* pref[] =
		    {"R", "G", "B", "A", "X", "Y", "Z", "x", "y", "z"};
		auto s = LayerInfo::shortName(full);
		for(int i = 0; i < 10; ++i)
		{
			if(s == pref[i])
			{
				return i;
			}
		}
		return 100;
	};
	for(auto& name : order)
	{
		auto& l = byLayer[name];
		std::vector<size_t> idx(l.m_channels.size());
		for(size_t i = 0; i < idx.size(); ++i)
		{
			idx[i] = i;
		}
		std::stable_sort(idx.begin(),
		                 idx.end(),
		                 [&](size_t a, size_t b)
		                 {
			                 return rank(l.m_channels[a]) <
			                        rank(l.m_channels[b]);
		                 });
		LayerInfo sorted = l;
		for(size_t i = 0; i < idx.size(); ++i)
		{
			sorted.m_channels[i] = l.m_channels[idx[i]];
			sorted.m_types[i] = l.m_types[idx[i]];
		}
		out.push_back(std::move(sorted));
	}
}

/// A ZIP / ZIPS chunk inflated without checking its Adler-32. A bit flipped
/// after writing (a disk, a network copy) fails that check, and OpenEXR 3.4
/// drops the whole chunk where older readers show it. The deflate
/// stream must still give exactly the chunk's bytes. Then undoes what the
/// writer did before deflating, as OpenEXR's internal_exr_undo_zip: bytes
/// delta-coded, then split into two halves.
exr_result_t unzipIgnoringChecksum(exr_decode_pipeline_t* d)
{
	const size_t packed = d->chunk.packed_size;
	const size_t size = d->chunk.unpacked_size;
	if(packed == size)
	{
		return EXR_ERR_SUCCESS; // stored as is: unpacked_buffer is the data
	}
	if(packed < 2)
	{
		return EXR_ERR_CORRUPT_CHUNK;
	}
	struct Decompressor
	{
		libdeflate_decompressor* m_d = libdeflate_alloc_decompressor();
		~Decompressor()
		{
			libdeflate_free_decompressor(m_d);
		}
	};
	thread_local Decompressor dec;
	if(!dec.m_d)
	{
		return EXR_ERR_OUT_OF_MEMORY;
	}
	std::vector<uint8_t> raw(size);
	size_t in = 0, out = 0;
	// Raw deflate past the 2-byte zlib header: the checksum after the
	// stream is never read.
	const auto res =
	    libdeflate_deflate_decompress_ex(dec.m_d,
	                                     static_cast<const uint8_t*>(
	                                         d->packed_buffer) +
	                                         2,
	                                     packed - 2,
	                                     raw.data(),
	                                     size,
	                                     &in,
	                                     &out);
	if(res != LIBDEFLATE_SUCCESS || out != size)
	{
		const auto* label =
		    static_cast<const std::string*>(d->decoding_user_data);
		spdlog::warn("{}: {}; not shown",
		             label ? *label : fmt::format("zip chunk {}", d->chunk.idx),
		             res == LIBDEFLATE_SUCCESS
		                 ? fmt::format("inflates to {} of {} bytes", out, size)
		             : res == LIBDEFLATE_INSUFFICIENT_SPACE
		                 ? fmt::format("inflates to more than {} bytes", size)
		                 : std::string("its deflate data is broken"));
		return EXR_ERR_CORRUPT_CHUNK;
	}
	for(size_t i = 1; i < size; ++i)
	{
		raw[i] = static_cast<uint8_t>(raw[i - 1] + raw[i] - 128);
	}
	auto* dst = static_cast<uint8_t*>(d->unpacked_buffer);
	const size_t second = (size + 1) / 2;
	for(size_t i = 0; i < size; ++i)
	{
		dst[i] = raw[i % 2 ? second + i / 2 : i / 2];
	}
	d->bytes_decompressed = size;
	return EXR_ERR_SUCCESS;
}

/// Reads again, through OpenEXR's core API with unzipIgnoringChecksum, the
/// chunks the C++ API refused, into `img`'s planes. Only ZIP / ZIPS: other
/// codecs have no checksum to skip. Opens the file when first needed.
class Salvage
{
public:
	Salvage(std::string path, int part, const LayerInfo& l, LayerImage& img)
	    : m_path(std::move(path)), m_part(part), m_layer(l), m_img(img)
	{
	}
	~Salvage()
	{
		if(m_ctx)
		{
			exr_finish(&m_ctx);
		}
	}
	Salvage(const Salvage&) = delete;
	Salvage& operator=(const Salvage&) = delete;

	/// Full-resolution tile (tx, ty), its first pixel at (x, y).
	bool tile(int tx, int ty, int x, int y)
	{
		const std::string what = fmt::format("tile ({}, {})", tx, ty);
		exr_chunk_info_t ci;
		return open() &&
		       check(exr_read_tile_chunk_info(m_ctx, m_part, tx, ty, 0, 0, &ci),
		             what,
		             "no chunk") &&
		       decode(ci, x, y, what) && damaged(what);
	}
	/// The strip of scanlines starting at `y`.
	bool strip(int y)
	{
		const std::string what = fmt::format("scanlines from {}", y);
		exr_chunk_info_t ci;
		return open() &&
		       check(exr_read_scanline_chunk_info(m_ctx, m_part, y, &ci),
		             what,
		             "no chunk") &&
		       decode(ci, m_img.m_dataWindow.m_x0, y, what) && damaged(what);
	}

private:
	bool open()
	{
		if(m_ctx || m_failed)
		{
			return m_ctx != nullptr;
		}
		exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
		// Its default prints to stderr, under the TUI.
		init.error_handler_fn =
		    [](exr_const_context_t, exr_result_t, const char*) {};
		exr_compression_t comp = EXR_COMPRESSION_LAST_TYPE;
		m_failed =
		    !check(exr_start_read(&m_ctx, m_path.c_str(), &init),
		           "salvage",
		           "cannot open") ||
		    !check(exr_get_compression(m_ctx, m_part, &comp),
		           "salvage",
		           "no compression") ||
		    (comp != EXR_COMPRESSION_ZIP && comp != EXR_COMPRESSION_ZIPS);
		if(m_failed && comp != EXR_COMPRESSION_LAST_TYPE)
		{
			spdlog::debug("{}: salvage: compression {} is not zip, nothing to "
			              "salvage",
			              m_path,
			              static_cast<int>(comp));
		}
		if(m_failed && m_ctx)
		{
			exr_finish(&m_ctx);
			m_ctx = nullptr;
		}
		return m_ctx != nullptr;
	}

	/// The chunk `ci` into the layer's planes, its first pixel at (x, y).
	bool
	decode(const exr_chunk_info_t& ci, int x, int y, const std::string& what)
	{
		spdlog::debug("{}: {}: {} bytes packed, {} unpacked, {}x{} at {}, {}",
		              m_path,
		              what,
		              ci.packed_size,
		              ci.unpacked_size,
		              ci.width,
		              ci.height,
		              x,
		              y);
		exr_decode_pipeline_t d = EXR_DECODE_PIPELINE_INITIALIZER;
		if(!check(exr_decoding_initialize(m_ctx, m_part, &ci, &d),
		          what,
		          "decode setup"))
		{
			return false;
		}
		const auto& names = m_layer.m_channels;
		for(int i = 0; i < d.channel_count; ++i)
		{
			exr_coding_channel_info_t& ch = d.channels[i];
			auto it = std::find(names.begin(), names.end(), ch.channel_name);
			if(it == names.end() || ch.x_samples != 1 || ch.y_samples != 1)
			{
				continue; // no pointer: skipped
			}
			Plane& p = m_img.m_planes[it - names.begin()];
			const int px = p.isHalf() ? 2 : 4;
			auto* base = p.isHalf() ? reinterpret_cast<uint8_t*>(p.halfData())
			                        : reinterpret_cast<uint8_t*>(p.floatData());
			ch.user_bytes_per_element = static_cast<int16_t>(px);
			ch.user_data_type = p.isHalf() ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT;
			ch.user_pixel_stride = px;
			ch.user_line_stride = px * m_img.m_width;
			ch.decode_to_ptr =
			    base + (static_cast<size_t>(y - m_img.m_dataWindow.m_y0) *
			                m_img.m_width +
			            (x - m_img.m_dataWindow.m_x0)) *
			               px;
		}
		bool ok = check(exr_decoding_choose_default_routines(m_ctx, m_part, &d),
		                what,
		                "decode routines");
		if(ok)
		{
			const std::string label = m_path + ": " + what;
			d.decoding_user_data = const_cast<std::string*>(&label);
			d.decompress_fn = &unzipIgnoringChecksum;
			ok = check(exr_decoding_run(m_ctx, m_part, &d), what, "decode");
		}
		exr_decoding_destroy(m_ctx, &d);
		return ok;
	}

	/// True on success; else logs which step failed and why (debug).
	bool check(exr_result_t rv, const std::string& what, const char* step) const
	{
		if(rv != EXR_ERR_SUCCESS)
		{
			spdlog::debug("{}: {}: {}: {}",
			              m_path,
			              what,
			              step,
			              exr_get_default_error_message(rv));
		}
		return rv == EXR_ERR_SUCCESS;
	}

	bool damaged(const std::string& what)
	{
		++m_img.m_chunksDamaged;
		spdlog::warn("{}: {} fails its zip checksum; shown as it inflates",
		             m_path,
		             what);
		return true;
	}

	std::string m_path;
	int m_part;
	const LayerInfo& m_layer;
	LayerImage& m_img;
	exr_context_t m_ctx = nullptr;
	bool m_failed = false;
};

/// After a whole-image read failed: read again one chunk-high strip at a
/// time, keeping the strips that decode. A render still being written, or a
/// copy cut short, then shows what it has; one only just opened, nothing.
void readWhatIsThere(Imf::InputPart& part, Salvage& salvage, LayerImage& img)
{
	const Imf::Header& h = part.header();
	const Imath::Box2i dw = h.dataWindow();
	const int lines = Imf::getCompressionNumScanlines(h.compression());
	int read = 0, total = 0;
	for(int y = dw.min.y; y <= dw.max.y; y += lines, ++total)
	{
		try
		{
			part.readPixels(y, std::min(y + lines - 1, dw.max.y));
			++read;
		}
		catch(const std::exception& e)
		{
			spdlog::debug("{}: scanlines from {}: {}",
			              part.fileName(),
			              y,
			              e.what());
			read += salvage.strip(y);
		}
	}
	img.m_chunksRead = read;
	img.m_chunks = total;
	spdlog::info("{}: {} of {} chunks read, {} of them damaged",
	             part.fileName(),
	             read,
	             total,
	             img.m_chunksDamaged);
}

/// A tiled part's full-resolution level, tile by tile when the whole of it
/// will not read: a bucket render still going (its tile table not written
/// yet) or a copy cut short shows the buckets it has, wherever they are.
/// The generic InputPart refuses such a file outright.
void readTiles(Imf::MultiPartInputFile& file,
               int part,
               const Imf::FrameBuffer& fb,
               Salvage& salvage,
               LayerImage& img)
{
	Imf::TiledInputPart tiles(file, part);
	tiles.setFrameBuffer(fb);
	const int nx = tiles.numXTiles(0), ny = tiles.numYTiles(0);
	try
	{
		tiles.readTiles(0, nx - 1, 0, ny - 1, 0, 0);
		spdlog::debug("{}: all {} tiles read", tiles.fileName(), nx * ny);
		return;
	}
	catch(const std::exception& e)
	{
		spdlog::debug("{}: whole level: {}; tile by tile",
		              tiles.fileName(),
		              e.what());
	}
	int read = 0;
	for(int y = 0; y < ny; ++y)
	{
		for(int x = 0; x < nx; ++x)
		{
			try
			{
				tiles.readTile(x, y, 0, 0);
				++read;
			}
			catch(const std::exception& e)
			{
				spdlog::debug("{}: tile ({}, {}): {}",
				              tiles.fileName(),
				              x,
				              y,
				              e.what());
				const Imath::Box2i dw = tiles.header().dataWindow();
				read += salvage.tile(x,
				                     y,
				                     dw.min.x + x * tiles.tileXSize(),
				                     dw.min.y + y * tiles.tileYSize());
			}
		}
	}
	img.m_chunksRead = read;
	img.m_chunks = nx * ny;
	spdlog::info("{}: {} of {} tiles read, {} of them damaged",
	             tiles.fileName(),
	             read,
	             img.m_chunks,
	             img.m_chunksDamaged);
}

} // namespace

ImageInfo probeExr(const std::filesystem::path& p)
{
	initThreads();
	try
	{
		Imf::MultiPartInputFile file(p.string().c_str());
		ImageInfo info;
		info.m_path = p.string();
		info.m_format = "exr";
		for(int i = 0; i < file.parts(); ++i)
		{
			const Imf::Header& h = file.header(i);
			PartInfo part;
			part.m_name = h.hasName() ? h.name() : "";
			part.m_dataWindow = toBox(h.dataWindow());
			part.m_displayWindow = toBox(h.displayWindow());
			part.m_tiled = h.hasTileDescription();
			part.m_deep = h.hasType() && Imf::isDeepData(h.type());
			Imf::getCompressionNameFromId(h.compression(), part.m_compression);
			for(auto it = h.begin(); it != h.end(); ++it)
			{
				part.m_attributes.push_back({it.name(),
				                             it.attribute().typeName(),
				                             attrValue(it.attribute())});
			}
			if(!info.m_fps)
			{
				if(auto* fps = h.findTypedAttribute<Imf::RationalAttribute>(
				       "framesPerSecond"))
				{
					info.m_fps = double(fps->value());
				}
			}
			if(!part.m_deep)
			{
				collectLayers(h, i, part.m_name, info.m_layers);
			}
			info.m_parts.push_back(std::move(part));
		}
		if(info.m_layers.empty())
		{
			throw LoadError("no displayable (non-deep) channels");
		}
		return info;
	}
	catch(const LoadError&)
	{
		throw;
	}
	catch(const std::exception& e)
	{
		throw LoadError(e.what());
	}
}

LayerImage loadExrLayer(const ImageInfo& info, int layerIndex)
{
	initThreads();
	const LayerInfo& l = info.m_layers[layerIndex];
	try
	{
		Imf::MultiPartInputFile file(info.m_path.c_str());
		const Imf::Header& h = file.header(l.m_part);
		Imath::Box2i dw = h.dataWindow();

		LayerImage img;
		img.m_info = l;
		img.m_dataWindow = toBox(dw);
		img.m_displayWindow = toBox(h.displayWindow());
		img.m_width = dw.max.x - dw.min.x + 1;
		img.m_height = dw.max.y - dw.min.y + 1;

		Imf::FrameBuffer fb;
		img.m_planes.resize(l.m_channels.size());
		for(size_t c = 0; c < l.m_channels.size(); ++c)
		{
			const Imf::Channel* ch = h.channels().findChannel(l.m_channels[c]);
			int xs = ch ? ch->xSampling : 1, ys = ch ? ch->ySampling : 1;
			img.m_channelNames.push_back(LayerInfo::shortName(l.m_channels[c]));
			// Half stays half (lossless, half the cache); float and uint
			// read as float.
			const bool half = ch && ch->type == Imf::HALF;
			const size_t n = static_cast<size_t>(img.m_width) * img.m_height;
			img.m_planes[c] = half ? Plane::halves(n) : Plane::floats(n);
			if(xs != 1 || ys != 1)
			{
				continue; // sub-sampled channels (rare: luminance/chroma) left
				          // black
			}
			Plane& p = img.m_planes[c];
			const size_t px = half ? sizeof(uint16_t) : sizeof(float);
			fb.insert(l.m_channels[c],
			          Imf::Slice::Make(half ? Imf::HALF : Imf::FLOAT,
			                           half ? static_cast<void*>(p.halfData())
			                                : static_cast<void*>(p.floatData()),
			                           dw,
			                           px,
			                           px * img.m_width));
		}
		std::string comp;
		Imf::getCompressionNameFromId(h.compression(), comp);
		spdlog::debug("{}: layer '{}' of part {}: {}x{}, {}, {}, {} channels",
		              info.m_path,
		              l.label(),
		              l.m_part,
		              img.m_width,
		              img.m_height,
		              h.hasTileDescription() ? "tiled" : "scanline",
		              comp,
		              l.m_channels.size());
		Salvage salvage(info.m_path, l.m_part, l, img);
		if(h.hasTileDescription())
		{
			readTiles(file, l.m_part, fb, salvage, img);
			return img;
		}
		Imf::InputPart part(file, l.m_part);
		part.setFrameBuffer(fb);
		try
		{
			part.readPixels(dw.min.y, dw.max.y);
			spdlog::debug("{}: all scanlines read", info.m_path);
		}
		catch(const std::exception& e)
		{
			spdlog::debug("{}: whole image: {}; strip by strip",
			              info.m_path,
			              e.what());
			readWhatIsThere(part, salvage, img);
		}
		return img;
	}
	catch(const std::exception& e)
	{
		throw LoadError(e.what());
	}
}

} // namespace rega
