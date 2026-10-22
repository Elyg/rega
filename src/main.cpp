#include "BuildInfo.h"
#include "app/App.h"
#include "app/HelpKeys.h"
#include "app/Tutorial.h"
#include "image/Loader.h"
#include "image/Render.h"
#include "image/Sequence.h"
#include "term/Caps.h"
#include "term/Doctor.h"
#include "term/Kitty.h"
#include "term/Output.h"
#include "util/Log.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int dump(const fs::path& path)
{
	auto& out = rv::log::out();
	rv::ImageInfo info = rv::probeImage(path);
	out.info("{}  [{}]", info.m_path, info.m_format);
	if(info.m_fps)
	{
		out.info("fps: {}", *info.m_fps);
	}
	for(size_t i = 0; i < info.m_parts.size(); ++i)
	{
		const auto& p = info.m_parts[i];
		out.info("\npart {}{}{}{}",
		         i,
		         p.m_name.empty() ? "" : fmt::format(" \"{}\"", p.m_name),
		         p.m_tiled ? " tiled" : "",
		         p.m_deep ? " deep" : "");
		for(const auto& a : p.m_attributes)
		{
			out.info("  {} ({}): {}", a.m_name, a.m_type, a.m_value);
		}
	}
	out.info("\nlayers:");
	for(const auto& l : info.m_layers)
	{
		std::string chans;
		for(size_t c = 0; c < l.m_channels.size(); ++c)
		{
			chans += fmt::format(" {}({})",
			                     rv::LayerInfo::shortName(l.m_channels[c]),
			                     l.m_types[c]);
		}
		out.info("  {}:{}", l.label(), chans);
	}
	return 0;
}

