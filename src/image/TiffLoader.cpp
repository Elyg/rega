#include "image/Loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <tiffio.h>

namespace rega
{

namespace
{

// libtiff reports through callbacks, by default to stderr (under the TUI).
// Each file keeps its last error for LoadError; warnings go to the log.
class TiffFile
{
public:
	explicit TiffFile(const std::string& path) : m_path(path)
	{
		std::unique_ptr<TIFFOpenOptions, void (*)(TIFFOpenOptions*)>
		    opts(TIFFOpenOptionsAlloc(), TIFFOpenOptionsFree);
		TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), onError, this);
		TIFFOpenOptionsSetWarningHandlerExtR(opts.get(), onWarning, this);
		m_tif = TIFFOpenExt(path.c_str(), "r", opts.get());
		if(!m_tif)
		{
			throw LoadError(error("cannot read TIFF"));
		}
	}
	~TiffFile()
	{
		TIFFClose(m_tif);
	}
	TiffFile(const TiffFile&) = delete;
	TiffFile& operator=(const TiffFile&) = delete;

	TIFF* get() const
	{
		return m_tif;
	}
	const std::string& path() const
	{
		return m_path;
	}
	/// `what`, plus libtiff's last error when there is one.
	std::string error(const std::string& what) const
	{
		return m_error.empty() ? what : what + ": " + m_error;
	}

private:
	static std::string format(const char* fmt, va_list ap)
	{
		char buf[512];
		std::vsnprintf(buf, sizeof buf, fmt, ap);
		return buf;
	}
	static int
	onError(TIFF*, void* self, const char* module, const char* fmt, va_list ap)
	{
		auto* f = static_cast<TiffFile*>(self);
		f->m_error = format(fmt, ap);
		spdlog::debug("{}: {}: {}", f->m_path, module, f->m_error);
		return 1;
	}
	static int onWarning(
	    TIFF*, void* self, const char* module, const char* fmt, va_list ap)
	{
		spdlog::debug("{}: {}: {}",
		              static_cast<TiffFile*>(self)->m_path,
		              module,
		              format(fmt, ap));
		return 1;
	}

	std::string m_path;
	std::string m_error;
	TIFF* m_tif = nullptr;
};

enum class Kind
{
	U8,
	U16,
	U32,
	F16,
	F32,
	F64,
	RGBA8 ///< anything else libtiff can turn into 8-bit RGBA (palette, CMYK..)
};

/// The current directory's layout: what is read, and how.
struct Page
{
	uint32_t m_width = 0, m_height = 0;
	uint16_t m_samples = 1, m_bits = 1, m_format = SAMPLEFORMAT_UINT;
	uint16_t m_photometric = PHOTOMETRIC_MINISBLACK;
	uint16_t m_planar = PLANARCONFIG_CONTIG;
	uint16_t m_orientation = ORIENTATION_TOPLEFT;
	Kind m_kind = Kind::RGBA8;
	int m_colour = 3; ///< leading colour samples: 1 grey, 3 RGB; then extras
	std::vector<std::string> m_channels, m_types;
};

std::string photometricName(uint16_t p)
{
	switch(p)
	{
		case PHOTOMETRIC_MINISWHITE:
			return "minisWhite";
		case PHOTOMETRIC_MINISBLACK:
			return "minisBlack";
		case PHOTOMETRIC_RGB:
			return "rgb";
		case PHOTOMETRIC_PALETTE:
			return "palette";
		case PHOTOMETRIC_SEPARATED:
			return "separated";
		case PHOTOMETRIC_YCBCR:
			return "ycbcr";
		case PHOTOMETRIC_CIELAB:
			return "cielab";
		default:
			return std::to_string(p);
	}
}

std::string codecName(uint16_t compression)
{
	const TIFFCodec* c = TIFFFindCODEC(compression);
	std::string n = c ? c->name : "compression " + std::to_string(compression);
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	return n;
}

