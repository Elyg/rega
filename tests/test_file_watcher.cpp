#include "TestContext.h"
#include "app/FileWatcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>

using namespace rega;
using namespace regatest;
using namespace std::chrono_literals;

namespace
{

// Collects what a FileWatcher reports (from its thread).
struct Changes
{
	std::mutex m_mu;
	std::vector<fs::path> m_paths;
	int m_calls = 0;

	FileWatcher::ChangedFn fn()
	{
		return [this](FileWatcher::Paths changed)
		{
			std::lock_guard lock(m_mu);
			++m_calls;
			m_paths.insert(m_paths.end(), changed.begin(), changed.end());
		};
	}
	bool has(const fs::path& p)
	{
		std::lock_guard lock(m_mu);
		return std::ranges::find(m_paths, p) != m_paths.end();
	}
	int calls()
	{
		std::lock_guard lock(m_mu);
		return m_calls;
	}
	void clear()
	{
		std::lock_guard lock(m_mu);
		m_paths.clear();
		m_calls = 0;
	}
};

constexpr auto INTERVAL = 10ms;

// Long enough for a few polls: the first one takes the baseline.
void settle()
{
	std::this_thread::sleep_for(INTERVAL * 6);
}

} // namespace

TEST(FileWatcher, ReportsFilesAddedChangedAndRemovedInAWatchedDir)
{
	const fs::path dir = freshDir("watch-dir");
	const fs::path a = touch(dir / "a.exr", "1");
	Changes changes;
	FileWatcher w(INTERVAL, changes.fn());
	w.watch({dir});
	settle();
	EXPECT_EQ(changes.calls(), 0); // nothing yet

	const fs::path b = touch(dir / "b.exr");
	ASSERT_TRUE(waitFor([&] { return changes.has(b); }));
	EXPECT_TRUE(changes.has(dir)); // the listing changed too
	EXPECT_FALSE(changes.has(a));

	changes.clear();
	touch(a, "longer"); // re-rendered in place
	ASSERT_TRUE(waitFor([&] { return changes.has(a); }));

	changes.clear();
	fs::remove(b);
	ASSERT_TRUE(waitFor([&] { return changes.has(b); }));
}

TEST(FileWatcher, WatchesASingleFile)
{
	const fs::path dir = freshDir("watch-file");
	const fs::path f = touch(dir / "f.exr", "1");
	const fs::path other = touch(dir / "other.exr");
	Changes changes;
	FileWatcher w(INTERVAL, changes.fn());
	w.watch({f});
	settle();
	touch(other, "not watched");
	touch(f, "22");
	ASSERT_TRUE(waitFor([&] { return changes.has(f); }));
	EXPECT_FALSE(changes.has(other));
}

TEST(FileWatcher, NewlyWatchedPathsStartFromTheirCurrentState)
{
	const fs::path dir = freshDir("watch-new");
	const fs::path f = touch(dir / "f.exr", "1");
	Changes changes;
	FileWatcher w(INTERVAL, changes.fn());
	settle();
	touch(f, "changed before it was watched");
	w.watch({dir});
	settle();
	EXPECT_EQ(changes.calls(), 0);
}

TEST(FileWatcher, StopJoinsTheThreadAndReportsNothingMore)
{
	const fs::path dir = freshDir("watch-stop");
	Changes changes;
	FileWatcher w(INTERVAL, changes.fn());
	w.watch({dir});
	settle();
	EXPECT_TRUE(w.running());
	w.stop();
	EXPECT_FALSE(w.running());
	touch(dir / "late.exr");
	std::this_thread::sleep_for(INTERVAL * 5);
	EXPECT_EQ(changes.calls(), 0);
	w.stop(); // again: harmless
}

TEST(FileWatcher, StopsPromptlyEvenWithALongInterval)
{
	Changes changes;
	const auto start = std::chrono::steady_clock::now();
	{
		FileWatcher w(std::chrono::hours(1), changes.fn());
	}
	EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}
