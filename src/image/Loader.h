#pragma once

#include "image/ImageBuffer.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace rega
{

/// Probe or decode failure; what() says why.
struct LoadError : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

/// .exr, .png, .jpg, .jpeg, .tif, .tiff, .mov or .mp4 (any case).
bool isSupportedImage(const std::filesystem::path& p);

/// .mov or .mp4 (any case): one file holding many frames.
bool isMovie(const std::filesystem::path& p);
/// Frame `index` (0-based) of a movie as a path of its own, "clip.mov#41", so
/// that everything going by path treats a movie like a numbered sequence. Only
/// the loaders and ImageService open these; a plain movie path is its frame 0.
std::filesystem::path movieFramePath(const std::filesystem::path& file,
                                     int index);
/// The file behind `p` and the frame in it: (movie, index) for a movie frame's
/// path, (p, 0) for anything else.
std::pair<std::filesystem::path, int>
splitMovieFrame(const std::filesystem::path& p);

/// Throws LoadError unless a `width` x `height` frame of `channels` planes is
/// one rega will allocate: sides positive, at most frameSizeLimit() pixels, and
/// at most four times that across the planes. A header can name any size; the
/// loaders ask before they allocate.
void checkFrameSize(int64_t width, int64_t height, size_t channels);
/// Most pixels in a frame: 1024 megapixels, or $REGA_MAX_MEGAPIXELS.
int64_t frameSizeLimit();

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
ImageInfo probeMovie(const std::filesystem::path& p);
LayerImage loadMovieLayer(const ImageInfo& info, int layerIndex);

/// sRGB EOTF, 0-1 code to linear: how display-referred (integer) files are
/// decoded, so the default sRGB view round-trips them.
float srgbToLinear(float v);

} // namespace rega
