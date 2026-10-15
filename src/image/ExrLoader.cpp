#include "image/Loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

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
#include <ImfTimeCodeAttribute.h>
#include <ImfVecAttribute.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace rv
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

/// After a whole-image read failed: read again one chunk-high strip at a
/// time, keeping the strips that decode. A render still being written, or a
/// copy cut short, then shows what it has; one only just opened, nothing.
void readWhatIsThere(Imf::InputPart& part, LayerImage& img)
{
	const Imf::Header& h = part.header();
	const Imath::Box2i dw = h.dataWindow();
	const int lines = h.hasTileDescription()
	                      ? static_cast<int>(h.tileDescription().ySize)
	                      : Imf::getCompressionNumScanlines(h.compression());
	int read = 0, total = 0;
	for(int y = dw.min.y; y <= dw.max.y; y += lines, ++total)
	{
		try
		{
			part.readPixels(y, std::min(y + lines - 1, dw.max.y));
			++read;
		}
		catch(const std::exception&)
		{
		}
	}
	img.m_chunksRead = read;
	img.m_chunks = total;
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
		Imf::InputPart part(file, l.m_part);
		const Imf::Header& h = part.header();
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
		part.setFrameBuffer(fb);
		try
		{
			part.readPixels(dw.min.y, dw.max.y);
		}
		catch(const std::exception&)
		{
			readWhatIsThere(part, img);
		}
		return img;
	}
	catch(const std::exception& e)
	{
		throw LoadError(e.what());
	}
}

} // namespace rv
