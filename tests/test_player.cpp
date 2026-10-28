#include "app/Player.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

using namespace rega;
using namespace std::chrono_literals;

namespace
{

// A Player on a clock the test moves by hand, at 10 fps (100 ms frames).
struct FakePlayer
{
	Player::Clock::time_point m_now{};
	Player m_player{{}, [this] { return m_now; }};
	int m_frame = 0;
	int m_frames = 10;
	bool m_ready = true;
	std::vector<int> m_prefetched;

	FakePlayer()
	{
		m_player.setFps(10);
		m_player.play();
	}
	// Move the clock by `dt`, then tick; true if the frame advanced.
	bool step(Player::Clock::duration dt, int ahead = 0)
	{
		m_now += dt;
		auto next = m_player.tick(
		    m_frame,
		    m_frames,
		    ahead,
		    [this](int f) { m_prefetched.push_back(f); },
		    [this](int) { return m_ready; });
		if(next)
		{
			m_frame = *next;
		}
		return next.has_value();
	}
};

} // namespace

TEST(Player, AdvancesOncePerPeriod)
{
	FakePlayer p;
	EXPECT_FALSE(p.step(50ms));
	EXPECT_TRUE(p.step(50ms)); // 100 ms
	EXPECT_EQ(p.m_frame, 1);
	// Four ticks per frame, as the ticker wakes the UI.
	int advances = 0;
	for(int i = 0; i < 40; ++i)
	{
		advances += p.step(25ms);
	}
	EXPECT_EQ(advances, 10);
	EXPECT_EQ(p.m_frame, 1); // 11 frames of 10: wrapped round
}

TEST(Player, StallsInsteadOfSkippingWhenTheNextFrameIsNotReady)
{
	FakePlayer p;
	p.m_ready = false;
	EXPECT_FALSE(p.step(100ms));
	EXPECT_FALSE(p.step(100ms));
	EXPECT_EQ(p.m_frame, 0);
	p.m_ready = true;
	EXPECT_TRUE(p.step(10ms));
	EXPECT_EQ(p.m_frame, 1); // the next one, not the one the clock is at
}

TEST(Player, CatchesUpAfterFallingFarBehind)
{
	FakePlayer p;
	// Over two periods late: the next frame counts from now.
	EXPECT_TRUE(p.step(500ms));
	EXPECT_FALSE(p.step(50ms));
	EXPECT_TRUE(p.step(50ms));
}

TEST(Player, KeepsCadenceWhenSlightlyLate)
{
	FakePlayer p;
	// 150 ms: advances, and the next one is due 100 ms after the last *due*
	// time, i.e. 50 ms from now.
	EXPECT_TRUE(p.step(150ms));
	EXPECT_TRUE(p.step(50ms));
}

TEST(Player, MeasuredFpsIsSmoothed)
{
	FakePlayer p;
	EXPECT_EQ(p.m_player.measuredFps(), 0.0);
	EXPECT_TRUE(p.step(100ms));
	EXPECT_NEAR(p.m_player.measuredFps(), 10.0, 1e-6);
	// 200 ms since the last advance, weighted 15% into the interval.
	EXPECT_TRUE(p.step(200ms));
	EXPECT_NEAR(p.m_player.measuredFps(),
	            1.0 / (0.1 * 0.85 + 0.2 * 0.15),
	            1e-6);
	// Pausing and playing again starts the measurement over.
	p.m_player.pause();
	p.m_player.play();
	EXPECT_EQ(p.m_player.measuredFps(), 0.0);
}

TEST(Player, MeasuredFpsIgnoresSteadyLateness)
{
	// The bug: every frame shown 5 ms after it was due, 100 ms apart, read
	// as 1 / 105 ms (9.5 fps) instead of the 10 fps actually shown.
	FakePlayer p;
	EXPECT_TRUE(p.step(105ms));
	for(int i = 0; i < 30; ++i)
	{
		EXPECT_TRUE(p.step(100ms));
	}
	EXPECT_NEAR(p.m_player.measuredFps(), 10.0, 0.01); // was 9.52
}

