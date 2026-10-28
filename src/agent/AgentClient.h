#pragma once

#include "agent/Protocol.h"
#include "image/Render.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rega::agent
{

/// Agent sockets to try, best first: LC_REGA_AGENT from each attached tmux
/// client (tmuxClientEnv), then our own, then the forwards in /tmp
/// (rega-agent-*.sock of ours, newest first: sshd may not accept the
/// variable). Duplicates dropped.
std::vector<std::string> agentSockets(bool tmux);
/// A connection to an agent: the connected fd and the agent's token, or fd -1
/// (dead: a ControlMaster keeps old forwards bound).
struct AgentLink
{
	int m_fd = -1;
	std::string m_token;
};
/// Connect to `path` and exchange HELLOs.
AgentLink connectAgent(const std::string& path, int timeoutMs = 2000);

/// Picture quality on the wire: JPEG 1-99, LOSSLESS (PNG), RAW (RGBA).
constexpr int QUALITY_LOSSLESS = 100;
constexpr int QUALITY_RAW = 0;
/// "q85", "lossless", "raw".
std::string qualityName(int quality);
/// `img` encoded for FRAME at `quality`.
std::pair<Codec, std::string> encodeFrame(const Rgba8Image& img, int quality);

/// The remote end of agent mode: sends rendered frames to `rega --agent` on
/// the machine with the terminal and tracks the shared memory names it holds
/// them under. A name is good for one showing (the terminal unlinks it once
/// read); the agent then arms the next one. Thread-safe.
class AgentClient
{
public:
	/// The first agent of agentSockets() that answers; null when none does.
	static std::shared_ptr<AgentClient> discover(bool tmux);
	AgentClient(AgentLink link, std::string path);
	/// Says BYE: the agent unlinks everything this connection held.
	~AgentClient();
	AgentClient(const AgentClient&) = delete;
	AgentClient& operator=(const AgentClient&) = delete;

	/// Called (on the reader thread) when a name is armed or the agent goes.
	void setOnChange(std::function<void()> fn);
	[[nodiscard]] const std::string& path() const noexcept
	{
		return m_path;
	}
	[[nodiscard]] bool connected() const noexcept
	{
		return m_connected;
	}
	void setQuality(int quality) noexcept
	{
		m_quality = quality;
	}
	[[nodiscard]] int quality() const noexcept
	{
		return m_quality;
	}

	/// Encode `img` and send it; the key it goes by, 0 when the agent is gone.
	/// Blocks while the link is full: that is the pacing.
	uint32_t send(const Rgba8Image& img);
	enum class Show
	{
		NOW,  ///< `name` is armed: show it now
		WAIT, ///< not armed yet (on its way, or being re-armed)
		GONE  ///< dropped, or the agent is gone: send the picture again
	};
	/// The name to show `key` under now. A NOW name is used up: the agent
	/// arms the next one once the terminal read it. One the terminal never
	/// read (STALE_AFTER on) is handed out again.
	Show take(uint32_t key, std::string& name);
	/// Done with `key`: the agent may let it go.
	void forget(uint32_t key);
	static constexpr auto STALE_AFTER = std::chrono::seconds(1);

	/// For the stats overlay (S).
	struct Stats
	{
		size_t m_sent = 0, m_shown = 0, m_stale = 0, m_held = 0;
		uint64_t m_bytes = 0, m_lastBytes = 0;
		int m_lastW = 0, m_lastH = 0;
		double m_readyMs = 0, m_rearmMs = 0; ///< smoothed
	};
	[[nodiscard]] Stats stats() const;

private:
	using Clock = std::chrono::steady_clock;
	struct Frame
	{
		std::string m_name;
		bool m_armed = false, m_dropped = false, m_everReady = false;
		Clock::time_point m_sentAt, m_shownAt;
	};
	bool write(const Message& m);
	void readLoop();

	const int m_fd;
	const std::string m_path;
	std::atomic<bool> m_connected{true};
	std::atomic<int> m_quality{85};
	std::mutex m_writeMutex; ///< one message at a time on the socket
	mutable std::mutex m_mutex;
	std::unordered_map<uint32_t, Frame> m_frames;
	uint32_t m_nextKey = 1;
	Stats m_stats;
	std::function<void()> m_onChange;
	std::thread m_reader;
};

} // namespace rega::agent
