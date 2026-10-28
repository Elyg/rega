#include "agent/AgentDemo.h"

#include "agent/AgentClient.h"
#include "agent/Protocol.h"
#include "image/Loader.h"
#include "image/Render.h"
#include "image/Sequence.h"
#include "image/StbWriter.h"
#include "term/Caps.h"
#include "term/Kitty.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace rega::agent
{

namespace
{

using Clock = std::chrono::steady_clock;

double ms(Clock::duration d)
{
	return std::chrono::duration<double, std::milli>(d).count();
}

std::atomic<bool> g_stop{false};

void onInterrupt(int)
{
	g_stop = true;
}

// The image id the demo shows under (256-palette placeholder colour).
constexpr uint32_t IMAGE_ID = 201;

// Samples (ms) with their mean and max, for the per-loop lines.
struct Samples
{
	double m_sum = 0, m_max = 0;
	size_t m_n = 0;
	void add(double v)
	{
		m_sum += v;
		m_max = std::max(m_max, v);
		++m_n;
	}
	[[nodiscard]] std::string str() const
	{
		return m_n ? fmt::format("{:.1f} avg / {:.1f} max ms",
		                         m_sum / double(m_n),
		                         m_max)
		           : std::string("-");
	}
};

// What the reader thread learns, per frame key.
struct Slot
{
	std::string m_name;
	bool m_armed = false, m_dropped = false, m_everReady = false;
	Clock::time_point m_sentAt, m_shownAt;
};

struct Shared
{
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::vector<Slot> m_slots;
	bool m_closed = false;
	Samples m_readyLatency; ///< FRAME written → READY
	Samples m_rearmLatency; ///< SHOWN → READY of the next name
};

void writeTerminal(std::string_view bytes)
{
	std::fwrite(bytes.data(), 1, bytes.size(), stdout);
	std::fflush(stdout);
}

} // namespace

int runAgentDemo(const DemoOptions& opt)
{
	std::vector<std::string> missing;
	std::vector<std::filesystem::path> frames;
	for(const Entry& e : entriesForArgs(opt.m_paths, missing))
	{
		if(e.m_kind == Entry::Kind::SEQUENCE)
		{
			frames.insert(frames.end(), e.m_frames.begin(), e.m_frames.end());
		}
		else if(e.m_kind == Entry::Kind::FILE)
		{
			frames.push_back(e.m_path);
		}
	}
	if(frames.empty())
	{
		spdlog::error("--agent-demo: no images in the paths given");
		return 2;
	}

	TermCaps caps = detectCaps("kitty", "auto");
	refreshCellSize(caps);
	struct winsize ws{};
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col < 2 ||
	   ws.ws_row < 3)
	{
		spdlog::error("--agent-demo: needs a terminal");
		return 2;
	}

	const std::vector<std::string> sockets = agentSockets(caps.m_tmux);
	AgentLink link;
	std::string socketPath;
	size_t dead = 0;
	for(const auto& s : sockets)
	{
		link = connectAgent(s);
		if(link.m_fd >= 0)
		{
			socketPath = s;
			break;
		}
		++dead;
	}
	if(link.m_fd < 0)
	{
		spdlog::error("--agent-demo: no agent answered ({} socket(s) "
		              "tried: {}); run `rega --agent -- ssh …` on the Mac",
		              sockets.size(),
		              fmt::join(sockets, ", "));
		return 1;
	}
	std::signal(SIGPIPE, SIG_IGN);
	std::signal(SIGINT, onInterrupt);
	const int quality = opt.m_quality == "lossless" ? QUALITY_LOSSLESS
	                    : opt.m_quality == "raw"
	                        ? QUALITY_RAW
	                        : std::atoi(opt.m_quality.c_str());

	// Fit the first frame into the terminal above a status line.
	const ImageInfo first = probeImage(frames.front());
	const Box2i box = first.m_parts.front().m_displayWindow;
	const int areaCols = std::min<int>(ws.ws_col, kitty::MAX_PLACEHOLDER_INDEX);
	const int areaRows =
	    std::min<int>(ws.ws_row - 1, kitty::MAX_PLACEHOLDER_INDEX);
	const double scale =
	    std::min(double(areaCols * caps.m_cellW) / box.width(),
	             double(areaRows * caps.m_cellH) / box.height());
	const int cols =
	    std::clamp(int(std::ceil(box.width() * scale / caps.m_cellW)),
	               1,
	               areaCols);
	const int rows =
	    std::clamp(int(std::ceil(box.height() * scale / caps.m_cellH)),
	               1,
	               areaRows);
	const int outW = cols * caps.m_cellW, outH = rows * caps.m_cellH;
	// transmitShared takes the size from a picture: one of the right shape.
	Rgba8Image shape;
	shape.m_width = outW;
	shape.m_height = outH;
	shape.m_pixels.resize(size_t(outW) * outH * 4);
	kitty::TransmitOptions topt;
	topt.m_id = IMAGE_ID;
	topt.m_cols = cols;
	topt.m_rows = rows;
	topt.m_tmux = caps.tmuxLayers();

	std::vector<std::string> report;
	report.push_back(fmt::format(
	    "agent {} (token {}, {} dead skipped); {} frames {}x{} px in {}x{} "
	    "cells, quality {}, tmux layers {}",
	    socketPath,
	    link.m_token,
	    dead,
	    frames.size(),
	    outW,
	    outH,
	    cols,
	    rows,
	    opt.m_quality,
	    topt.m_tmux));

	// Placeholder cells once; every frame replaces the image under them.
	{
		std::string screen = "\x1b[?25l\x1b[2J";
		for(int r = 0; r < rows; ++r)
		{
			screen += fmt::format("\x1b[{};1H\x1b[38;5;{}m", r + 1, IMAGE_ID);
			for(int c = 0; c < cols; ++c)
			{
				screen += kitty::placeholderCell(r, c);
			}
			screen += "\x1b[39m";
		}
		writeTerminal(screen);
	}
	auto status = [&](const std::string& text)
	{ writeTerminal(fmt::format("\x1b[{};1H\x1b[2K{}", ws.ws_row, text)); };

	Shared shared;
	shared.m_slots.resize(frames.size());
	std::mutex sendMutex;
	auto send = [&](const Message& m)
	{
		std::lock_guard lock(sendMutex);
		return writeMessage(link.m_fd, m);
	};

	std::thread reader(
	    [&]
	    {
		    for(;;)
		    {
			    auto m = readMessage(link.m_fd);
			    std::lock_guard lock(shared.m_mutex);
			    if(!m || m->m_type == Type::BYE)
			    {
				    shared.m_closed = true;
				    shared.m_cv.notify_all();
				    return;
			    }
			    if(m->m_key >= shared.m_slots.size())
			    {
				    continue;
			    }
			    Slot& s = shared.m_slots[m->m_key];
			    if(m->m_type == Type::READY)
			    {
				    const auto now = Clock::now();
				    if(s.m_everReady)
				    {
					    shared.m_rearmLatency.add(ms(now - s.m_shownAt));
				    }
				    else
				    {
					    shared.m_readyLatency.add(ms(now - s.m_sentAt));
				    }
				    s.m_name = m->m_name;
				    s.m_armed = s.m_everReady = true;
			    }
			    else if(m->m_type == Type::DROP)
			    {
				    s.m_armed = false;
				    s.m_dropped = true;
			    }
			    shared.m_cv.notify_all();
		    }
	    });

	// Show `key` from its armed name; false when it never came (dropped,
	// agent gone, ^C).
	size_t waits = 0, stale = 0;
	Samples waited;
	auto show = [&](uint32_t key, Clock::time_point due)
	{
		std::string name;
		{
			std::unique_lock lock(shared.m_mutex);
			Slot& s = shared.m_slots[key];
			if(!s.m_armed && Clock::now() >= due)
			{
				++waits;
			}
			const auto t0 = Clock::now();
			// A shown name not re-armed within a second: the terminal kept it
			// (no unlink), so it may still be there.
			const bool rearming = s.m_everReady;
			while(!s.m_armed && !s.m_dropped && !shared.m_closed && !g_stop &&
			      !(rearming && Clock::now() - t0 > std::chrono::seconds(1)))
			{
				shared.m_cv.wait_for(lock, std::chrono::milliseconds(100));
			}
			if(!s.m_armed && rearming && !s.m_dropped && !shared.m_closed &&
			   !g_stop)
			{
				++stale;
			}
			else if(!s.m_armed)
			{
				return false;
			}
			if(const double w = ms(Clock::now() - std::max(t0, due)); w > 0.5)
			{
				waited.add(w);
			}
			name = s.m_name;
			s.m_armed = false;
			s.m_shownAt = Clock::now();
		}
		writeTerminal(kitty::transmitShared(shape, name, topt));
		Message shown;
		shown.m_type = Type::SHOWN;
		shown.m_key = key;
		send(shown);
		return true;
	};

	// Loop 1: render, encode and send every frame (another thread), showing
	// each as soon as the agent has it.
	Samples prep, enc, write;
	size_t bytes = 0;
	const auto loopStart = Clock::now();
	std::thread producer(
	    [&]
	    {
		    for(uint32_t key = 0; key < frames.size() && !g_stop; ++key)
		    {
			    const auto t0 = Clock::now();
			    Rgba8Image bmp;
			    try
			    {
				    const ImageInfo info = probeImage(frames[key]);
				    bmp = renderLayer(loadLayer(info, 0), outW, outH, {}, {});
			    }
			    catch(const std::exception& e)
			    {
				    spdlog::warn("{}: {}", frames[key].string(), e.what());
				    std::lock_guard lock(shared.m_mutex);
				    shared.m_slots[key].m_dropped = true;
				    shared.m_cv.notify_all();
				    continue;
			    }
			    const auto t1 = Clock::now();
			    auto [codec, encoded] = encodeFrame(bmp, quality);
			    const auto t2 = Clock::now();
			    Message m;
			    m.m_type = Type::FRAME;
			    m.m_key = key;
			    m.m_width = uint32_t(bmp.m_width);
			    m.m_height = uint32_t(bmp.m_height);
			    m.m_codec = codec;
			    m.m_bytes = std::move(encoded);
			    bytes += m.m_bytes.size();
			    {
				    std::lock_guard lock(shared.m_mutex);
				    shared.m_slots[key].m_sentAt = Clock::now();
			    }
			    if(!send(m))
			    {
				    return;
			    }
			    prep.add(ms(t1 - t0));
			    enc.add(ms(t2 - t1));
			    write.add(ms(Clock::now() - t2));
		    }
	    });
	size_t shown = 0;
	for(uint32_t key = 0; key < frames.size() && !g_stop; ++key)
	{
		shown += show(key, Clock::now());
		status(fmt::format("loop 1: {}/{}", key + 1, frames.size()));
	}
	producer.join();
	const double loop1 = ms(Clock::now() - loopStart) / 1e3;
	{
		std::lock_guard lock(shared.m_mutex);
		report.push_back(fmt::format(
		    "loop 1: {} shown, {:.1f} fps, {:.0f} KB/frame ({:.1f} MB/s); "
		    "render {}, encode {}, write {}; FRAME→READY {}",
		    shown,
		    shown / std::max(loop1, 1e-6),
		    frames.empty() ? 0.0 : bytes / 1024.0 / frames.size(),
		    bytes / 1048576.0 / std::max(loop1, 1e-6),
		    prep.str(),
		    enc.str(),
		    write.str(),
		    shared.m_readyLatency.str()));
	}
	status(report.back());

	// Loops 2+: names only, paced at opt.m_fps.
	for(int loop = 2; loop <= opt.m_loops && !g_stop; ++loop)
	{
		waits = stale = 0;
		waited = {};
		{
			std::lock_guard lock(shared.m_mutex);
			shared.m_rearmLatency = {};
		}
		shown = 0;
		const auto start = Clock::now();
		for(uint32_t key = 0; key < frames.size() && !g_stop; ++key)
		{
			const auto due =
			    opt.m_fps > 0
			        ? start +
			              std::chrono::duration_cast<Clock::duration>(
			                  std::chrono::duration<double>(key / opt.m_fps))
			        : Clock::now();
			std::this_thread::sleep_until(due);
			shown += show(key, due);
		}
		const double secs = ms(Clock::now() - start) / 1e3;
		std::lock_guard lock(shared.m_mutex);
		report.push_back(fmt::format(
		    "loop {}: {} shown, {:.1f} fps (target {}), 0 frame bytes; {} "
		    "late (waited {}), {} stale; SHOWN→re-armed {}",
		    loop,
		    shown,
		    shown / std::max(secs, 1e-6),
		    opt.m_fps > 0 ? fmt::format("{:g}", opt.m_fps) : "max",
		    waits,
		    waited.str(),
		    stale,
		    shared.m_rearmLatency.str()));
		status(report.back());
	}

	// The agent unlinks everything on BYE: first let the terminal read the
	// last names (re-armed once it has).
	{
		std::unique_lock lock(shared.m_mutex);
		shared.m_cv.wait_for(lock,
		                     std::chrono::seconds(1),
		                     [&]
		                     {
			                     return shared.m_closed ||
			                            std::all_of(shared.m_slots.begin(),
			                                        shared.m_slots.end(),
			                                        [](const Slot& s)
			                                        {
				                                        return s.m_armed ||
				                                               s.m_dropped;
			                                        });
		                     });
	}
	Message bye;
	bye.m_type = Type::BYE;
	send(bye);
	shutdown(link.m_fd, SHUT_WR);
	reader.join();
	close(link.m_fd);
	writeTerminal(kitty::deleteImage(IMAGE_ID, topt.m_tmux) +
	              fmt::format("\x1b[{};1H\x1b[2K\x1b[?25h", ws.ws_row));
	for(const auto& line : report)
	{
		fmt::print("{}\n", line);
	}
	return g_stop ? 130 : 0;
}

} // namespace rega::agent
