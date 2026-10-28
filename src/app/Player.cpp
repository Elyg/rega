#include "app/Player.h"

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace rega
{

Player::Player(std::function<void()> wake, NowFn now)
    : m_wake(std::move(wake)), m_now(std::move(now))
{
}

Player::~Player()
{
	stopTicker();
}

void Player::play()
{
	m_playing = true;
	m_lastAdvance = m_now();
	m_lastShown = m_lastAdvance;
	m_measuredFps = 0;
	m_interval = 0;
	setDue(m_fps > 0 ? m_fps : DEFAULT_FPS);
	startTicker();
}

void Player::pause()
{
	m_playing = false;
	stopTicker();
}

void Player::reset()
{
	pause();
	m_measuredFps = 0;
	m_fps = 0;
}

int Player::readAhead(std::size_t budgetBytes,
                      double frameBytes,
                      std::size_t sources) noexcept
{
	const double perSource =
	    static_cast<double>(budgetBytes) * 0.6 / std::max(1.0, frameBytes) /
	    static_cast<double>(std::max<std::size_t>(1, sources));
	// Clamp before converting: a double past INT_MAX makes the cast undefined
	// (x86 gives INT_MIN, so a huge budget read ahead the least).
	return static_cast<int>(std::clamp(perSource,
	                                   static_cast<double>(MIN_AHEAD),
	                                   static_cast<double>(MAX_AHEAD)));
}

double Player::sinceAdvance(Clock::time_point now, double fps)
{
	m_tickerIntervalMs = std::max(2, static_cast<int>(1000.0 / fps / 4));
	return std::chrono::duration<double>(now - m_lastAdvance).count();
}

void Player::setDue(double fps)
{
	const auto due =
	    m_lastAdvance + std::chrono::duration_cast<Clock::duration>(
	                        std::chrono::duration<double>(1.0 / fps));
	m_due = due.time_since_epoch().count();
}

void Player::advanced(Clock::time_point now, double since, double fps)
{
	// Measured against the previous advance, not the schedule: each frame
	// is a little late, and against the schedule that lateness read as a
	// slow clock (22/25 fps shown while it really ran at 25).
	// The interval is smoothed, not its inverse: averaging 1/interval
	// over jittery intervals reads high (24.8/24).
	if(m_lastShown)
	{
		const double interval =
		    std::max(1e-6,
		             std::chrono::duration<double>(now - *m_lastShown).count());
		m_interval =
		    m_interval <= 0 ? interval : m_interval * 0.85 + interval * 0.15;
		m_measuredFps = 1.0 / m_interval;
	}
	m_lastShown = now;
	// Keep cadence: advance the reference by one period unless we fell far
	// behind.
	m_lastAdvance =
	    since > 2.0 / fps
	        ? now
	        : m_lastAdvance + std::chrono::duration_cast<Clock::duration>(
	                              std::chrono::duration<double>(1.0 / fps));
	setDue(fps);
}

void Player::startTicker()
{
	stopTicker();
	if(!m_wake)
	{
		return;
	}
	m_ticker = std::jthread(
	    [this](std::stop_token stop)
	    {
		    std::mutex mu;
		    std::condition_variable_any cv;
		    std::unique_lock lock(mu);
		    while(!stop.stop_requested())
		    {
			    // Sleep until the next frame is due. Past it (stalled, or
			    // the UI has not got to it yet), poll a few times a frame.
			    // Both return early when stopped.
			    const auto due = nextDue();
			    if(due > Clock::now())
			    {
				    cv.wait_until(lock, stop, due, [] { return false; });
			    }
			    else
			    {
				    cv.wait_for(lock,
				                stop,
				                std::chrono::milliseconds(
				                    m_tickerIntervalMs.load()),
				                [] { return false; });
			    }
			    if(!stop.stop_requested())
			    {
				    m_wake();
			    }
		    }
	    });
}

void Player::stopTicker()
{
	if(m_ticker.joinable())
	{
		m_ticker.request_stop();
		m_ticker.join();
	}
}

} // namespace rega
