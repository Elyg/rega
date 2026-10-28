#include "agent/AgentClient.h"

#include "image/StbWriter.h"
#include "term/Caps.h"
#include "term/Kitty.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <unistd.h>

namespace rega::agent
{

namespace
{
// The forwards sshd made for agents of ours, newest first.
std::vector<std::string> forwardsInTmp()
{
	std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
	std::error_code ec;
	for(const auto& e : std::filesystem::directory_iterator("/tmp", ec))
	{
		const std::string name = e.path().filename().string();
		struct stat st{};
		if(!name.starts_with("rega-agent-") || !name.ends_with(".sock") ||
		   stat(e.path().c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) ||
		   st.st_uid != getuid())
		{
			continue;
		}
		found.emplace_back(e.last_write_time(ec), e.path().string());
	}
	std::sort(found.begin(),
	          found.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<std::string> out;
	for(auto& f : found)
	{
		out.push_back(std::move(f.second));
	}
	return out;
}
} // namespace

std::vector<std::string> agentSockets(bool tmux)
{
	std::vector<std::string> out;
	if(tmux)
	{
		out = tmuxClientEnv("LC_REGA_AGENT");
	}
	if(const char* own = std::getenv("LC_REGA_AGENT"); own && *own)
	{
		out.emplace_back(own);
	}
	for(auto& s : forwardsInTmp())
	{
		out.push_back(std::move(s));
	}
	std::vector<std::string> unique;
	for(auto& s : out)
	{
		if(!s.empty() &&
		   std::find(unique.begin(), unique.end(), s) == unique.end())
		{
			unique.push_back(std::move(s));
		}
	}
	return unique;
}

AgentLink connectAgent(const std::string& path, int timeoutMs)
{
	AgentLink link;
	sockaddr_un addr{};
	if(path.size() >= sizeof addr.sun_path)
	{
		return link;
	}
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd < 0)
	{
		return link;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
	if(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
	{
		close(fd);
		return link;
	}
	Message hello;
	hello.m_type = Type::HELLO;
	hello.m_version = PROTOCOL_VERSION;
	// A dead forward accepts, then closes once ssh can't reach the Mac end:
	// only an answer counts.
	pollfd p{fd, POLLIN, 0};
	std::optional<Message> answer;
	if(writeMessage(fd, hello) && poll(&p, 1, timeoutMs) == 1)
	{
		answer = readMessage(fd);
	}
	if(!answer || answer->m_type != Type::HELLO ||
	   answer->m_version != PROTOCOL_VERSION)
	{
		close(fd);
		return link;
	}
	link.m_fd = fd;
	link.m_token = answer->m_token;
	return link;
}

std::string qualityName(int quality)
{
	if(quality == QUALITY_LOSSLESS)
	{
		return "lossless";
	}
	if(quality == QUALITY_RAW)
	{
		return "raw";
	}
	return "q" + std::to_string(quality);
}

std::pair<Codec, std::string> encodeFrame(const Rgba8Image& img, int quality)
{
	if(quality == QUALITY_RAW)
	{
		return {Codec::RAW,
		        std::string(img.m_pixels.begin(), img.m_pixels.end())};
	}
	if(quality >= QUALITY_LOSSLESS)
	{
		return {Codec::PNG, kitty::encodePng(img)};
	}
	return {Codec::JPEG, encodeJpeg(img, quality)};
}

std::shared_ptr<AgentClient> AgentClient::discover(bool tmux)
{
	for(const auto& path : agentSockets(tmux))
	{
		AgentLink link = connectAgent(path);
		if(link.m_fd >= 0)
		{
			spdlog::info("agent: {} (token {})", path, link.m_token);
			return std::make_shared<AgentClient>(std::move(link), path);
		}
		spdlog::debug("agent: {} did not answer", path);
	}
	return nullptr;
}

AgentClient::AgentClient(AgentLink link, std::string path)
    : m_fd(link.m_fd), m_path(std::move(path))
{
	m_reader = std::thread([this] { readLoop(); });
}

AgentClient::~AgentClient()
{
	setOnChange({}); // whoever it wakes may be going too
	Message bye;
	bye.m_type = Type::BYE;
	write(bye);
	shutdown(m_fd, SHUT_RDWR);
	m_reader.join();
	close(m_fd);
}

void AgentClient::setOnChange(std::function<void()> fn)
{
	std::lock_guard lock(m_mutex);
	m_onChange = std::move(fn);
}

bool AgentClient::write(const Message& m)
{
	if(!m_connected)
	{
		return false;
	}
	std::lock_guard lock(m_writeMutex);
	if(!writeMessage(m_fd, m))
	{
		m_connected = false;
		return false;
	}
	return true;
}

uint32_t AgentClient::send(const Rgba8Image& img)
{
	if(!m_connected || img.m_width <= 0 || img.m_height <= 0)
	{
		return 0;
	}
	auto [codec, bytes] = encodeFrame(img, m_quality);
	Message m;
	m.m_type = Type::FRAME;
	m.m_width = static_cast<uint32_t>(img.m_width);
	m.m_height = static_cast<uint32_t>(img.m_height);
	m.m_codec = codec;
	m.m_bytes = std::move(bytes);
	{
		std::lock_guard lock(m_mutex);
		m.m_key = m_nextKey++;
		Frame& f = m_frames[m.m_key];
		f.m_sentAt = Clock::now();
		++m_stats.m_sent;
		m_stats.m_bytes += m.m_bytes.size();
		m_stats.m_lastBytes = m.m_bytes.size();
		m_stats.m_lastW = img.m_width;
		m_stats.m_lastH = img.m_height;
	}
	if(!write(m))
	{
		std::lock_guard lock(m_mutex);
		m_frames.erase(m.m_key);
		return 0;
	}
	return m.m_key;
}

AgentClient::Show AgentClient::take(uint32_t key, std::string& name)
{
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_frames.find(key);
		if(!m_connected || it == m_frames.end() || it->second.m_dropped)
		{
			return Show::GONE;
		}
		Frame& f = it->second;
		const auto now = Clock::now();
		if(f.m_armed)
		{
			f.m_armed = false;
		}
		else if(f.m_everReady && now - f.m_shownAt > STALE_AFTER)
		{
			// Never read: the terminal or a tmux on the way dropped the
			// escape. The name should still be there.
			++m_stats.m_stale;
			spdlog::debug("agent: {} not read in {} ms; again",
			              f.m_name,
			              std::chrono::duration_cast<std::chrono::milliseconds>(
			                  now - f.m_shownAt)
			                  .count());
		}
		else
		{
			return Show::WAIT;
		}
		name = f.m_name;
		f.m_shownAt = now;
		++m_stats.m_shown;
	}
	Message shown;
	shown.m_type = Type::SHOWN;
	shown.m_key = key;
	write(shown);
	return Show::NOW;
}

void AgentClient::forget(uint32_t key)
{
	{
		std::lock_guard lock(m_mutex);
		if(m_frames.erase(key) == 0)
		{
			return;
		}
	}
	Message drop;
	drop.m_type = Type::DROP;
	drop.m_key = key;
	write(drop);
}

AgentClient::Stats AgentClient::stats() const
{
	std::lock_guard lock(m_mutex);
	Stats s = m_stats;
	s.m_held = m_frames.size();
	return s;
}

void AgentClient::readLoop()
{
	auto smooth = [](double& avg, double v)
	{ avg = avg > 0 ? 0.8 * avg + 0.2 * v : v; };
	for(;;)
	{
		auto m = readMessage(m_fd);
		std::function<void()> onChange;
		{
			std::lock_guard lock(m_mutex);
			onChange = m_onChange;
			if(!m || m->m_type == Type::BYE)
			{
				m_connected = false;
				spdlog::info("agent: {} gone", m_path);
			}
			else if(const auto it = m_frames.find(m->m_key);
			        it != m_frames.end())
			{
				Frame& f = it->second;
				if(m->m_type == Type::READY)
				{
					const auto now = Clock::now();
					const double ms =
					    std::chrono::duration<double, std::milli>(
					        now - (f.m_everReady ? f.m_shownAt : f.m_sentAt))
					        .count();
					smooth(f.m_everReady ? m_stats.m_rearmMs
					                     : m_stats.m_readyMs,
					       ms);
					f.m_name = m->m_name;
					f.m_armed = f.m_everReady = true;
				}
				else if(m->m_type == Type::DROP)
				{
					f.m_armed = false;
					f.m_dropped = true;
				}
			}
		}
		if(onChange)
		{
			onChange();
		}
		if(!m_connected)
		{
			return;
		}
	}
}

} // namespace rega::agent
