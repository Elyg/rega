// libFuzzer harness for the image loaders, built once per format with
// REGA_FUZZ_EXT (".exr", ".tif", ".png", ".jpg", ".mov"). The loaders read
// files by path, so each input is written to one first.
//
//   just fuzz            # every format, 60 s each
//   build/fuzz/rega_fuzz_exr corpus/ -max_total_time=600
#include "image/Loader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace
{

const std::string& inputPath()
{
	static const std::string PATH = []
	{
		spdlog::set_level(spdlog::level::off); // a line per broken file
		return (std::filesystem::temp_directory_path() /
		        ("rega-fuzz-" + std::to_string(getpid()) + REGA_FUZZ_EXT))
		    .string();
	}();
	return PATH;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::string& path = inputPath();
	FILE* f = std::fopen(path.c_str(), "wb");
	if(!f)
	{
		return 0;
	}
	std::fwrite(data, 1, size, f);
	std::fclose(f);
	try
	{
		const rega::ImageInfo info = rega::probeImage(path);
		// A few layers are enough: they share the decoding below them.
		const int layers = std::min(static_cast<int>(info.m_layers.size()), 4);
		for(int l = 0; l < layers; ++l)
		{
			try
			{
				(void)rega::loadLayer(info, l);
			}
			catch(const rega::LoadError&)
			{
			}
		}
	}
	catch(const rega::LoadError&)
	{
	}
	return 0;
}