/// The current directory's layout. Throws LoadError when it cannot be shown.
Page readPage(const TiffFile& f)
{
	TIFF* tif = f.get();
	Page pg;
	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &pg.m_width);
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &pg.m_height);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &pg.m_samples);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &pg.m_bits);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &pg.m_format);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &pg.m_planar);
	TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &pg.m_orientation);
	if(!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &pg.m_photometric))
	{
		pg.m_photometric =
		    pg.m_samples < 3 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB;
	}
	uint16_t extraCount = 0;
	uint16_t* extraTypes = nullptr;
	TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extraCount, &extraTypes);
	if(pg.m_width == 0 || pg.m_height == 0)
	{
		throw LoadError("empty TIFF page");
	}

	const bool grey = pg.m_photometric == PHOTOMETRIC_MINISBLACK ||
	                  pg.m_photometric == PHOTOMETRIC_MINISWHITE;
	const bool rgb = pg.m_photometric == PHOTOMETRIC_RGB && pg.m_samples >= 3;
	const int b = pg.m_bits;
	if(grey || rgb)
	{
		if(pg.m_format == SAMPLEFORMAT_UINT)
		{
			pg.m_kind = b == 8    ? Kind::U8
			            : b == 16 ? Kind::U16
			            : b == 32 ? Kind::U32
			                      : Kind::RGBA8;
		}
		else if(pg.m_format == SAMPLEFORMAT_IEEEFP)
		{
			pg.m_kind = b == 16   ? Kind::F16
			            : b == 32 ? Kind::F32
			            : b == 64 ? Kind::F64
			                      : Kind::RGBA8;
		}
	}

	if(pg.m_kind != Kind::RGBA8)
	{
		pg.m_colour = grey ? 1 : 3;
		static const char* const COLOUR[] = {"R", "G", "B"};
		for(int c = 0; c < pg.m_colour; ++c)
		{
			pg.m_channels.emplace_back(grey ? "Y" : COLOUR[c]);
		}
		// The first extra sample is alpha when it says so, or when it is the
		// only one (writers often leave it unspecified).
		const int extras = pg.m_samples - pg.m_colour;
		for(int e = 0; e < extras; ++e)
		{
			const uint16_t t =
			    e < extraCount ? extraTypes[e] : EXTRASAMPLE_UNSPECIFIED;
			const bool alpha =
			    e == 0 && (t != EXTRASAMPLE_UNSPECIFIED || extras == 1);
			pg.m_channels.push_back(alpha ? "A"
			                              : "extra" + std::to_string(e + 1));
		}
		const char* type = pg.m_format == SAMPLEFORMAT_IEEEFP
		                       ? (b == 16   ? "half"
		                          : b == 32 ? "float"
		                                    : "double")
		                       : (b == 8    ? "uint8"
		                          : b == 16 ? "uint16"
		                                    : "uint32");
		pg.m_types.assign(pg.m_channels.size(), type);
		return pg;
	}

	char why[1024] = "";
	if(pg.m_format == SAMPLEFORMAT_IEEEFP || !TIFFRGBAImageOK(tif, why))
	{
		throw LoadError(
		    fmt::format("unsupported TIFF page ({}, {}-bit {}){}{}",
		                photometricName(pg.m_photometric),
		                b,
		                pg.m_format == SAMPLEFORMAT_IEEEFP ? "float" : "int",
		                *why ? ": " : "",
		                why));
	}
	pg.m_colour = 3;
	pg.m_channels = {"R", "G", "B"};
	if(extraCount > 0)
	{
		pg.m_channels.emplace_back("A");
	}
	pg.m_types.assign(pg.m_channels.size(), "uint8");
	return pg;
}

bool isReduced(TIFF* tif)
{
	uint32_t type = 0;
	return TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &type) &&
	       (type & FILETYPE_REDUCEDIMAGE);
}

std::string stringTag(TIFF* tif, uint32_t tag)
{
	const char* s = nullptr;
	return TIFFGetField(tif, tag, &s) && s ? s : "";
}

/// sRGB-decoded integer codes, built once.
float srgb8(uint8_t v)
{
	static const std::vector<float> TABLE = []
	{
		std::vector<float> t(256);
		for(int i = 0; i < 256; ++i)
		{
			t[i] = srgbToLinear(i / 255.0f);
		}
		return t;
	}();
	return TABLE[v];
}
float srgb16(uint16_t v)
{
	static const std::vector<float> TABLE = []
	{
		std::vector<float> t(65536);
		for(int i = 0; i < 65536; ++i)
		{
			t[i] = srgbToLinear(i / 65535.0f);
		}
		return t;
	}();
	return TABLE[v];
}

