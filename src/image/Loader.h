#pragma once

#include "image/ImageBuffer.h"

#include <filesystem>
#include <stdexcept>

namespace rv
{

/// Probe or decode failure; what() says why.
struct LoadError : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

/// .exr, .png, .jpg, .jpeg, .tif or .tiff (any case).
bool isSupportedImage(const std::filesystem::path& p);

/// Read headers / layer structure only. Throws LoadError.
ImageInfo probeImage(const std::filesystem::path& p);

/// Decode one layer (index into ImageInfo::layers) to float planes. Throws
/// LoadError.
LayerImage loadLayer(const ImageInfo& info, int layerIndex);

/// Format-specific entry points (used by the dispatcher above).
ImageInfo probeExr(const std::filesystem::path& p);
LayerImage loadExrLayer(const ImageInfo& info, int layerIndex);
ImageInfo probeStb(const std::filesystem::path& p);
LayerImage loadStbLayer(const ImageInfo& info, int layerIndex);
ImageInfo probeTiff(const std::filesystem::path& p);
LayerImage loadTiffLayer(const ImageInfo& info, int layerIndex);

/// sRGB EOTF, 0-1 code to linear: how display-referred (integer) files are
/// decoded, so the default sRGB view round-trips them.
float srgbToLinear(float v);

} // namespace rv