TEST(Player, MeasuredFpsIsNotInflatedByJitter)
{
	// Alternating 50 / 150 ms (10 fps on average, kept by the cadence):
	// averaging 1/interval would read 13.3 fps.
	FakePlayer p;
	EXPECT_TRUE(p.step(100ms));
	for(int i = 0; i < 40; ++i)
	{
		EXPECT_TRUE(p.step(150ms));
		EXPECT_TRUE(p.step(50ms));
	}
	EXPECT_NEAR(p.m_player.measuredFps(), 10.0, 0.5);
}

TEST(Player, NextDueIsOnePeriodAfterTheSchedule)
{
	FakePlayer p;
	const auto start = p.m_now;
	EXPECT_EQ(p.m_player.nextDue(), start + 100ms);
	EXPECT_TRUE(p.step(130ms)); // late: the schedule keeps its cadence
	EXPECT_EQ(p.m_player.nextDue(), start + 200ms);
	EXPECT_TRUE(p.step(500ms)); // far behind: counts from now
	EXPECT_EQ(p.m_player.nextDue(), p.m_now + 100ms);
}

TEST(Player, PrefetchesTheFramesAheadWrappingRound)
{
	FakePlayer p;
	p.m_frames = 5;
	p.m_frame = 3;
	p.step(10ms, 10); // more ahead than frames: each other frame once
	EXPECT_EQ(p.m_prefetched, (std::vector<int>{4, 0, 1, 2}));
	p.m_prefetched.clear();
	p.step(10ms, 2);
	EXPECT_EQ(p.m_prefetched, (std::vector<int>{4, 0}));
}

TEST(Player, ReadAheadIsClampedTo2To96)
{
	EXPECT_EQ(Player::readAhead(1000, 1e9, 1), Player::MIN_AHEAD);
	EXPECT_EQ(Player::readAhead(size_t(1) << 40, 10, 1), Player::MAX_AHEAD);
	// 60% of the budget, split between the sources.
	EXPECT_EQ(Player::readAhead(1000, 10, 2), 30);
	EXPECT_EQ(Player::readAhead(1000, 0, 0), Player::MAX_AHEAD); // no div 0
}

TEST(Player, TargetFpsPrefersTheChosenThenTheFile)
{
	Player p({});
	EXPECT_EQ(p.targetFps(std::nullopt), Player::DEFAULT_FPS);
	EXPECT_EQ(p.targetFps(25.0), 25.0);
	p.setFps(30);
	EXPECT_EQ(p.targetFps(25.0), 30.0);
	p.reset();
	EXPECT_EQ(p.fps(), 0.0);
	EXPECT_FALSE(p.playing());
}

TEST(Player, TickerWakesWhenTheFrameIsDue)
{
	// At 10 fps with nothing advancing, the first wake comes when frame 1
	// is due (100 ms), not a quarter period in. Timed at the wake itself: a
	// loaded CI machine can oversleep any sleep the test does.
	using Clock = std::chrono::steady_clock;
	std::atomic<Clock::rep> firstWake{0};
	Player p(
	    [&]
	    {
		    Clock::rep none = 0;
		    firstWake.compare_exchange_strong(
		        none, Clock::now().time_since_epoch().count());
	    });
	p.setFps(10);
	const auto start = Clock::now();
	p.play();
	const auto end = start + 5s;
	while(firstWake == 0 && Clock::now() < end)
	{
		std::this_thread::sleep_for(1ms);
	}
	p.pause();
	ASSERT_NE(firstWake.load(), 0);
	EXPECT_GE(Clock::time_point(Clock::duration(firstWake.load())) - start,
	          100ms);
}

TEST(Player, TickerWakesWhilePlayingAndStopsOnPause)
{
	std::atomic<int> wakes{0};
	Player p([&] { ++wakes; });
	p.play();
	ASSERT_TRUE(p.playing());
	const auto end = std::chrono::steady_clock::now() + 2s;
	while(wakes < 3 && std::chrono::steady_clock::now() < end)
	{
		std::this_thread::sleep_for(5ms);
	}
	EXPECT_GE(wakes.load(), 3);
	p.pause(); // joins the ticker
	const int after = wakes;
	std::this_thread::sleep_for(60ms);
	EXPECT_EQ(wakes.load(), after);
}
