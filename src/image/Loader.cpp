#include "image/Loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace rega
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
	       e == ".tif" || e == ".tiff" || isMovie(p);
}

bool isMovie(const std::filesystem::path& p)
{
	const auto e = lowerExt(p);
	return e == ".mov" || e == ".mp4";
}

std::filesystem::path movieFramePath(const std::filesystem::path& file,
                                     int index)
{
	return file.string() + "#" + std::to_string(index);
}

std::pair<std::filesystem::path, int>
splitMovieFrame(const std::filesystem::path& p)
{
	const std::string s = p.string();
	const auto at = s.rfind('#');
	const size_t digits = at == std::string::npos ? 0 : s.size() - at - 1;
	if(digits == 0 || digits > 9 ||
	   !std::all_of(s.begin() + static_cast<std::ptrdiff_t>(at) + 1,
	                s.end(),
	                [](unsigned char c) { return std::isdigit(c); }))
	{
		return {p, 0};
	}
	std::filesystem::path file = s.substr(0, at);
	if(!isMovie(file))
	{
		return {p, 0};
	}
	return {std::move(file), std::stoi(s.substr(at + 1))};
}

ImageInfo probeImage(const std::filesystem::path& p)
{
	const auto [file, frame] = splitMovieFrame(p);
	if(isMovie(file))
	{
		return probeMovie(p);
	}
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
	                 : isMovie(info.m_path) ? loadMovieLayer(info, layerIndex)
	                                        : loadStbLayer(info, layerIndex);
	scanLayer(img);
	return img;
}

} // namespace rega
