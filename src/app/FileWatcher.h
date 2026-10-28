#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace rega
{

/// Polls paths on its own thread (once per `interval`) and reports what changed
/// on disk. A watched directory reports each entry added, removed or changed
/// (size / mtime) and itself; a watched file reports itself. Paths start being
/// compared from the first poll after watch(); nothing is reported for them
/// before that.
class FileWatcher
{
public:
	using Paths = std::vector<std::filesystem::path>;
	/// Called on the watcher thread with the paths that changed.
	using ChangedFn = std::function<void(Paths changed)>;

	FileWatcher(std::chrono::milliseconds interval, ChangedFn onChanged);
	~FileWatcher(); ///< stops and joins the thread
	FileWatcher(const FileWatcher&) = delete;
	FileWatcher& operator=(const FileWatcher&) = delete;

	/// Replace the watched set. Safe from any thread.
	void watch(Paths paths);
	/// Stop polling and join the thread (also done on destruction).
	void stop();
	bool running() const noexcept
	{
		return m_thread.joinable();
	}

private:
	struct Stamp
	{
		bool m_exists = false;
		std::uintmax_t m_size = 0;
		std::filesystem::file_time_type m_mtime{};
		bool operator==(const Stamp&) const = default;
	};
	/// A file, or every entry of a directory (by path), plus the path itself.
	using Snapshot = std::map<std::filesystem::path, Stamp>;
	static Snapshot snapshot(const std::filesystem::path& p);
	void run(std::stop_token stop);

	std::chrono::milliseconds m_interval;
	ChangedFn m_onChanged;
	std::mutex m_mu;
	std::condition_variable_any m_cv;
	Paths m_paths; ///< guarded by m_mu
	/// Last snapshot per watched path (watcher thread only).
	std::map<std::filesystem::path, Snapshot> m_last;
	std::jthread m_thread; ///< last: stops before the rest goes
};

} // namespace rega
