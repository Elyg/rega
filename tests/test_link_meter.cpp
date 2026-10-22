// Measuring the link to a terminal over ssh: probes the terminal answers,
// or what tmux wrote to its clients.

#include "term/Caps.h"
#include "term/Kitty.h"
#include "term/LinkMeter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace rv;
using namespace std::chrono_literals;
using kitty::LinkMeter;
using Mode = LinkMeter::Mode;

namespace
{

const LinkMeter::Clock::time_point T0 = LinkMeter::Clock::time_point{} + 1h;

LinkMeter meter(Mode mode, double guess = 4e6)
{
	LinkMeter m;
	m.setGuess(guess);
	m.setMode(mode);
	return m;
}

} // namespace

TEST(LinkMeter, UnpacedMeasuresAndWaitsForNothing)
{
	LinkMeter m = meter(Mode::ACK, 0);
	m.wrote(1'000'000, T0);
	EXPECT_FALSE(m.wantsProbe(T0));
	EXPECT_EQ(m.rate(), 0.0);
	EXPECT_EQ(m.backlog(T0), 0.0);
}

TEST(LinkMeter, EstimateDrainsAtTheGuess)
{
	LinkMeter m = meter(Mode::ESTIMATE, 1e6);
	m.wrote(500'000, T0);
	EXPECT_DOUBLE_EQ(m.backlog(T0), 0.5);
	EXPECT_DOUBLE_EQ(m.backlog(T0 + 200ms), 0.3);
	EXPECT_EQ(m.backlog(T0 + 1s), 0.0);
	EXPECT_FALSE(m.wantsProbe(T0)); // nothing comes back to ask
}

TEST(LinkMeter, AnswersTimeTheLinkPastTheRoundTrip)
{
	LinkMeter m = meter(Mode::ACK);
	// First, nothing ahead: the answer times the round trip alone.
	ASSERT_TRUE(m.wantsProbe(T0));
	m.probed(T0);
	m.answered(T0 + 50ms);
	EXPECT_DOUBLE_EQ(m.roundTrip(), 0.05);
	EXPECT_FALSE(m.measured());
	EXPECT_FALSE(m.wantsProbe(T0 + 60ms)); // nothing written since

	// 2 MB on an idle 20 MB/s link: back 50 ms + 100 ms later.
	const auto t = T0 + 1s;
	m.wrote(2'000'000, t);
	ASSERT_TRUE(m.wantsProbe(t));
	m.probed(t);
	EXPECT_GT(m.backlog(t), 0.0); // at the guess, 4 MB/s, until measured
	m.answered(t + 150ms);
	EXPECT_TRUE(m.measured());
	EXPECT_NEAR(m.rate(), 20e6, 1e3);
	EXPECT_EQ(m.backlog(t + 150ms), 0.0);
}

TEST(LinkMeter, QueuedFramesAreTimedFromTheAnswerBefore)
{
	LinkMeter m = meter(Mode::ACK);
	m.probed(T0);
	m.answered(T0 + 20ms); // round trip
	// Two 1 MB frames back to back on a 10 MB/s link: the second is
	// through 100 ms after the first, whatever the round trip.
	const auto t = T0 + 1s;
	m.wrote(1'000'000, t);
	m.probed(t);
	m.wrote(1'000'000, t + 1ms);
	m.probed(t + 1ms);
	m.answered(t + 120ms);
	EXPECT_NEAR(m.rate(), 10e6, 1e3);
	m.answered(t + 220ms);
	EXPECT_NEAR(m.rate(), 10e6, 1e3);
	EXPECT_EQ(m.backlog(t + 220ms), 0.0);
}

TEST(LinkMeter, BacklogLeavesARoundTripInFlight)
{
	// So the next frame goes before the last is answered: the link stays
	// busy instead of idling a round trip a frame.
	LinkMeter m = meter(Mode::ACK, 10e6);
	m.probed(T0);
	m.answered(T0 + 40ms);
	const auto t = T0 + 1s;
	m.wrote(1'000'000, t); // 100 ms of link
	m.probed(t);
	EXPECT_NEAR(m.backlog(t), 0.06, 1e-9);
}

TEST(LinkMeter, AnUnansweredProbeIsGivenUp)
{
	LinkMeter m = meter(Mode::ACK, 1e6);
	m.probed(T0);
	m.answered(T0 + 10ms);
	m.wrote(1'000'000, T0 + 1s);
	m.probed(T0 + 1s);
	EXPECT_GT(m.backlog(T0 + 2s), 0.0);
	// Past the timeout its bytes count as through: playback can't stall
	// on a terminal that never answers.
	EXPECT_EQ(m.backlog(T0 + 1s + LinkMeter::PROBE_TIMEOUT + 1ms), 0.0);
	m.wrote(10, T0 + 5s);
	m.answered(T0 + 5s); // late, for the one given up on: ignored
	EXPECT_FALSE(m.measured());
}

TEST(LinkMeter, SmallWritesTeachNothing)
{
	LinkMeter m = meter(Mode::ACK);
	m.probed(T0);
	m.answered(T0 + 10ms);
	m.wrote(1000, T0 + 1s); // a delete, a few placements
	m.probed(T0 + 1s);
	m.answered(T0 + 1s + 500ms); // a slow answer says nothing of the link
	EXPECT_FALSE(m.measured());
}

TEST(LinkMeter, CountsWhatTheConnectionCarried)
{
	LinkMeter m = meter(Mode::COUNTER);
	EXPECT_FALSE(m.wantsProbe(T0)); // a tmux of ours would answer itself
	m.delivered(0, T0);             // the first poll: a start
	m.wrote(3'000'000, T0 + 10ms);
	// Written partway through: how long the link stood idle before is
	// unknown, so this interval says nothing of its rate.
	m.delivered(100'000, T0 + 50ms);
	EXPECT_FALSE(m.measured());
	m.delivered(800'000, T0 + 150ms); // behind all along: 8 MB/s
	EXPECT_NEAR(m.rate(), 8e6, 1e3);
	EXPECT_NEAR(m.backlog(T0 + 150ms), 2.1e6 / 8e6, 1e-9);
	m.delivered(800'000, T0 + 250ms);
	EXPECT_NEAR(m.rate(), 8e6, 1e3);
	// Caught up within the interval: a lower bound, not a rate.
	m.delivered(1'500'000, T0 + 350ms);
	EXPECT_NEAR(m.rate(), 8e6, 1e3);
	EXPECT_EQ(m.backlog(T0 + 350ms), 0.0);
}

TEST(LinkMeter, AnIdleLinkDoesNotReadAsASlowOne)
{
	// The spiral seen over ssh: small frames leave the link idle most of
	// each poll; read as its rate, the next frames shrink, and so on.
	LinkMeter m = meter(Mode::COUNTER);
	m.delivered(0, T0);
	m.wrote(3'000'000, T0 + 10ms);
	m.delivered(0, T0 + 20ms);
	m.delivered(1'000'000, T0 + 120ms); // 10 MB/s, busy all along
	ASSERT_NEAR(m.rate(), 10e6, 1e3);
	m.delivered(2'000'000, T0 + 320ms); // through it all: no sample
	EXPECT_NEAR(m.rate(), 10e6, 1e3);
	// A 100 KB frame per poll, through within it: room to spare, so a
	// little more is assumed each time (up to twice what was measured).
	double last = m.rate();
	for(int i = 1; i <= 30; ++i)
	{
		const auto t = T0 + 320ms + i * 100ms;
		m.wrote(100'000, t - 50ms);
		m.delivered(100'000, t);
		EXPECT_GE(m.rate(), last);
		last = m.rate();
	}
	EXPECT_NEAR(m.rate(), 20e6, 1e3);
	// Until the link fills and says: the peak fades towards what it says.
	const auto t = T0 + 4s;
	m.wrote(5'000'000, t);
	m.delivered(100'000, t + 10ms);
	m.delivered(900'000, t + 110ms);   // 9 MB/s, busy all along
	EXPECT_NEAR(m.rate(), 9.5e6, 1e3); // 10 MB/s, faded 5%
}

TEST(LinkMeter, ALowReadingFadesThePeakInsteadOfReplacingIt)
{
	LinkMeter m = meter(Mode::ACK);
	m.probed(T0);
	m.answered(T0 + 10ms);
	auto frame = [&, t = T0 + 1s](double busySeconds) mutable
	{
		m.wrote(1'000'000, t);
		m.probed(t);
		m.answered(t + 10ms +
		           std::chrono::duration_cast<LinkMeter::Clock::duration>(
		               std::chrono::duration<double>(busySeconds)));
		t += 1s;
	};
	frame(0.1); // 10 MB/s
	EXPECT_NEAR(m.rate(), 10e6, 1e3);
	frame(1.0); // 1 MB/s: slow start after a pause, say
	EXPECT_NEAR(m.rate(), 9.5e6, 1e3);
	frame(0.05); // 20 MB/s
	EXPECT_NEAR(m.rate(), 20e6, 1e3);
}

TEST(LinkMeter, BetweenPollsTheLinkGoesOnCarrying)
{
	// Polls are 100 ms apart; waiting for the next one before the next frame
	// held playback to ten a second.
	LinkMeter m = meter(Mode::COUNTER, 10e6);
	m.delivered(0, T0);
	m.wrote(1'000'000, T0 + 1ms);
	m.delivered(0, T0 + 2ms, 0.04); // written just now: nothing to see yet
	// 0.1 s of link, less the 40 ms round trip in flight.
	EXPECT_NEAR(m.backlog(T0 + 2ms), 0.06, 1e-9);
	EXPECT_NEAR(m.backlog(T0 + 32ms), 0.03, 1e-9);
	EXPECT_EQ(m.backlog(T0 + 70ms), 0.0);
	// What came after the poll waits for its turn: not drained before it.
	m.wrote(1'000'000, T0 + 50ms);
	EXPECT_NEAR(m.backlog(T0 + 52ms), 0.1 + 0.1 - 0.05 - 0.04, 1e-9);
}

TEST(LinkMeter, NothingAcknowledgedMeansNothingOnItsWay)
{
	// tmux unwraps the passthrough: a little less crosses than was written.
	// Nothing acknowledged while some seems on its way: it was all through.
	LinkMeter m = meter(Mode::COUNTER);
	m.delivered(0, T0);
	m.wrote(1'000'000, T0 + 10ms);
	m.delivered(990'000, T0 + 100ms);
	EXPECT_GT(m.backlog(T0 + 100ms), 0.0);
	m.delivered(0, T0 + 200ms);
	EXPECT_EQ(m.backlog(T0 + 200ms), 0.0);
}

TEST(LinkMeter, AnswersAreDeviceAttributes)
{
	EXPECT_TRUE(LinkMeter::isAnswer("\x1b[?62;22c"));
	EXPECT_TRUE(LinkMeter::isAnswer("\x1b[?1;2c"));
	EXPECT_FALSE(LinkMeter::isAnswer("c"));
	EXPECT_FALSE(LinkMeter::isAnswer("\x1b[?c"));       // too short to be one
	EXPECT_FALSE(LinkMeter::isAnswer("\x1b[>1;10;0c")); // DA2
	EXPECT_FALSE(LinkMeter::isAnswer("\x1b[A"));
}

TEST(LinkMeter, ANewModeStartsWithNothingOnItsWay)
{
	LinkMeter m = meter(Mode::ESTIMATE, 1e6);
	m.wrote(2'000'000, T0);
	EXPECT_GT(m.backlog(T0), 1.0);
	m.setMode(Mode::COUNTER); // a counter turned up
	EXPECT_EQ(m.backlog(T0), 0.0);
	m.wrote(1'000'000, T0);
	m.setMode(Mode::COUNTER); // the same mode again: nothing changes
	EXPECT_DOUBLE_EQ(m.backlog(T0), 1.0);
}

TEST(LinkMeter, SlowestConnectionCounts)
{
	using Counts = std::unordered_map<std::string, TcpCount>;
	const Counts before{{"a", {100, 0.01}}, {"b", {1000, 0.01}}};
	const auto slow =
	    slowestDelivery(before,
	                    Counts{{"a", {600, 0.03}}, {"b", {1200, 0.05}}});
	ASSERT_TRUE(slow);
	EXPECT_EQ(slow->m_acked, 200u);
	EXPECT_DOUBLE_EQ(slow->m_roundTrip, 0.05); // its own, as it is now
	EXPECT_EQ(slowestDelivery(before, Counts{{"a", {600, 0}}, {"c", {5, 0}}})
	              ->m_acked,
	          500u);
	EXPECT_FALSE(slowestDelivery(before, Counts{{"c", {5, 0}}})); // new
	EXPECT_FALSE(slowestDelivery({}, Counts{{"a", {600, 0}}}));
}

TEST(LinkMeter, SshConnectionsMatchWhatSsShows)
{
	const auto v4 = parseSshConnection("10.4.5.6 51514 10.1.2.3 22");
	ASSERT_TRUE(v4);
	EXPECT_EQ(v4->m_client, "10.4.5.6:51514");
	EXPECT_EQ(v4->m_serverPort, "22");
	EXPECT_EQ(parseSshConnection("2001:db8::5 40000 2001:db8::1 2222")->m_client,
	          "2001:db8::5:40000");
	EXPECT_FALSE(parseSshConnection("10.4.5.6 51514"));

	// `ss -tinH state established`: a line per socket, tcp_info indented.
	const std::string ss =
	    "0      0      10.1.2.3:22     10.4.5.6:51514\n"
	    "\t cubic wscale:7,7 rto:204 rtt:1.2/0.5 mss:1448 "
	    "bytes_sent:2000000000 bytes_acked:1933806145 bytes_received:61000 "
	    "segs_out:9\n"
	    "0      0      [::ffff:10.1.2.3]:22     [::ffff:10.9.9.9]:6000\n"
	    "\t cubic bytes_acked:6029 bytes_received:3000\n"
	    "0      0      [2001:db8::1]:2222     [2001:db8::5]:40000\n"
	    "\t cubic bytes_acked:77\n";
	const auto acked = parseBytesAcked(ss);
	EXPECT_EQ(acked.size(), 3u);
	EXPECT_EQ(acked.at("10.4.5.6:51514").m_acked, 1933806145u);
	EXPECT_DOUBLE_EQ(acked.at("10.4.5.6:51514").m_roundTrip, 0.0012);
	EXPECT_EQ(acked.at("10.9.9.9:6000").m_acked, 6029u);   // IPv4-mapped
	EXPECT_EQ(acked.at("10.9.9.9:6000").m_roundTrip, 0.0); // not shown
	EXPECT_EQ(acked.at("2001:db8::5:40000").m_acked, 77u);
	EXPECT_TRUE(parseBytesAcked("").empty());
}

TEST(Transmitter, ProbesFollowWhatADrawWrote)
{
	std::mutex mu;
	std::vector<std::string> writes;
	kitty::Transmitter tx(0,
	                      Transfer::DIRECT,
	                      [&](std::string_view b, bool)
	                      {
		                      std::lock_guard lk(mu);
		                      writes.emplace_back(b);
	                      });
	tx.setLinkRate(4e6);
	tx.setLinkMode(Mode::ACK);
	tx.probeLink(); // the round trip, first
	ASSERT_EQ(writes.size(), 1u);
	EXPECT_EQ(writes[0], "\x1b[c");
	tx.linkAnswered();
	tx.probeLink(); // nothing written since: no probe
	EXPECT_EQ(writes.size(), 1u);
	tx.write(std::string(400'000, 'x'));
	EXPECT_GT(tx.linkBacklog(), 0.0);
	tx.probeLink();
	ASSERT_EQ(writes.size(), 3u);
	EXPECT_EQ(writes[2], "\x1b[c");
	tx.linkAnswered();
	EXPECT_EQ(tx.linkBacklog(), 0.0);

	// Not inline: nothing crosses a link, nothing to ask.
	tx.setTransfer(Transfer::TEMP_FILE);
	tx.write(std::string(400'000, 'x'));
	tx.probeLink();
	EXPECT_EQ(writes.size(), 4u);
	EXPECT_EQ(tx.linkRate(), 0.0);
}
