#include "app/FileWatcher.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace rega
{

FileWatcher::FileWatcher(std::chrono::milliseconds interval,
                         ChangedFn onChanged)
    : m_interval(interval), m_onChanged(std::move(onChanged)),
      m_thread([this](std::stop_token stop) { run(stop); })
{
}

FileWatcher::~FileWatcher()
{
	stop();
}

void FileWatcher::watch(Paths paths)
{
	std::lock_guard lock(m_mu);
	m_paths = std::move(paths);
}

void FileWatcher::stop()
{
	if(m_thread.joinable())
	{
		m_thread.request_stop();
		m_thread.join();
	}
}

FileWatcher::Snapshot FileWatcher::snapshot(const fs::path& p)
{
	auto stampOf = [](const fs::directory_entry& e)
	{
		std::error_code ec;
		Stamp s;
		s.m_exists = e.exists(ec);
		if(s.m_exists)
		{
			s.m_mtime = e.last_write_time(ec);
			s.m_size = e.is_regular_file(ec) ? e.file_size(ec) : 0;
		}
		return s;
	};
	Snapshot snap;
	std::error_code ec;
	const fs::directory_entry self(p, ec);
	snap[p] = stampOf(self);
	if(self.is_directory(ec))
	{
		for(fs::directory_iterator it(p, ec), end; !ec && it != end;
		    it.increment(ec))
		{
			// The listing's own path form (dir / name), as the browser has
			// it.
			snap[p / it->path().filename()] = stampOf(*it);
		}
		// The directory's own mtime changes with every entry added or
		// removed, which the entries already say.
		snap[p].m_mtime = {};
	}
	return snap;
}

void FileWatcher::run(std::stop_token stop)
{
	std::unique_lock lock(m_mu);
	while(!stop.stop_requested())
	{
		// Returns early when stopped.
		m_cv.wait_for(lock, stop, m_interval, [] { return false; });
		if(stop.stop_requested())
		{
			break;
		}
		const Paths paths = m_paths;
		lock.unlock();

		Paths changed;
		std::map<fs::path, Snapshot> now;
		for(const auto& p : paths)
		{
			Snapshot snap = snapshot(p);
			if(auto it = m_last.find(p); it != m_last.end())
			{
				const Snapshot& before = it->second;
				bool any = false;
				for(const auto& [path, stamp] : snap)
				{
					auto b = before.find(path);
					if(b == before.end() || !(b->second == stamp))
					{
						changed.push_back(path);
						any = true;
					}
				}
				for(const auto& [path, stamp] : before)
				{
					if(!snap.contains(path))
					{
						changed.push_back(path); // gone
						any = true;
					}
				}
				if(any && std::ranges::find(changed, p) == changed.end())
				{
					changed.push_back(p);
				}
			}
			now.emplace(p, std::move(snap));
		}
		m_last = std::move(now);
		if(!changed.empty() && m_onChanged)
		{
			m_onChanged(std::move(changed));
		}
		lock.lock();
	}
}

} // namespace rega
