#pragma once

#include <Imath/half.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// Pixel box with inclusive corners; empty when m_x1 < m_x0.
struct Box2i
{
	int m_x0 = 0, m_y0 = 0, m_x1 = -1,
	    m_y1 = -1; ///< inclusive, like Imath::Box2i
	int width() const
	{
		return m_x1 - m_x0 + 1;
	}
	int height() const
	{
		return m_y1 - m_y0 + 1;
	}
	bool contains(int x, int y) const
	{
		return x >= m_x0 && x <= m_x1 && y >= m_y0 && y <= m_y1;
	}
	bool operator==(const Box2i&) const = default;
	/// Smallest box holding both (an empty box adds nothing).
	Box2i unite(const Box2i& o) const
	{
		if(o.m_x1 < o.m_x0 || o.m_y1 < o.m_y0)
		{
			return *this;
		}
		if(m_x1 < m_x0 || m_y1 < m_y0)
		{
			return o;
		}
		return {std::min(m_x0, o.m_x0),
		        std::min(m_y0, o.m_y0),
		        std::max(m_x1, o.m_x1),
		        std::max(m_y1, o.m_y1)};
	}
};

/// One displayable group of channels: an EXR layer within a part ("" = the root
/// RGBA channels), or the whole image for PNG/JPEG.
struct LayerInfo
{
	int m_part = 0;
	std::string m_partName; ///< EXR multi-part "name" attribute, may be empty
	std::string m_layer;    ///< e.g. "diffuse", "" for root channels
	std::vector<std::string>
	    m_channels;                   ///< full channel names, e.g. "diffuse.R"
	std::vector<std::string> m_types; ///< "half" | "float" | "uint" per channel

	/// Short channel name ("R" for "diffuse.R").
	static std::string shortName(const std::string& full);
	/// Human label: "part/layer" with sensible fallbacks.
	std::string label() const;
};

/// Header attribute, value formatted for display.
struct Attribute
{
	std::string m_name;
	std::string m_type;
	std::string m_value;
};

/// One EXR part's header; PNG/JPEG have one part.
struct PartInfo
{
	std::string m_name;
	Box2i m_dataWindow;
	Box2i m_displayWindow;
	bool m_tiled = false;
	bool m_deep = false;
	std::string m_compression;
	std::vector<Attribute> m_attributes;
};

/// Header-level information, cheap to obtain (no pixel decode).
struct ImageInfo
{
	std::string m_path;
	std::string m_format; ///< "exr" | "png" | "jpeg"
	bool m_displayReferred =
	    false; ///< LDR formats: decoded with sRGB EOTF to linear
	std::vector<PartInfo> m_parts;
	std::vector<LayerInfo> m_layers;
	std::optional<double> m_fps;

	/// The first part's display / data window.
	const Box2i& displayWindow() const
	{
		return m_parts.front().m_displayWindow;
	}
	const Box2i& dataWindow() const
	{
		return m_parts.front().m_dataWindow;
	}
	/// Index of the layer whose label() or layer name is `label`; -1 if none.
	int findLayer(const std::string& label) const;
	/// What `f` fits: the frame plus any pixels outside it (overscan), for the
	/// part holding `label` (the first part if not found).
	Box2i fitBounds(const std::string& label) const
	{
		const int l = findLayer(label);
		const PartInfo& p =
		    m_parts[l >= 0 ? static_cast<size_t>(m_layers[l].m_part) : 0];
		return p.m_displayWindow.unite(p.m_dataWindow);
	}
};

/// Half bits to float: one instruction on ARM64 (Imath does it in software
/// there, a third slower in renderLayer); Imath's table elsewhere.
inline float halfToFloat(uint16_t h) noexcept
{
#if defined(__aarch64__)
	return static_cast<float>(std::bit_cast<__fp16>(h));
#else
	return imath_half_to_float(h);
#endif
}

/// One channel's pixels, kept in the file's precision: 16-bit half for half
/// channels (most plates; half the memory of float, and lossless), 32-bit
/// float for everything else. Reads always give float.
class Plane
{
public:
	Plane() = default;
	explicit Plane(std::vector<float> values) : m_float(std::move(values))
	{
	}
	/// `n` zeroed values of either kind.
	static Plane floats(size_t n)
	{
		return Plane(std::vector<float>(n, 0.0f));
	}
	static Plane halves(size_t n)
	{
		Plane p;
		p.m_half.assign(n, 0); // +0.0
		p.m_isHalf = true;
		return p;
	}

