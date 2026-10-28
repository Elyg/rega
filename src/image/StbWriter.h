#pragma once

#include "image/Render.h"

#include <string>

namespace rega
{

/// `img` as a baseline JPEG at `quality` (1-100), stb_image_write. Alpha is
/// dropped: transparent pixels come out as their RGB (black from
/// renderLayer).
std::string encodeJpeg(const Rgba8Image& img, int quality);

} // namespace rega
