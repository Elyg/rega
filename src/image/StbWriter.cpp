#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "image/StbWriter.h"

#include <stb_image_write.h>

namespace rega
{

std::string encodeJpeg(const Rgba8Image& img, int quality)
{
	std::string out;
	out.reserve(img.m_pixels.size() / 8);
	stbi_write_jpg_to_func(
	    [](void* ctx, void* data, int size)
	    {
		    static_cast<std::string*>(ctx)->append(static_cast<char*>(data),
		                                           static_cast<size_t>(size));
	    },
	    &out,
	    img.m_width,
	    img.m_height,
	    4,
	    img.m_pixels.data(),
	    quality);
	return out;
}

} // namespace rega
