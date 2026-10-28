#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>

namespace rega::kitty
{

/// What the link to a terminal over ssh carries, and how long it still needs
/// for what was written. tmux takes everything a client over ssh can't yet
/// (a minute of playback, still playing out after a pause) and nothing
/// pushes back, so the writer has to know. Three ways:
/// - ESTIMATE: nothing comes back; the link is assumed to carry the guess.
/// - ACK: no tmux on our side of the link. A probe (DA1) follows what a draw
///   wrote; the terminal, or a tmux at the desk, answers it once all before
///   it is through. The answers time the link.
/// - COUNTER: a tmux on our side answers probes itself, straight away, and
///   keeps the terminal's answers; what it counts as written to a client is
///   what it buffered. The ssh connection's far end acknowledging bytes
///   (bytes_acked, `ss`) is what crossed: delivered() is fed from that.
/// Not thread-safe (the Transmitter locks). Times are passed in for tests.
class LinkMeter
{
public:
	enum class Mode
	{
		ESTIMATE,
		ACK,
		COUNTER
	};
	using Clock = std::chrono::steady_clock;

	/// `guess`: bytes a second until measured (--link-rate); 0: unpaced,
	/// nothing measured or waited for.
	void setGuess(double guess);
	/// A new mode starts from nothing on its way: what one counted the next
	/// can't account for.
	void setMode(Mode mode);
	[[nodiscard]] Mode mode() const
	{
		return m_mode;
	}

	/// `bytes` went into the link.
	void wrote(size_t bytes, Clock::time_point now);
	/// ACK: whether a probe should follow now (pacing on, and something was
	/// written since the last one, or the round trip is still unknown).
	[[nodiscard]] bool wantsProbe(Clock::time_point now) const;
	/// ACK: a probe went out after everything wrote() counted.
	void probed(Clock::time_point now);
	/// ACK: the terminal answered the oldest probe.
	void answered(Clock::time_point now);
	/// COUNTER: bytes the slowest connection carried since the last call,
	/// and its round trip in seconds (TCP's, from `ss`; 0 unknown).
	void delivered(uint64_t bytes, Clock::time_point now, double roundTrip = 0);

	/// Bytes a second: the peak measured (fading), else the guess; 0
	/// unpaced.
	[[nodiscard]] double rate() const;
	/// Whether rate() comes from the link rather than the guess.
	[[nodiscard]] bool measured() const
	{
		return m_measured > 0;
	}
	/// In seconds, 0 unknown. ACK: the shortest seen for a probe with nothing
	/// ahead of it. COUNTER: TCP's, as the last delivered() had it.
	[[nodiscard]] double roundTrip() const;
	/// Seconds the link still needs for what was written (0 unpaced).
	[[nodiscard]] double backlog(Clock::time_point now) const;

	/// ACK: whether `in` is a probe's answer (DA1: ESC [ ? digits/; c).
	[[nodiscard]] static bool isAnswer(std::string_view in);

	/// An unanswered probe is given up after this long (a terminal that
	/// never answers): its bytes count as through.
	static constexpr std::chrono::seconds PROBE_TIMEOUT{3};

private:
	struct Probe
	{
		Clock::time_point m_sent;
		uint64_t m_written; ///< m_written when it went out
		bool m_bare;        ///< nothing ahead of it: times the round trip
	};
	void sample(double bytesPerSecond);
	void dropLost(Clock::time_point now);

	Mode m_mode = Mode::ESTIMATE;
	double m_guess = 0;
	double m_measured = 0; ///< bytes a second, peak fading; 0 until measured
	uint64_t m_written = 0;
	uint64_t m_through = 0;    ///< of m_written: known to be through the link
	uint64_t m_probedUpTo = 0; ///< ACK: m_written at the last probe
	Clock::time_point m_linkFree; ///< ESTIMATE: when it is through
	std::deque<Probe> m_probes;   ///< ACK: unanswered, oldest first
	Clock::time_point m_lastAnswer;
	Clock::time_point m_lastBare;   ///< ACK: last probe with nothing ahead
	std::deque<double> m_bareTrips; ///< ACK: round trips, newest last
	Clock::time_point m_lastPoll;   ///< COUNTER
	uint64_t m_pendingAtPoll = 0;   ///< COUNTER: on its way at m_lastPoll
	uint64_t m_writtenAtPoll = 0;   ///< COUNTER: m_written at m_lastPoll
	double m_counterTrip = 0;       ///< COUNTER: round trip, seconds
	double m_boost = 1; ///< COUNTER: room seen to spare since the last sample
};

} // namespace rega::kitty
