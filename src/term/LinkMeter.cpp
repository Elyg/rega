#include "term/LinkMeter.h"

#include <algorithm>
#include <cctype>

namespace rega::kitty
{

namespace
{

using Seconds = std::chrono::duration<double>;

// A sample from fewer bytes, or over less time, is mostly noise: the
// link's own jitter, the terminal's scheduling.
constexpr uint64_t MIN_SAMPLE_BYTES = 32 * 1024;
constexpr double MIN_SAMPLE_SECONDS = 0.005;
// How often a bare probe re-times the round trip while nothing is sent.
constexpr std::chrono::seconds BARE_INTERVAL{5};
// Round trips kept: the shortest of the last few, so a route that got
// slower is noticed.
constexpr size_t BARE_TRIPS = 8;
// COUNTER: polls closer than this say little about a rate.
constexpr double MIN_POLL_SECONDS = 0.02;
// A link reads slower than it is far more often than faster (an idle tail,
// TCP slow start after a pause): keep the peak, let it fade slowly so a
// link that did get slower is followed.
constexpr double PEAK_DECAY = 0.95;
// COUNTER: a link that keeps up with the pictures has room to spare, but
// how much shows only once it is full. Assume a little more each poll it
// keeps up, until it doesn't and a sample says.
constexpr double PROBE_GROWTH = 1.05;
constexpr double MAX_BOOST = 2.0;

} // namespace

void LinkMeter::setGuess(double guess)
{
	m_guess = std::max(0.0, guess);
}

void LinkMeter::setMode(Mode mode)
{
	if(mode == m_mode)
	{
		return;
	}
	m_mode = mode;
	m_through = m_probedUpTo = m_written;
	m_probes.clear();
	m_linkFree = {};
	m_lastPoll = {};
	m_pendingAtPoll = 0;
	m_writtenAtPoll = m_written;
	m_boost = 1;
}

void LinkMeter::wrote(size_t bytes, Clock::time_point now)
{
	m_written += bytes;
	if(m_mode == Mode::ESTIMATE && m_guess > 0)
	{
		// At what was measured, when it was: a counter that went away.
		m_linkFree = std::max(now, m_linkFree) +
		             std::chrono::duration_cast<Clock::duration>(
		                 Seconds(static_cast<double>(bytes) / rate()));
	}
	dropLost(now);
}

bool LinkMeter::wantsProbe(Clock::time_point now) const
{
	if(m_mode != Mode::ACK || m_guess <= 0)
	{
		return false;
	}
	return m_written > m_probedUpTo ||
	       (m_probes.empty() && now - m_lastBare >= BARE_INTERVAL);
}

void LinkMeter::probed(Clock::time_point now)
{
	dropLost(now);
	const bool bare = m_probes.empty() && m_through >= m_written;
	m_probes.push_back({now, m_written, bare});
	m_probedUpTo = m_written;
	if(bare)
	{
		m_lastBare = now;
	}
}

void LinkMeter::answered(Clock::time_point now)
{
	if(m_probes.empty())
	{
		return; // one given up on, answered after all
	}
	const Probe p = m_probes.front();
	m_probes.pop_front();
	const double elapsed = Seconds(now - p.m_sent).count();
	const uint64_t bytes =
	    p.m_written > m_through ? p.m_written - m_through : 0;
	if(p.m_bare)
	{
		m_bareTrips.push_back(elapsed);
		if(m_bareTrips.size() > BARE_TRIPS)
		{
			m_bareTrips.pop_front();
		}
	}
	else if(bytes >= MIN_SAMPLE_BYTES)
	{
		// The link carried these from when they reached the far end of it
		// (sent + the round trip, a link standing idle) or when it was
		// through the ones before (a link kept busy), until now.
		const auto rtt =
		    std::chrono::duration_cast<Clock::duration>(Seconds(roundTrip()));
		const auto start = std::max(p.m_sent + rtt, m_lastAnswer);
		const double busy = Seconds(now - start).count();
		if(busy >= MIN_SAMPLE_SECONDS)
		{
			sample(static_cast<double>(bytes) / busy);
		}
	}
	m_through = std::max(m_through, p.m_written);
	m_lastAnswer = now;
}

void LinkMeter::delivered(uint64_t bytes,
                          Clock::time_point now,
                          double roundTrip)
{
	const uint64_t pending = m_written - m_through;
	const uint64_t fresh = m_written - m_writtenAtPoll; // since the last poll
	if(m_lastPoll != Clock::time_point{} && pending > 0)
	{
		const double dt = Seconds(now - m_lastPoll).count();
		if(bytes == 0 && m_pendingAtPoll > 0)
		{
			// Nothing acknowledged in a whole poll of something on its way
			// (not just written, with no time to get anywhere): nothing is.
			// What was counted ahead (escapes tmux unwraps, ssh compression)
			// is through after all.
			m_through = m_written;
		}
		else if(bytes < m_pendingAtPoll && dt >= MIN_POLL_SECONDS)
		{
			// Not through what was already on its way when the interval
			// began: busy all along. Anything less (a frame written halfway,
			// the link idle before it) would read the link slower than it is.
			sample(static_cast<double>(bytes) / dt);
		}
		else if(m_measured > 0 && fresh >= MIN_SAMPLE_BYTES)
		{
			// Through the pictures as fast as they came: room to spare.
			m_boost = std::min(MAX_BOOST, m_boost * PROBE_GROWTH);
		}
	}
	// The connection also carried the screen's text and ssh's own framing:
	// never more than was written.
	m_through = std::min(m_written, m_through + bytes);
	m_lastPoll = now;
	m_pendingAtPoll = m_written - m_through;
	m_writtenAtPoll = m_written;
	m_counterTrip = roundTrip;
}

double LinkMeter::rate() const
{
	if(m_guess <= 0)
	{
		return 0;
	}
	return m_measured > 0 ? m_measured * m_boost : m_guess;
}

double LinkMeter::roundTrip() const
{
	if(m_mode == Mode::COUNTER)
	{
		return m_counterTrip;
	}
	return m_bareTrips.empty()
	           ? 0.0
	           : *std::min_element(m_bareTrips.begin(), m_bareTrips.end());
}

double LinkMeter::backlog(Clock::time_point now) const
{
	if(m_guess <= 0)
	{
		return 0;
	}
	switch(m_mode)
	{
		case Mode::ESTIMATE:
			return m_linkFree > now ? Seconds(m_linkFree - now).count() : 0.0;
		case Mode::ACK:
		{
			uint64_t through = m_through;
			for(const Probe& p : m_probes)
			{
				if(now - p.m_sent > PROBE_TIMEOUT)
				{
					through = std::max(through, p.m_written);
				}
			}
			// A round trip's worth is on its way, not waiting: keeping it
			// in flight keeps the link busy between frames.
			const double s =
			    static_cast<double>(m_written - through) / rate() - roundTrip();
			return std::max(0.0, s);
		}
		case Mode::COUNTER:
		{
			// Polls are 100 ms apart: between them the link goes on carrying
			// what it had at the last one (not what came since). Waiting for
			// the next poll held playback to ten frames a second.
			const double r = rate();
			double pending = static_cast<double>(m_written - m_through);
			if(m_lastPoll != Clock::time_point{} && now > m_lastPoll)
			{
				pending -= std::min(static_cast<double>(m_pendingAtPoll),
				                    r * Seconds(now - m_lastPoll).count());
			}
			// What a round trip holds is in flight, its acknowledgement on
			// its way back: waiting for it idled the link a round trip a
			// frame.
			return std::max(0.0, pending / r - m_counterTrip);
		}
	}
	return 0;
}

bool LinkMeter::isAnswer(std::string_view in)
{
	if(in.size() < 5 || !in.starts_with("\x1b[?") || in.back() != 'c')
	{
		return false;
	}
	return std::all_of(in.begin() + 3,
	                   in.end() - 1,
	                   [](char c)
	                   {
		                   return std::isdigit(static_cast<unsigned char>(c)) ||
		                          c == ';';
	                   });
}

void LinkMeter::sample(double bytesPerSecond)
{
	m_measured = std::max(bytesPerSecond, m_measured * PEAK_DECAY);
	m_boost = 1;
}

void LinkMeter::dropLost(Clock::time_point now)
{
	while(!m_probes.empty() && now - m_probes.front().m_sent > PROBE_TIMEOUT)
	{
		m_through = std::max(m_through, m_probes.front().m_written);
		m_probes.pop_front();
	}
}

} // namespace rega::kitty