template <typename T>
T load(const uint8_t* p)
{
	T v;
	std::memcpy(&v, p, sizeof v);
	return v;
}

/// One stored sample as a float: integers scaled to 0-1 (MinIsWhite
/// inverted), colour through the sRGB EOTF as for PNG; floats as stored.
float sampleValue(const uint8_t* p, Kind kind, bool colour, bool invert)
{
	switch(kind)
	{
		case Kind::U8:
		{
			const uint8_t v = invert ? 255 - *p : *p;
			return colour ? srgb8(v) : v / 255.0f;
		}
		case Kind::U16:
		{
			uint16_t v = load<uint16_t>(p);
			v = invert ? 65535 - v : v;
			return colour ? srgb16(v) : v / 65535.0f;
		}
		case Kind::U32:
		{
			uint32_t v = load<uint32_t>(p);
			v = invert ? 0xffffffffu - v : v;
			const float f = static_cast<float>(v / 4294967295.0);
			return colour ? srgbToLinear(f) : f;
		}
		case Kind::F16:
			return halfToFloat(load<uint16_t>(p));
		case Kind::F32:
			return load<float>(p);
		case Kind::F64:
			return static_cast<float>(load<double>(p));
		case Kind::RGBA8:
			break;
	}
	return 0;
}

/// Strips or tiles, contiguous or one plane per sample: each chunk is decoded
/// by libtiff and spread into the planes. A chunk that fails stays zero.
void readDirect(const TiffFile& f, const Page& pg, LayerImage& img)
{
	TIFF* tif = f.get();
	const bool tiled = TIFFIsTiled(tif);
	uint32_t cw = pg.m_width, ch = pg.m_height;
	if(tiled)
	{
		TIFFGetField(tif, TIFFTAG_TILEWIDTH, &cw);
		TIFFGetField(tif, TIFFTAG_TILELENGTH, &ch);
	}
	else
	{
		TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &ch);
		ch = std::min(ch, pg.m_height);
	}
	if(cw == 0 || ch == 0)
	{
		throw LoadError("TIFF tile or strip size is 0");
	}
	const bool separate = pg.m_planar == PLANARCONFIG_SEPARATE;
	const int planes = separate ? pg.m_samples : 1;
	const int perPixel = separate ? 1 : pg.m_samples;
	const size_t sampleBytes = pg.m_bits / 8;
	const size_t pixelBytes = sampleBytes * perPixel;
	const tmsize_t size = tiled ? TIFFTileSize(tif) : TIFFStripSize(tif);
	if(size <= 0)
	{
		throw LoadError(f.error("bad TIFF chunk size"));
	}
	std::vector<uint8_t> buf(static_cast<size_t>(size));

	const bool invert = pg.m_photometric == PHOTOMETRIC_MINISWHITE;
	const bool flipX = pg.m_orientation == ORIENTATION_TOPRIGHT ||
	                   pg.m_orientation == ORIENTATION_BOTRIGHT;
	const bool flipY = pg.m_orientation == ORIENTATION_BOTRIGHT ||
	                   pg.m_orientation == ORIENTATION_BOTLEFT;
	const uint32_t w = pg.m_width, h = pg.m_height;
	const uint32_t nx = (w + cw - 1) / cw, ny = (h + ch - 1) / ch;
	int read = 0, total = 0;
	for(int pl = 0; pl < planes; ++pl)
	{
		for(uint32_t cy = 0; cy < ny; ++cy)
		{
			for(uint32_t cx = 0; cx < nx; ++cx)
			{
				const uint32_t x0 = cx * cw, y0 = cy * ch;
				++total;
				const tmsize_t n =
				    tiled ? TIFFReadEncodedTile(
				                tif,
				                TIFFComputeTile(tif, x0, y0, 0, pl),
				                buf.data(),
				                size)
				          : TIFFReadEncodedStrip(tif,
				                                 TIFFComputeStrip(tif, y0, pl),
				                                 buf.data(),
				                                 size);
				if(n < 0)
				{
					continue;
				}
				++read;
				const uint32_t x1 = std::min(w, x0 + cw),
				               y1 = std::min(h, y0 + ch);
				for(int s = 0; s < perPixel; ++s)
				{
					const int c = separate ? pl : s;
					const bool colour = c < pg.m_colour;
					Plane& plane = img.m_planes[c];
					for(uint32_t y = y0; y < y1; ++y)
					{
						const uint8_t* row = buf.data() +
						                     (y - y0) * cw * pixelBytes +
						                     s * sampleBytes;
						const size_t dy = flipY ? h - 1 - y : y;
						for(uint32_t x = x0; x < x1; ++x)
						{
							const size_t dx = flipX ? w - 1 - x : x;
							plane.set(dy * w + dx,
							          sampleValue(row + (x - x0) * pixelBytes,
							                      pg.m_kind,
							                      colour,
							                      invert));
						}
					}
				}
			}
		}
	}
	if(read == 0)
	{
		throw LoadError(f.error("cannot decode TIFF"));
	}
	if(read < total)
	{
		img.m_chunksRead = read;
		img.m_chunks = total;
		spdlog::info("{}: {} of {} {} read",
		             f.path(),
		             read,
		             total,
		             tiled ? "tiles" : "strips");
	}
}

