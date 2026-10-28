#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "image/Loader.h"
#include "util/Parallel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stb_image.h>

namespace rega
{

// LDR files are display-referred sRGB; linearised (srgbToLinear) so the
// pipeline (exposure etc.) is uniform and the default sRGB view transform
// round-trips to the original pixels.

ImageInfo probeStb(const std::filesystem::path& p)
{
	int w = 0, h = 0, comp = 0;
	if(!stbi_info(p.string().c_str(), &w, &h, &comp))
	{
		throw LoadError(std::string("cannot read image: ") +
		                stbi_failure_reason());
	}
	ImageInfo info;
	info.m_path = p.string();
	std::string ext = p.extension().string();
	info.m_format = (ext == ".png" || ext == ".PNG") ? "png" : "jpeg";
	info.m_displayReferred = true;

	PartInfo part;
	part.m_dataWindow = part.m_displayWindow = Box2i{0, 0, w - 1, h - 1};
	bool is16 = stbi_is_16_bit(p.string().c_str());
	part.m_compression = info.m_format;
	part.m_attributes.push_back(
	    {"size", "V2i", std::to_string(w) + "x" + std::to_string(h)});
	part.m_attributes.push_back({"components", "int", std::to_string(comp)});
	part.m_attributes.push_back({"bitDepth", "int", is16 ? "16" : "8"});
	info.m_parts.push_back(part);

	LayerInfo layer;
	static const char* names[4][4] = {{"Y"},
	                                  {"Y", "A"},
	                                  {"R", "G", "B"},
	                                  {"R", "G", "B", "A"}};
	for(int c = 0; c < comp && c < 4; ++c)
	{
		layer.m_channels.emplace_back(names[comp - 1][c]);
		layer.m_types.emplace_back(is16 ? "uint16" : "uint8");
	}
	info.m_layers.push_back(layer);
	return info;
}

LayerImage loadStbLayer(const ImageInfo& info, int layerIndex)
{
	const auto& path = info.m_path;
	int w = 0, h = 0, comp = 0;
	bool is16 = stbi_is_16_bit(path.c_str());
	std::unique_ptr<void, void (*)(void*)> data(
	    is16 ? static_cast<void*>(stbi_load_16(path.c_str(), &w, &h, &comp, 0))
	         : static_cast<void*>(stbi_load(path.c_str(), &w, &h, &comp, 0)),
	    stbi_image_free);
	if(!data)
	{
		throw LoadError(std::string("cannot decode image: ") +
		                stbi_failure_reason());
	}

	LayerImage img;
	img.m_info = info.m_layers[layerIndex];
	img.m_dataWindow = img.m_displayWindow = Box2i{0, 0, w - 1, h - 1};
	img.m_width = w;
	img.m_height = h;
	const size_t n = static_cast<size_t>(w) * h;
	const float scale = is16 ? 1.0f / 65535.0f : 1.0f / 255.0f;
	// Lookup table for the 8-bit case.
	float lut[256];
	for(int i = 0; i < 256; ++i)
	{
		lut[i] = srgbToLinear(static_cast<float>(i) / 255.0f);
	}

	for(int c = 0; c < comp; ++c)
	{
		img.m_channelNames.push_back(img.m_info.m_channels[c]);
		bool isAlpha = img.m_info.m_channels[c] == "A";
		std::vector<float> plane(n);
		parallelFor(static_cast<int>((n + 65535) / 65536),
		            1,
		            [&](int b, int e)
		            {
			            for(size_t i = static_cast<size_t>(b) * 65536;
			                i < std::min(n, static_cast<size_t>(e) * 65536);
			                ++i)
			            {
				            size_t idx = i * comp + c;
				            if(is16)
				            {
					            float v = static_cast<const uint16_t*>(
					                          data.get())[idx] *
					                      scale;
					            plane[i] = isAlpha ? v : srgbToLinear(v);
				            }
				            else
				            {
					            uint8_t v = static_cast<const uint8_t*>(
					                data.get())[idx];
					            plane[i] = isAlpha ? v * scale : lut[v];
				            }
			            }
		            });
		img.m_planes.emplace_back(std::move(plane));
	}
	return img;
}

} // namespace rega
