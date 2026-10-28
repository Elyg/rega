#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rega::agent
{

/// Frames held as POSIX shared memory for the terminal to read (kitty t=s),
/// in RAM only: mmap'd and mlock'd, never in a file. The terminal unlinks a
/// name once it read it; the mapping outlives that, so rearm() copies it into
/// a new object under the next generation's name for the next showing.
class ShmStore
{
public:
	/// @param token  name prefix, unique to this store: names are
	///               "/re<token>-<key>-<gen>" (macOS allows 31 characters).
	/// @param budget bytes held at most; past it the least recently put or
	///               re-armed frames go (0: no limit).
	ShmStore(std::string token, uint64_t budget);
	/// Unlinks and unmaps everything (clear()).
	~ShmStore();
	ShmStore(const ShmStore&) = delete;
	ShmStore& operator=(const ShmStore&) = delete;

	struct Put
	{
		std::string m_name; ///< empty: shared memory could not be made
		std::vector<uint32_t> m_dropped; ///< keys evicted to make room
	};
	/// Hold `n` bytes of RGBA for `key` (replacing what it held) under a
	/// fresh name.
	Put put(uint32_t key, const uint8_t* rgba, size_t n);
	/// The name `key` is under now; empty when not held.
	[[nodiscard]] std::string name(uint32_t key) const;
	/// Check `key`'s name from now on: it went to the terminal, which unlinks
	/// it once read.
	void watch(uint32_t key);
	/// For every watched name the terminal unlinked: a copy under the next
	/// name. Returns those keys and names; an empty name: the copy failed
	/// and the frame is dropped.
	std::vector<std::pair<uint32_t, std::string>> rearm();
	[[nodiscard]] const std::string& token() const noexcept
	{
		return m_token;
	}
	[[nodiscard]] bool watching() const noexcept
	{
		return !m_watched.empty();
	}
	void drop(uint32_t key);
	/// Unlink every name and unmap every frame.
	void clear();

	[[nodiscard]] uint64_t bytes() const noexcept
	{
		return m_bytes;
	}
	[[nodiscard]] size_t frames() const noexcept
	{
		return m_frames.size();
	}
	/// Bytes mlock refused (they may be paged out to swap; E5).
	[[nodiscard]] uint64_t unlockedBytes() const noexcept
	{
		return m_unlocked;
	}

private:
	struct Frame
	{
		void* m_map = nullptr;
		size_t m_size = 0;
		uint32_t m_gen = 0;
		bool m_locked = false;
		std::string m_name;
		std::list<uint32_t>::iterator m_lru;
	};
	/// A new object holding `n` bytes from `src` under `name`, mapped and
	/// locked into `f`. False on failure (nothing left behind).
	bool create(Frame& f, const std::string& name, const void* src, size_t n);
	void release(Frame& f);
	std::string nameFor(uint32_t key, uint32_t gen) const;

	std::string m_token;
	uint64_t m_budget;
	uint64_t m_bytes = 0, m_unlocked = 0;
	std::unordered_map<uint32_t, Frame> m_frames;
	std::list<uint32_t> m_lru; ///< oldest first
	/// Watched keys: since when, and whether its being unread was logged.
	struct Watch
	{
		std::chrono::steady_clock::time_point m_since;
		bool m_logged = false;
	};
	std::unordered_map<uint32_t, Watch> m_watched;
};

/// `rega --agent [options] -- ssh <args>`: what runAgent() is given.
struct AgentOptions
{
	std::vector<std::string> m_command; ///< ssh and its arguments
	uint64_t m_budget = 0;              ///< per connection; 0 no limit
	std::string m_log; ///< where to log while ssh has the terminal ("" none)
};

/// The ssh command with the forward and env var added: ssh, then `-R
/// <remote>:<local> -o SetEnv=LC_REGA_AGENT=<remote> -o
/// SendEnv=LC_REGA_AGENT`, then the rest (runAgent sets the variable too).
std::vector<std::string> sshCommand(const std::vector<std::string>& command,
                                    const std::string& remoteSocket,
                                    const std::string& localSocket);

/// Run ssh with the socket forwarded, serving frames until it exits; then
/// unlink everything. Returns ssh's exit status.
int runAgent(const AgentOptions& opt);

/// `rega --shell-init bash|zsh`: shell code defining `rega_ssh`, which runs
/// an interactive ssh under `rega --agent` (`rega` is its path) and any
/// other as it is, and an `ssh` calling it unless one is defined already
/// (that one calls rega_ssh itself). REGA_NO_AGENT=1 turns it off. Empty
/// for another shell.
std::string shellInit(const std::string& shell, const std::string& rega);

} // namespace rega::agent