// Time each pipeline stage on one file (median of a few runs). For tuning, not
// for users: hidden from --help.
int bench(const fs::path& path, int outW, int outH)
{
	using Clock = std::chrono::steady_clock;
	auto& out = rv::log::out();
	auto ms = [](Clock::time_point a, Clock::time_point b)
	{ return std::chrono::duration<double, std::milli>(b - a).count(); };
	auto median = [](std::vector<double> v)
	{
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};
	std::vector<double> tProbe, tDecode, tDown, tRender, tZlib, tB64;
	rv::Rgba8Image bmp;
	for(int run = 0; run < 5; ++run)
	{
		auto t0 = Clock::now();
		rv::ImageInfo info = rv::probeImage(path);
		auto t1 = Clock::now();
		rv::LayerImage img = rv::loadLayer(info, 0);
		auto t2 = Clock::now();
		rv::LayerImage half = rv::downsample(img, 4);
		auto t3 = Clock::now();
		bmp = rv::renderLayer(img, outW, outH, {}, {});
		auto t4 = Clock::now();
		std::string z =
		    rv::kitty::zlibCompress(bmp.m_pixels.data(), bmp.m_pixels.size());
		auto t5 = Clock::now();
		std::string b =
		    rv::kitty::base64(reinterpret_cast<const uint8_t*>(z.data()),
		                      z.size());
		auto t6 = Clock::now();
		tProbe.push_back(ms(t0, t1));
		tDecode.push_back(ms(t1, t2));
		tDown.push_back(ms(t2, t3));
		tRender.push_back(ms(t3, t4));
		tZlib.push_back(ms(t4, t5));
		tB64.push_back(ms(t5, t6));
	}
	out.info("{}  → {}x{} output", path.string(), outW, outH);
	out.info("  probe        {:8.2f} ms", median(tProbe));
	out.info("  decode       {:8.2f} ms", median(tDecode));
	out.info("  downsample/4 {:8.2f} ms", median(tDown));
	out.info("  render       {:8.2f} ms", median(tRender));
	out.info("  zlib         {:8.2f} ms", median(tZlib));
	out.info("  base64       {:8.2f} ms", median(tB64));
	// What goes down the pipe, and the cheaper variants worth comparing.
	const size_t raw = bmp.m_pixels.size();
	const std::string z =
	    rv::kitty::zlibCompress(bmp.m_pixels.data(), bmp.m_pixels.size());
	std::vector<uint8_t> rgb;
	rgb.reserve(raw / 4 * 3);
	for(size_t i = 0; i < raw; i += 4)
	{
		rgb.insert(rgb.end(), &bmp.m_pixels[i], &bmp.m_pixels[i + 3]);
	}
	std::vector<double> tRgb;
	std::string zRgb;
	for(int run = 0; run < 5; ++run)
	{
		auto t0 = Clock::now();
		zRgb = rv::kitty::zlibCompress(rgb.data(), rgb.size());
		tRgb.push_back(ms(t0, Clock::now()));
	}
	auto mb = [](size_t b) { return b / (1024.0 * 1024.0); };
	out.info(
	    "  payload      RGBA {:.1f} MB → zlib {:.1f} MB → base64 {:.1f} MB",
	    mb(raw),
	    mb(z.size()),
	    mb((z.size() + 2) / 3 * 4));
	out.info("  zlib RGB     {:8.2f} ms → {:.1f} MB (f=24, no alpha)",
	         median(tRgb),
	         mb(zRgb.size()));
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	CLI::App cli{
	    "rvtui — terminal image browser/viewer for EXR, TIFF, PNG and JPEG"};
	cli.set_version_flag("--version", RVTUI_VERSION);

	std::vector<std::string> paths;
	std::string benchPath;
	cli.add_option("--bench", benchPath)->group(""); // hidden: stage timings
	bool helpKeys = false; // hidden: the user guide's keys reference
	cli.add_flag("--help-keys", helpKeys)->group("");
	bool tutorialRender = false; // hidden: render the tutorial set, then exit
	cli.add_flag("--tutorial-render", tutorialRender)->group("");
	std::string dumpPath, graphics = "auto", transfer = "auto", chooserFile,
	                      cwdFile;
	auto* pathsOpt = cli.add_option(
	    "paths",
	    paths,
	    "Directory, or image(s) to view; a sequence needs a glob: "
	    "shot.*.exr, 'shot.####.exr' or 'shot.#.exr' (any padding). "
	    "Label one with :label=<slot>:<text> (slot tl tc tr bl bc br, "
	    "top or bottom), e.g. a.exr:label=top:main");
	std::vector<std::pair<std::string, std::string>> diffs;
	auto* diffOpt =
	    cli.add_option("--diff",
	                   diffs,
	                   "Compare A with B: shown as A, B and |A - B|, tiled "
	                   "(repeatable; "
	                   "paths can carry labels; n / p go pair by pair)")
	        ->allow_extra_args(false); // a pair, then paths again
	cli.add_option("--dump",
	               dumpPath,
	               "Print layers, channels and metadata, then exit");
	cli.add_option("--graphics",
	               graphics,
	               "Image output: auto | kitty | halfblock")
	    ->check(CLI::IsMember({"auto", "kitty", "halfblock"}));
	cli.add_option("--transfer",
	               transfer,
	               "How kitty images reach the terminal: auto | direct | shm | "
	               "file (auto: in tmux, direct with a client attached over "
	               "ssh, else file; outside it, direct over ssh, else shm)")
	    ->check(CLI::IsMember({"auto", "direct", "shm", "file"}));
	cli.add_option("--chooser-file",
	               chooserFile,
	               "Browser: o writes the marked paths (else the selected one) "
	               "here and exits");
	std::uint64_t cacheBytes = rv::ImageService::DEFAULT_BUDGET;
	cli.add_option("--cache",
	               cacheBytes,
	               "Memory for decoded images, e.g. 8G or 512M (default 4G)")
	    ->transform(CLI::AsSizeValue(false))
	    ->check(CLI::Validator(
	        [](const std::string& bytes)
	        {
		        return std::stoull(bytes) < (std::uint64_t(64) << 20)
		                   ? std::string("at least 64M")
		                   : std::string();
	        },
	        ""));
	auto linkRate =
	    static_cast<std::uint64_t>(rv::kitty::Transmitter::DEFAULT_LINK_RATE);
	cli.add_option("--link-rate",
	               linkRate,
	               "Bytes a second the ssh link carries, e.g. 20M; playback "
	               "sending images inline is paced to it (default 4M, 0: "
	               "unpaced)")
	    ->transform(CLI::AsSizeValue(false));
	bool doctor = false;
	cli.add_flag("--doctor",
	             doctor,
	             "Check the terminal / tmux setup for images and copy, then "
	             "exit");
	bool tutorial = false;
	cli.add_flag("--tutorial",
	             tutorial,
	             "A guided tour on sample images (rendered into "
	             "~/.cache/rvtui/tutorial on first use)");
	cli.add_option("--cwd-file",
	               cwdFile,
	               "On exit, write the last directory here");
	CLI11_PARSE(cli, argc, argv);
	rv::log::initConsole();

	try
	{
		if(doctor)
		{
			return rv::runDoctor(graphics);
		}
		if(!dumpPath.empty())
		{
			return dump(dumpPath);
		}
		if(tutorialRender)
		{
			// For scripts (the guide's recorder): the set as --tutorial
			// opens it, rendered only when missing or out of date.
			rv::tutorial::ensure(rv::tutorial::defaultDir());
			fmt::print("{}\n", rv::tutorial::defaultDir().string());
			return 0;
		}
		if(helpKeys)
		{
			fmt::print("{}\n", rv::help::markdown());
			return 0;
		}
		if(!benchPath.empty())
		{
			return bench(benchPath, 2560, 1440);
		}
		rv::AppOptions opts;
		opts.m_paths = paths;
		if(!diffs.empty())
		{
			// Pairs and plain paths, in the order given. The order lists an
			// option once per value: a pair twice.
			size_t p = 0, d = 0, diffValues = 0;
			for(const CLI::Option* o : cli.parse_order())
			{
				if(o == pathsOpt && p < paths.size())
				{
					opts.m_args.push_back({paths[p++]});
				}
				else if(o == diffOpt && diffValues++ % 2 == 0 &&
				        d < diffs.size())
				{
					opts.m_args.push_back({diffs[d].first, diffs[d].second});
					++d;
				}
			}
			for(; d < diffs.size(); ++d)
			{
				opts.m_args.push_back({diffs[d].first, diffs[d].second});
			}
			for(; p < paths.size(); ++p)
			{
				opts.m_args.push_back({paths[p]});
			}
		}
		if(tutorial)
		{
			const auto dir = rv::tutorial::defaultDir();
			rv::tutorial::ensure(dir,
			                     {},
			                     [&dir](int done, int total)
			                     {
				                     fmt::print(stderr,
				                                "\rrendering the tutorial into "
				                                "{}: {}/{}",
				                                dir.string(),
				                                done,
				                                total);
				                     if(done == total)
				                     {
					                     fmt::print(stderr, "\n");
				                     }
			                     });
			opts.m_paths = {dir.string()};
		}
		opts.m_graphics = graphics;
		opts.m_transfer = transfer;
		opts.m_chooserFile = chooserFile;
		opts.m_cwdFile = cwdFile;
		opts.m_cacheBytes = cacheBytes;
		opts.m_tutorial = tutorial;
		opts.m_linkRate = static_cast<double>(linkRate);
		return rv::runApp(opts);
	}
	catch(const std::exception& e)
	{
		spdlog::error("{}", e.what());
		return 1;
	}
}