/// Palette, CMYK, YCbCr, 1-bit and the like, through libtiff's RGBA reader.
void readRgba(const TiffFile& f, const Page& pg, LayerImage& img)
{
	const size_t n = static_cast<size_t>(pg.m_width) * pg.m_height;
	std::vector<uint32_t> raster(n);
	if(!TIFFReadRGBAImageOriented(f.get(),
	                              pg.m_width,
	                              pg.m_height,
	                              raster.data(),
	                              ORIENTATION_TOPLEFT,
	                              0))
	{
		throw LoadError(f.error("cannot decode TIFF"));
	}
	for(size_t c = 0; c < img.m_planes.size(); ++c)
	{
		Plane& plane = img.m_planes[c];
		for(size_t i = 0; i < n; ++i)
		{
			const uint32_t v = raster[i];
			const uint8_t b = c == 0   ? TIFFGetR(v)
			                  : c == 1 ? TIFFGetG(v)
			                  : c == 2 ? TIFFGetB(v)
			                           : TIFFGetA(v);
			plane.set(i, c < 3 ? srgb8(b) : b / 255.0f);
		}
	}
}

} // namespace

ImageInfo probeTiff(const std::filesystem::path& p)
{
	TiffFile f(p.string());
	TIFF* tif = f.get();
	ImageInfo info;
	info.m_path = p.string();
	info.m_format = "tiff";
	std::string firstError;
	int dir = 0;
	do
	{
		PartInfo part;
		Page pg;
		try
		{
			pg = readPage(f);
		}
		catch(const LoadError& e)
		{
			spdlog::warn("{}: page {}: {}; not shown",
			             info.m_path,
			             dir + 1,
			             e.what());
			firstError = firstError.empty() ? e.what() : firstError;
			info.m_parts.push_back(std::move(part));
			continue;
		}
		const uint32_t w = pg.m_width, h = pg.m_height;
		part.m_name = stringTag(tif, TIFFTAG_PAGENAME);
		part.m_dataWindow = part.m_displayWindow =
		    Box2i{0, 0, static_cast<int>(w) - 1, static_cast<int>(h) - 1};
		part.m_tiled = TIFFIsTiled(tif);
		uint16_t compression = COMPRESSION_NONE;
		TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
		part.m_compression = codecName(compression);
		auto& a = part.m_attributes;
		a.push_back({"size", "V2i", fmt::format("{}x{}", w, h)});
		a.push_back({"components", "int", std::to_string(pg.m_samples)});
		a.push_back({"bitDepth", "int", std::to_string(pg.m_bits)});
		a.push_back({"sampleFormat",
		             "string",
		             pg.m_format == SAMPLEFORMAT_IEEEFP ? "float"
		             : pg.m_format == SAMPLEFORMAT_INT  ? "int"
		                                                : "uint"});
		a.push_back(
		    {"photometric", "string", photometricName(pg.m_photometric)});
		a.push_back(
		    {"planarConfig",
		     "string",
		     pg.m_planar == PLANARCONFIG_SEPARATE ? "separate" : "contig"});
		if(part.m_tiled)
		{
			uint32_t tw = 0, th = 0;
			TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
			TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
			a.push_back({"tileSize", "V2i", fmt::format("{}x{}", tw, th)});
		}
		else
		{
			uint32_t rows = 0;
			TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows);
			a.push_back(
			    {"rowsPerStrip", "int", std::to_string(std::min(rows, h))});
		}
		if(pg.m_orientation != ORIENTATION_TOPLEFT)
		{
			a.push_back(
			    {"orientation", "int", std::to_string(pg.m_orientation)});
		}
		for(auto [tag, name] : {std::pair{TIFFTAG_PAGENAME, "pageName"},
		                        {TIFFTAG_DOCUMENTNAME, "documentName"},
		                        {TIFFTAG_IMAGEDESCRIPTION, "description"},
		                        {TIFFTAG_SOFTWARE, "software"},
		                        {TIFFTAG_DATETIME, "dateTime"},
		                        {TIFFTAG_ARTIST, "artist"},
		                        {TIFFTAG_HOSTCOMPUTER, "hostComputer"},
		                        {TIFFTAG_COPYRIGHT, "copyright"}})
		{
			if(auto s = stringTag(tif, tag); !s.empty())
			{
				a.push_back({name, "string", s});
			}
		}
		// Thumbnails / previews stay in the header list but are not shown.
		if(isReduced(tif))
		{
			a.push_back({"subfileType", "string", "reduced"});
		}
		else
		{
			if(info.m_layers.empty())
			{
				info.m_displayReferred = pg.m_format != SAMPLEFORMAT_IEEEFP;
			}
			LayerInfo layer;
			layer.m_part = dir;
			layer.m_channels = pg.m_channels;
			layer.m_types = pg.m_types;
			info.m_layers.push_back(std::move(layer));
		}
		info.m_parts.push_back(std::move(part));
	} while(++dir, TIFFReadDirectory(tif));

	if(info.m_layers.empty())
	{
		throw LoadError(firstError.empty() ? "no displayable TIFF page"
		                                   : firstError);
	}
	// Pages are told apart by name (their page name if unique, else pageN);
	// a single page is just "rgba", like a PNG.
	if(info.m_layers.size() > 1)
	{
		std::set<std::string> used;
		for(auto& l : info.m_layers)
		{
			std::string& name = info.m_parts[l.m_part].m_name;
			if(name.empty() || !used.insert(name).second)
			{
				name = "page" + std::to_string(l.m_part + 1);
				used.insert(name);
			}
			l.m_partName = name;
		}
	}
	return info;
}

