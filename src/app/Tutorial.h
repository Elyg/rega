#pragma once

#include <filesystem>
#include <functional>

namespace rega::tutorial
{

/// What `rega --tutorial` renders.
struct Options
{
	int m_first = 1001; ///< first frame number of the sequence
	int m_frames = 100; ///< sequence length (1001-1100)
	int m_width = 960;  ///< sequence and stills resolution
	int m_height = 540;
};

/// $XDG_CACHE_HOME/rega/tutorial (~/.cache/rega/tutorial).
std::filesystem::path defaultDir();

/// Called after each file is written with (written, total); from any thread,
/// one call at a time.
using Progress = std::function<void(int done, int total)>;

/// Render the tutorial set into `dir` (created when missing), overwriting:
/// shot.####.exr, a multi-layer sequence (beauty, diffuse, specular, albedo,
/// N, Z, mask); wip.####.exr, a render still being written (frames missing,
/// the last one cut short); single-layer stills (colour chart and a changed
/// version to diff, HDR sky, one-channel ramp, overscan); nan-inf.exr, a
/// broken render; and README.txt, what to try.
void write(const std::filesystem::path& dir,
           const Options& opts = {},
           const Progress& progress = {});

/// write() unless `dir` already holds this set: a stamp written last, so an
/// interrupted render starts over. True when it rendered.
bool ensure(const std::filesystem::path& dir,
            const Options& opts = {},
            const Progress& progress = {});

} // namespace rega::tutorial
