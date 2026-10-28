#include "BuildInfo.h"
#include "agent/Agent.h"
#include "agent/AgentClient.h"
#include "agent/AgentDemo.h"
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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int dump(const fs::path& path)
{
	auto& out = rega::log::out();
	rega::ImageInfo info = rega::probeImage(path);
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
			                     rega::LayerInfo::shortName(l.m_channels[c]),
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
	auto& out = rega::log::out();
	auto ms = [](Clock::time_point a, Clock::time_point b)
	{ return std::chrono::duration<double, std::milli>(b - a).count(); };
	auto median = [](std::vector<double> v)
	{
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};
	std::vector<double> tProbe, tDecode, tDown, tRender, tZlib, tB64;
	rega::Rgba8Image bmp;
	for(int run = 0; run < 5; ++run)
	{
		auto t0 = Clock::now();
		rega::ImageInfo info = rega::probeImage(path);
		auto t1 = Clock::now();
		rega::LayerImage img = rega::loadLayer(info, 0);
		auto t2 = Clock::now();
		rega::LayerImage half = rega::downsample(img, 4);
		auto t3 = Clock::now();
		bmp = rega::renderLayer(img, outW, outH, {}, {});
		auto t4 = Clock::now();
		std::string z =
		    rega::kitty::zlibCompress(bmp.m_pixels.data(), bmp.m_pixels.size());
		auto t5 = Clock::now();
		std::string b =
		    rega::kitty::base64(reinterpret_cast<const uint8_t*>(z.data()),
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
	    rega::kitty::zlibCompress(bmp.m_pixels.data(), bmp.m_pixels.size());
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
		zRgb = rega::kitty::zlibCompress(rgb.data(), rgb.size());
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
	// PNG (f=100) per row filter, on the picture and on its alpha view (grey:
	// one channel instead of three).
	rega::DisplayParams alphaView;
	alphaView.m_mode = rega::ChannelMode::ALPHA;
	const rega::Rgba8Image alpha =
	    rega::renderLayer(rega::loadLayer(rega::probeImage(path), 0),
	                      outW,
	                      outH,
	                      {},
	                      alphaView);
	const std::pair<const char*, rega::kitty::PngFilter> filters[] =
	    {{"none", rega::kitty::PngFilter::NONE},
	     {"sub", rega::kitty::PngFilter::SUB},
	     {"up", rega::kitty::PngFilter::UP},
	     {"paeth", rega::kitty::PngFilter::PAETH}};
	for(const auto& [what, pic] :
	    {std::pair<const char*, const rega::Rgba8Image*>{"picture", &bmp},
	     {"alpha view", &alpha}})
	{
		const size_t was = pic == &bmp
		                       ? z.size()
		                       : rega::kitty::zlibCompress(pic->m_pixels.data(),
		                                                   pic->m_pixels.size())
		                             .size();
		out.info("  {}: zlib RGBA {:.2f} MB", what, mb(was));
		for(const auto& [name, f] : filters)
		{
			std::vector<double> t;
			std::string png;
			for(int run = 0; run < 3; ++run)
			{
				auto t0 = Clock::now();
				png = rega::kitty::encodePng(*pic, f);
				t.push_back(ms(t0, Clock::now()));
			}
			out.info("    png {:6} {:8.2f} ms → {:.2f} MB",
			         name,
			         median(t),
			         mb(png.size()));
		}
	}
	return 0;
}

// A size in bytes: "3.9G", "512M", "20000"; K/M/G/T count 1024s, with or
// without "i" / "B", any case. CLI::AsSizeValue reads the number as an
// integer: "3.9G" could not be converted.
CLI::Validator sizeValue()
{
	return CLI::Validator(
	    [](std::string& in) -> std::string
	    {
		    char* end = nullptr;
		    const double n = std::strtod(in.c_str(), &end);
		    std::string unit;
		    for(const char* c = end; *c; ++c)
		    {
			    if(*c != ' ')
			    {
				    unit += static_cast<char>(
				        std::tolower(static_cast<unsigned char>(*c)));
			    }
		    }
		    if(unit.ends_with('b'))
		    {
			    unit.pop_back();
		    }
		    if(unit.ends_with('i'))
		    {
			    unit.pop_back();
		    }
		    static const std::map<std::string, double>
		        SCALE{{"", 1.0},
		              {"k", 1024.0},
		              {"m", 1024.0 * 1024},
		              {"g", 1024.0 * 1024 * 1024},
		              {"t", 1024.0 * 1024 * 1024 * 1024}};
		    const auto scale = SCALE.find(unit);
		    if(end == in.c_str() || scale == SCALE.end() || !(n >= 0))
		    {
			    return "not a size: " + in + " (e.g. 512M, 3.9G)";
		    }
		    in = std::to_string(
		        static_cast<std::uint64_t>(std::llround(n * scale->second)));
		    return {};
	    },
	    "SIZE");
}

// This executable's absolute path, for --shell-init.
std::string selfPath(const char* argv0)
{
	std::error_code ec;
#ifdef __APPLE__
	char buf[4096];
	uint32_t size = sizeof buf;
	if(_NSGetExecutablePath(buf, &size) == 0)
	{
		if(auto p = fs::canonical(buf, ec); !ec)
		{
			return p.string();
		}
	}
#else
	if(auto p = fs::canonical("/proc/self/exe", ec); !ec)
	{
		return p.string();
	}
#endif
	auto p = fs::canonical(argv0, ec);
	return ec ? std::string(argv0) : p.string();
}

} // namespace

int main(int argc, char** argv)
{
	CLI::App cli{
	    "rega — terminal image browser/viewer for EXR, TIFF, PNG and JPEG"};
	cli.set_version_flag("--version", REGA_VERSION);

	std::vector<std::string> paths;
	std::string benchPath;
	cli.add_option("--bench", benchPath)->group(""); // hidden: stage timings
	bool helpKeys = false; // hidden: the user guide's keys reference
	cli.add_flag("--help-keys", helpKeys)->group("");
	bool tutorialRender = false; // hidden: render the tutorial set, then exit
	cli.add_flag("--tutorial-render", tutorialRender)->group("");
	// Hidden, agent mode prototype (docs/plans/agent-prototype.md): `--agent
	// -- ssh …` on the Mac, `--agent-demo <sequence>` on the remote.
	bool agent = false, agentDemo = false;
	cli.add_flag("--agent", agent)->group("");
	cli.add_flag("--agent-demo", agentDemo)->group("");
	std::uint64_t agentBudget = 0;
	cli.add_option("--agent-budget", agentBudget)
	    ->transform(sizeValue())
	    ->group("");
	std::string agentLog, quality = "85";
	cli.add_option("--agent-log", agentLog)->group("");
	cli.add_option("--quality", quality)
	    ->check(CLI::IsMember({"60", "85", "95", "lossless", "raw"}))
	    ->group("");
	std::string shellInit;
	cli.add_option("--shell-init", shellInit)
	    ->check(CLI::IsMember({"bash", "zsh"}))
	    ->group("");
	bool noAgent = false;
	cli.add_flag("--no-agent", noAgent)->group("");
	std::uint64_t agentCache = rega::kitty::Transmitter::DEFAULT_AGENT_CACHE;
	cli.add_option("--agent-cache", agentCache)
	    ->transform(sizeValue())
	    ->group("");
	int demoLoops = 3;
	double demoFps = 24;
	cli.add_option("--loops", demoLoops)->group("");
	cli.add_option("--fps", demoFps)->group("");
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
	std::uint64_t cacheBytes = rega::ImageService::DEFAULT_BUDGET;
	cli.add_option("--cache",
	               cacheBytes,
	               "Memory for decoded images, e.g. 8G or 512M (default 4G)")
	    ->transform(sizeValue())
	    ->check(CLI::Validator(
	        [](const std::string& bytes)
	        {
		        return std::stoull(bytes) < (std::uint64_t(64) << 20)
		                   ? std::string("at least 64M")
		                   : std::string();
	        },
	        ""));
	auto linkRate =
	    static_cast<std::uint64_t>(rega::kitty::Transmitter::DEFAULT_LINK_RATE);
	cli.add_option("--link-rate",
	               linkRate,
	               "Bytes a second the ssh link is assumed to carry until "
	               "measured, e.g. 20M; playback sending images inline is "
	               "paced to the link (default 4M, 0: unpaced, unmeasured)")
	    ->transform(sizeValue());
	std::uint64_t terminalCache = rega::AppOptions::DEFAULT_TERMINAL_CACHE;
	cli.add_option("--terminal-cache",
	               terminalCache,
	               "Terminal image memory frames played over ssh may keep "
	               "there, e.g. 512M: a loop's second pass sends no pixels "
	               "(default 256M, under kitty's and Ghostty's 320M; 3.9G with "
	               "Ghostty's image-storage-limit at its 4G maximum; 0: off)")
	    ->transform(sizeValue());
	bool doctor = false;
	cli.add_flag("--doctor",
	             doctor,
	             "Check the terminal / tmux setup for images and copy, then "
	             "exit");
	bool tutorial = false;
	cli.add_flag("--tutorial",
	             tutorial,
	             "A guided tour on sample images (rendered into "
	             "~/.cache/rega/tutorial on first use)");
	cli.add_option("--cwd-file",
	               cwdFile,
	               "On exit, write the last directory here");
	CLI11_PARSE(cli, argc, argv);
	rega::log::initConsole();

	try
	{
		if(doctor)
		{
			return rega::runDoctor(graphics);
		}
		if(!shellInit.empty())
		{
			fmt::print("{}",
			           rega::agent::shellInit(shellInit, selfPath(argv[0])));
			return 0;
		}
		if(agent)
		{
			rega::agent::AgentOptions a;
			a.m_command = paths; // what follows --
			a.m_budget = agentBudget;
			a.m_log = agentLog;
			return rega::agent::runAgent(a);
		}
		if(agentDemo)
		{
			rega::agent::DemoOptions d;
			d.m_paths = paths;
			d.m_quality = quality;
			d.m_loops = demoLoops;
			d.m_fps = demoFps;
			return rega::agent::runAgentDemo(d);
		}
		if(!dumpPath.empty())
		{
			return dump(dumpPath);
		}
		if(tutorialRender)
		{
			// For scripts (the guide's recorder): the set as --tutorial
			// opens it, rendered only when missing or out of date.
			rega::tutorial::ensure(rega::tutorial::defaultDir());
			fmt::print("{}\n", rega::tutorial::defaultDir().string());
			return 0;
		}
		if(helpKeys)
		{
			fmt::print("{}\n", rega::help::markdown());
			return 0;
		}
		if(!benchPath.empty())
		{
			return bench(benchPath, 2560, 1440);
		}
		rega::AppOptions opts;
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
			const auto dir = rega::tutorial::defaultDir();
			rega::tutorial::ensure(dir,
			                       {},
			                       [&dir](int done, int total)
			                       {
				                       fmt::print(
				                           stderr,
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
		opts.m_terminalCache = terminalCache;
		opts.m_agent = !noAgent;
		opts.m_agentCache = agentCache;
		opts.m_agentQuality = quality == "lossless"
		                          ? rega::agent::QUALITY_LOSSLESS
		                      : quality == "raw" ? rega::agent::QUALITY_RAW
		                                         : std::stoi(quality);
		return rega::runApp(opts);
	}
	catch(const std::exception& e)
	{
		spdlog::error("{}", e.what());
		return 1;
	}
}