LayerImage loadTiffLayer(const ImageInfo& info, int layerIndex)
{
	const LayerInfo& l = info.m_layers[layerIndex];
	TiffFile f(info.m_path);
	if(!TIFFSetDirectory(f.get(), static_cast<tdir_t>(l.m_part)))
	{
		throw LoadError(
		    f.error("no TIFF page " + std::to_string(l.m_part + 1)));
	}
	const Page pg = readPage(f);
	if(pg.m_channels != l.m_channels)
	{
		throw LoadError("TIFF page changed since it was read");
	}
	LayerImage img;
	img.m_info = l;
	img.m_width = static_cast<int>(pg.m_width);
	img.m_height = static_cast<int>(pg.m_height);
	img.m_dataWindow = img.m_displayWindow =
	    Box2i{0, 0, img.m_width - 1, img.m_height - 1};
	img.m_channelNames = pg.m_channels;
	const size_t n = static_cast<size_t>(img.m_width) * img.m_height;
	for(size_t c = 0; c < pg.m_channels.size(); ++c)
	{
		img.m_planes.push_back(pg.m_kind == Kind::F16 ? Plane::halves(n)
		                                              : Plane::floats(n));
	}
	if(pg.m_kind == Kind::RGBA8)
	{
		readRgba(f, pg, img);
	}
	else
	{
		readDirect(f, pg, img);
	}
	return img;
}

} // namespace rega