	[[nodiscard]] bool isHalf() const noexcept
	{
		return m_isHalf;
	}
	[[nodiscard]] size_t size() const noexcept
	{
		return m_isHalf ? m_half.size() : m_float.size();
	}
	[[nodiscard]] size_t bytes() const noexcept
	{
		return m_isHalf ? m_half.size() * sizeof(uint16_t)
		                : m_float.size() * sizeof(float);
	}
	float operator[](size_t i) const noexcept
	{
		return m_isHalf ? halfToFloat(m_half[i]) : m_float[i];
	}
	void set(size_t i, float v) noexcept
	{
		if(m_isHalf)
		{
			m_half[i] = imath_float_to_half(v);
		}
		else
		{
			m_float[i] = v;
		}
	}
	/// The raw storage, for decoders filling it (null when the other kind).
	float* floatData() noexcept
	{
		return m_isHalf ? nullptr : m_float.data();
	}
	uint16_t* halfData() noexcept
	{
		return m_isHalf ? m_half.data() : nullptr;
	}
	const float* floatData() const noexcept
	{
		return m_isHalf ? nullptr : m_float.data();
	}
	const uint16_t* halfData() const noexcept
	{
		return m_isHalf ? m_half.data() : nullptr;
	}

private:
	std::vector<float> m_float;
	std::vector<uint16_t> m_half; ///< Imath half bits
	bool m_isHalf = false;
};

/// One channel's finite values (NaN / ±inf left out): range and mean.
struct ChannelStats
{
	float m_min = 0, m_max = 0;
	double m_mean = 0;
	int64_t m_count = 0; ///< finite values; 0 = none, the rest meaningless
};

/// Decoded pixels of one layer, planar (see Plane), covering `dataWindow`.
/// `reduce` > 1 means the planes were box-downsampled by that factor (used for
/// flipbook caching); coordinates in `dataWindow`/`displayWindow` are always
/// full-resolution image space.
struct LayerImage
{
	LayerInfo m_info;
	Box2i m_dataWindow;
	Box2i m_displayWindow;
	int m_reduce = 1;
	int m_width = 0, m_height = 0; ///< plane dimensions (already reduced)
	std::vector<std::string>
	    m_channelNames; ///< short names, same order as planes
	std::vector<Plane> m_planes;
	/// Pixels with a NaN / an ±inf in any channel, and each plane's stats:
	/// from full resolution on load (scanLayer); a reduced copy keeps its
	/// source's.
	int64_t m_nanPixels = 0, m_infPixels = 0;
	std::vector<ChannelStats> m_stats; ///< per plane; empty until scanned
	/// An incomplete file (still rendering, or cut short): of m_chunks strips,
	/// only m_chunksRead decoded; the rest are zero. Both 0 when complete.
	int m_chunksRead = 0, m_chunks = 0;
	bool incomplete() const
	{
		return m_chunksRead < m_chunks;
	}

	Box2i fitBounds() const // frame plus overscan, see ImageInfo
	{
		return m_displayWindow.unite(m_dataWindow);
	}

	/// Sample plane `c` at full-res image coordinates; nullopt outside the data
	/// window.
	std::optional<float> at(int c, int x, int y) const
	{
		if(!m_dataWindow.contains(x, y))
		{
			return std::nullopt;
		}
		int px = (x - m_dataWindow.m_x0) / m_reduce,
		    py = (y - m_dataWindow.m_y0) / m_reduce;
		if(px >= m_width || py >= m_height)
		{
			return std::nullopt;
		}
		return m_planes[c][static_cast<size_t>(py) * m_width + px];
	}
	size_t bytes() const
	{
		size_t n = 0;
		for(const auto& p : m_planes)
		{
			n += p.bytes();
		}
		return n;
	}
};

using LayerImagePtr = std::shared_ptr<const LayerImage>;

/// Fill m_nanPixels / m_infPixels and m_stats from the planes.
void scanLayer(LayerImage& img);

/// Box-downsample by an integer factor (factor 1 returns a copy).
LayerImage downsample(const LayerImage& src, int factor);

/// |a - b| per channel (matched by short name), over the union of the data
/// windows (a pixel or channel one side lacks counts as 0), at full
/// resolution. B's display window; both NaN is no difference, one NaN stays
/// NaN. Scanned: m_stats max is the largest difference. Incomplete when
/// either side is.
LayerImage diffLayers(const LayerImage& a, const LayerImage& b);

/// The largest finite value in any channel of a scanned image (0 if none).
float maxValue(const LayerImage& img);

/// Which planes feed R, G, B, A when showing a layer in colour (-1 = absent).
struct ChannelMap
{
	int m_r = -1, m_g = -1, m_b = -1, m_a = -1;
};
/// R/G/B/A by name (also X/Y/Z, U/V/W); else the first channel as grey.
ChannelMap mapChannels(const std::vector<std::string>& shortNames);

} // namespace rv
