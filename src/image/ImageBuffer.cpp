#include "image/ImageBuffer.h"

#include "util/Parallel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>

namespace rv
{

std::string LayerInfo::shortName(const std::string& full)
{
	auto dot = full.rfind('.');
	return dot == std::string::npos ? full : full.substr(dot + 1);
}

std::string LayerInfo::label() const
{
	if(m_layer.empty())
	{
		return m_partName.empty() ? "rgba" : m_partName;
	}
	if(!m_partName.empty() && m_partName != m_layer)
	{
		return m_partName + "/" + m_layer;
	}
	return m_layer;
}

int ImageInfo::findLayer(const std::string& label) const
{
	for(size_t i = 0; i < m_layers.size(); ++i)
	{
		if(m_layers[i].label() == label || m_layers[i].m_layer == label)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

void scanLayer(LayerImage& img)
{
	const size_t planes = img.m_planes.size();
	std::mutex mu;
	int64_t nan = 0, inf = 0;
	std::vector<ChannelStats> stats(planes);
	std::vector<double> sums(planes, 0.0);
	parallelFor(img.m_height,
	            64,
	            [&](int yBegin, int yEnd)
	            {
		            const size_t i0 = static_cast<size_t>(yBegin) * img.m_width;
		            const size_t n =
		                static_cast<size_t>(yEnd - yBegin) * img.m_width;
		            // Per pixel: bit 0 a NaN in some channel, bit 1 an inf. A
		            // plane at a time, so each loop runs over one array of one
		            // type.
		            std::vector<uint8_t> bad(n, 0);
		            std::vector<ChannelStats> st(planes);
		            std::vector<double> sum(planes, 0.0);
		            auto scan = [&](size_t c, auto get)
		            {
			            float lo = INFINITY, hi = -INFINITY;
			            double total = 0;
			            int64_t count = 0;
			            for(size_t k = 0; k < n; ++k)
			            {
				            const float v = get(i0 + k);
				            if(!std::isfinite(v))
				            {
					            bad[k] |= std::isnan(v) ? 1 : 2;
					            continue;
				            }
				            lo = std::min(lo, v);
				            hi = std::max(hi, v);
				            total += v;
				            ++count;
			            }
			            st[c] = {lo, hi, 0, count};
			            sum[c] = total;
		            };
		            for(size_t c = 0; c < planes; ++c)
		            {
			            const Plane& p = img.m_planes[c];
			            if(const uint16_t* h = p.halfData())
			            {
				            scan(c,
				                 [h](size_t i) { return halfToFloat(h[i]); });
			            }
			            else
			            {
				            const float* f = p.floatData();
				            scan(c, [f](size_t i) { return f[i]; });
			            }
		            }
		            int64_t nn = 0, ni = 0;
		            for(uint8_t b : bad)
		            {
			            nn += b & 1;
			            ni += b >> 1;
		            }
		            std::lock_guard lk(mu);
		            nan += nn;
		            inf += ni;
		            for(size_t c = 0; c < planes; ++c)
		            {
			            ChannelStats& to = stats[c];
			            const ChannelStats& from = st[c];
			            if(!from.m_count)
			            {
				            continue;
			            }
			            to.m_min = to.m_count ? std::min(to.m_min, from.m_min)
			                                  : from.m_min;
			            to.m_max = to.m_count ? std::max(to.m_max, from.m_max)
			                                  : from.m_max;
			            to.m_count += from.m_count;
			            sums[c] += sum[c];
		            }
	            });
	for(size_t c = 0; c < planes; ++c)
	{
		if(stats[c].m_count)
		{
			stats[c].m_mean = sums[c] / static_cast<double>(stats[c].m_count);
		}
	}
	img.m_nanPixels = nan;
	img.m_infPixels = inf;
	img.m_stats = std::move(stats);
}

LayerImage downsample(const LayerImage& src, int factor)
{
	if(factor <= 1)
	{
		return src;
	}
	LayerImage out;
	out.m_info = src.m_info;
	out.m_dataWindow = src.m_dataWindow;
	out.m_displayWindow = src.m_displayWindow;
	out.m_channelNames = src.m_channelNames;
	out.m_nanPixels = src.m_nanPixels;
	out.m_infPixels = src.m_infPixels;
	out.m_stats = src.m_stats;
	out.m_chunksRead = src.m_chunksRead;
	out.m_chunks = src.m_chunks;
	out.m_chunksDamaged = src.m_chunksDamaged;
	out.m_reduce = src.m_reduce * factor;
	out.m_width = std::max(1, src.m_width / factor);
	out.m_height = std::max(1, src.m_height / factor);
	const float inv = 1.0f / static_cast<float>(factor * factor);
	for(const auto& plane : src.m_planes)
	{
		const size_t n = static_cast<size_t>(out.m_width) * out.m_height;
		Plane dst = plane.isHalf() ? Plane::halves(n) : Plane::floats(n);
		parallelFor(out.m_height,
		            32,
		            [&](int yBegin, int yEnd)
		            {
			            for(int y = yBegin; y < yEnd; ++y)
			            {
				            for(int x = 0; x < out.m_width; ++x)
				            {
					            float sum = 0;
					            for(int dy = 0; dy < factor; ++dy)
					            {
						            const size_t row =
						                static_cast<size_t>(y * factor + dy) *
						                src.m_width;
						            for(int dx = 0; dx < factor; ++dx)
						            {
							            sum += plane[row + x * factor + dx];
						            }
					            }
					            dst.set(static_cast<size_t>(y) * out.m_width +
					                        x,
					                    sum * inv);
				            }
			            }
		            });
		out.m_planes.push_back(std::move(dst));
	}
	return out;
}

LayerImage diffLayers(const LayerImage& a, const LayerImage& b)
{
	LayerImage out;
	out.m_info = b.m_info;
	out.m_dataWindow = a.m_dataWindow.unite(b.m_dataWindow);
	out.m_displayWindow = b.m_displayWindow;
	out.m_width = out.m_dataWindow.width();
	out.m_height = out.m_dataWindow.height();
	out.m_channelNames = b.m_channelNames;
	for(const auto& n : a.m_channelNames)
	{
		if(std::ranges::find(out.m_channelNames, n) == out.m_channelNames.end())
		{
			out.m_channelNames.push_back(n);
		}
	}
	auto planeOf = [](const LayerImage& img, const std::string& name)
	{
		const auto it = std::ranges::find(img.m_channelNames, name);
		return it == img.m_channelNames.end()
		           ? -1
		           : static_cast<int>(it - img.m_channelNames.begin());
	};
	// A sample of `img` at image (x, y): 0 outside it or without the plane.
	auto sample = [](const LayerImage& img, int c, int x, int y)
	{
		if(c < 0 || !img.m_dataWindow.contains(x, y))
		{
			return 0.0f;
		}
		return img.m_planes[c][static_cast<size_t>(y - img.m_dataWindow.m_y0) *
		                           img.m_width +
		                       (x - img.m_dataWindow.m_x0)];
	};
	const size_t n = static_cast<size_t>(out.m_width) * out.m_height;
	for(const auto& name : out.m_channelNames)
	{
		const int ca = planeOf(a, name), cb = planeOf(b, name);
		Plane dst = Plane::floats(n);
		parallelFor(out.m_height,
		            32,
		            [&](int yBegin, int yEnd)
		            {
			            for(int row = yBegin; row < yEnd; ++row)
			            {
				            const int y = out.m_dataWindow.m_y0 + row;
				            for(int col = 0; col < out.m_width; ++col)
				            {
					            const int x = out.m_dataWindow.m_x0 + col;
					            const float va = sample(a, ca, x, y);
					            const float vb = sample(b, cb, x, y);
					            const bool same = va == vb || (std::isnan(va) &&
					                                           std::isnan(vb));
					            dst.set(static_cast<size_t>(row) * out.m_width +
					                        col,
					                    same ? 0.0f : std::fabs(va - vb));
				            }
			            }
		            });
		out.m_planes.push_back(std::move(dst));
	}
	out.m_chunksRead = a.m_chunksRead + b.m_chunksRead;
	out.m_chunks = a.m_chunks + b.m_chunks;
	out.m_chunksDamaged = a.m_chunksDamaged + b.m_chunksDamaged;
	scanLayer(out);
	return out;
}

float maxValue(const LayerImage& img)
{
	float m = 0;
	for(const auto& s : img.m_stats)
	{
		if(s.m_count)
		{
			m = std::max(m, s.m_max);
		}
	}
	return m;
}

ChannelMap mapChannels(const std::vector<std::string>& names)
{
	auto find = [&](std::initializer_list<const char*> candidates)
	{
		for(const char* c : candidates)
		{
			for(size_t i = 0; i < names.size(); ++i)
			{
				std::string n = names[i];
				if(n == c)
				{
					return static_cast<int>(i);
				}
			}
		}
		for(const char* c : candidates) // case-insensitive second pass
		{
			for(size_t i = 0; i < names.size(); ++i)
			{
				std::string n = names[i], cc = c;
				std::transform(n.begin(), n.end(), n.begin(), ::tolower);
				std::transform(cc.begin(), cc.end(), cc.begin(), ::tolower);
				if(n == cc)
				{
					return static_cast<int>(i);
				}
			}
		}
		return -1;
	};
	ChannelMap m;
	m.m_r = find({"R", "red", "X", "U"});
	m.m_g = find({"G", "green", "Y", "V"});
	m.m_b = find({"B", "blue", "Z", "W"});
	m.m_a = find({"A", "alpha"});
	int present = (m.m_r >= 0) + (m.m_g >= 0) + (m.m_b >= 0);
	if(present == 0)
	{
		// No recognised colour channel: show the first non-alpha channel as grey.
		int first = -1;
		for(size_t i = 0; i < names.size(); ++i)
		{
			if(static_cast<int>(i) != m.m_a)
			{
				first = static_cast<int>(i);
				break;
			}
		}
		m.m_r = m.m_g = m.m_b = first >= 0 ? first : m.m_a;
	}
	else if(present == 1)
	{
		// Single channel (Y luminance, Z depth, ...): grey.
		m.m_r = m.m_g = m.m_b = std::max({m.m_r, m.m_g, m.m_b});
	}
	return m;
}

} // namespace rv
