#include "image/Loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace rv
{

namespace
{
std::string lowerExt(const std::filesystem::path& p)
{
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(), ::tolower);
	return e;
}
} // namespace

float srgbToLinear(float v)
{
	return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

bool isSupportedImage(const std::filesystem::path& p)
{
	auto e = lowerExt(p);
	return e == ".exr" || e == ".png" || e == ".jpg" || e == ".jpeg" ||
	       e == ".tif" || e == ".tiff";
}

ImageInfo probeImage(const std::filesystem::path& p)
{
	const auto e = lowerExt(p);
	if(e == ".exr")
	{
		return probeExr(p);
	}
	return e == ".tif" || e == ".tiff" ? probeTiff(p) : probeStb(p);
}

LayerImage loadLayer(const ImageInfo& info, int layerIndex)
{
	if(layerIndex < 0 || layerIndex >= static_cast<int>(info.m_layers.size()))
	{
		throw LoadError("layer index out of range");
	}
	LayerImage img = info.m_format == "exr"    ? loadExrLayer(info, layerIndex)
	                 : info.m_format == "tiff" ? loadTiffLayer(info, layerIndex)
	                                           : loadStbLayer(info, layerIndex);
	scanLayer(img);
	return img;
}

} // namespace rv
