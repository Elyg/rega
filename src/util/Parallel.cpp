#include "util/Parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace rega
{

namespace
{

thread_local bool IN_POOL = false;

class Pool
{
public:
	Pool()
	{
		unsigned n = std::max(2u, std::thread::hardware_concurrency()) - 1;
		for(unsigned i = 0; i < n; ++i)
		{
			m_workers.emplace_back([this](std::stop_token st) { work(st); });
		}
	}

	~Pool()
	{
		for(auto& w : m_workers)
		{
			w.request_stop();
		}
		m_cv.notify_all();
	}

	int size() const
	{
		return static_cast<int>(m_workers.size());
	}

	void submit(std::function<void()> task)
	{
		{
			std::lock_guard lk(m_mu);
			m_tasks.push_back(std::move(task));
		}
		m_cv.notify_one();
	}

private:
	void work(std::stop_token st)
	{
		IN_POOL = true;
		for(;;)
		{
			std::function<void()> task;
			{
				std::unique_lock lk(m_mu);
				if(!m_cv.wait(lk, st, [&] { return !m_tasks.empty(); }))
				{
					return; // stop requested
				}
				task = std::move(m_tasks.front());
				m_tasks.pop_front();
			}
			task();
		}
	}

	std::mutex m_mu;
	std::condition_variable_any m_cv;
	std::deque<std::function<void()>> m_tasks;
	std::vector<std::jthread> m_workers; // last: joined first on destruction
};

Pool& pool()
{
	static Pool p;
	return p;
}

} // namespace

int parallelism()
{
	return pool().size() + 1;
}

void parallelFor(int n, int grain, const std::function<void(int, int)>& fn)
{
	if(n <= 0)
	{
		return;
	}
	grain = std::max(1, grain);
	const int chunks = (n + grain - 1) / grain;
	if(chunks == 1 || IN_POOL)
	{
		fn(0, n);
		return;
	}
	const int helpers = std::min(chunks, parallelism()) - 1;
	std::atomic<int> next{0};
	auto drain = [&]
	{
		for(int c; (c = next.fetch_add(1)) < chunks;)
		{
			fn(c * grain, std::min(n, (c + 1) * grain));
		}
	};
	std::latch done(helpers);
	for(int i = 0; i < helpers; ++i)
	{
		pool().submit(
		    [&]
		    {
			    drain();
			    done.count_down();
		    });
	}
	drain();
	done.wait();
}

} // namespace rega
