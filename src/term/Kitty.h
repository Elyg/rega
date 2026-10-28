#pragma once

#include "image/Render.h"
#include "term/Caps.h"
#include "term/LinkMeter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace rega::agent
{
class AgentClient;
}

namespace rega::kitty
{

std::string base64(const uint8_t* data, size_t n);
/// One zlib stream (kitty o=z).
std::string zlibCompress(const uint8_t* data, size_t n);

/// Wrap an escape sequence in tmux DCS passthrough (requires `allow-passthrough
/// on`).
std::string tmuxWrap(std::string_view seq);
/// `seq` wrapped once per tmux in the way (0: as is; 2: tmux in tmux, the
/// outer one ssh'd from).
std::string tmuxWrap(std::string seq, int tmuxes);

/// How transmit() and friends place the image.
struct TransmitOptions
{
	uint32_t m_id = 0;          ///< kitty image id
	int m_cols = 0, m_rows = 0; ///< placement size in cells (0 = natural size)
	bool m_virtualPlacement =
	    true;              ///< U=1: displayed through Unicode placeholder cells
	int m_tmux = 0;        ///< tmuxes in the way, see tmuxWrap()
	size_t m_chunk = 4096; ///< base64 bytes per escape
	/// Counts toward Transmitter::linkBytesPerPixel: the picture, not the
	/// playbar (a strip of flat colour that would pull the average down).
	bool m_measureCost = true;
};

/// Every alpha is 255.
bool isOpaque(const Rgba8Image& img);
/// Red, green and blue equal everywhere (a channel or alpha shown as grey).
bool isGrey(const Rgba8Image& img);
/// PNG row filters (see encodePng).
enum class PngFilter
{
	NONE,
	SUB,
	UP,
	PAETH
};
/// `img` as a PNG (kitty f=100) with no more channels than it needs: grey
/// when isGrey, alpha only when not isOpaque. One zlib stream, like
/// zlibCompress.
std::string encodePng(const Rgba8Image& img, PngFilter filter = PngFilter::UP);
/// Full escape sequence transmitting `img` in the escapes themselves (base64
/// encodePng) and creating a placement.
std::string transmit(const Rgba8Image& img, const TransmitOptions& opt);
/// The same, but the raw RGBA pixels are in the POSIX shared memory object
/// `name` (t=s, see writeSharedMemory) or, with `file`, the file at path
/// `name` (t=f, see writeFile); only the name goes down the pipe.
std::string transmitShared(const Rgba8Image& img,
                           std::string_view name,
                           const TransmitOptions& opt,
                           bool file = false);
/// transmitShared() for a picture of `width` x `height` RGBA held elsewhere
/// (by the agent, see agent::AgentClient).
std::string transmitShared(int width,
                           int height,
                           std::string_view name,
                           const TransmitOptions& opt);
/// Create shared memory object `name` holding the raw pixels of `img`. The
/// terminal unlinks it once read. False on failure.
bool writeSharedMemory(const std::string& name, const Rgba8Image& img);
/// Create the file `path` holding the raw pixels of `img`. Terminals leave it
/// alone (every tmux client can read it); the Transmitter removes it.
bool writeFile(const std::string& path, const Rgba8Image& img);

/// Delete an image and free its data.
std::string deleteImage(uint32_t id, int tmux);
/// Delete the placements of an image, keeping its data.
std::string deletePlacements(uint32_t id, int tmux);

/// The placement id of every virtual placement: one per image, so a picture
/// sent again under the same image id replaces its placement instead of
/// adding one (with several, terminals draw any of them: stale sizes).
constexpr uint32_t PLACEMENT_ID = 1;

/// Sends kitty images to the terminal and keeps what that needs between
/// draws: the image ids in use, deletes waiting for the next frame, and the
/// shared memory objects not known to be read yet. One per App, handed to
/// every ImageSlot. Thread-safe: slots encode on their own threads.
class Transmitter
{
public:
	using WriteFn = std::function<void(std::string_view bytes, bool flush)>;

	/// @param tmux     tmuxes to wrap escapes for (passthrough), see tmuxWrap.
	/// @param transfer how pixels reach the terminal.
	/// @param write    where escapes go (term::writeRaw; a capture in tests).
	Transmitter(int tmux, Transfer transfer, WriteFn write);
	/// Removes the shared memory / files still around.
	~Transmitter();
	Transmitter(const Transmitter&) = delete;
	Transmitter& operator=(const Transmitter&) = delete;

	[[nodiscard]] int tmux() const noexcept
	{
		return m_tmux;
	}
	[[nodiscard]] Transfer transfer() const noexcept
	{
		return m_transfer;
	}
	/// Switch how pixels go from the next encode on (e.g. a terminal attached
	/// to tmux over ssh). Pictures already out stay as they were sent.
	void setTransfer(Transfer t) noexcept
	{
		m_transfer = t;
	}

	/// Non-zero 8-bit image id (placeholder foreground is a 256-palette
	/// colour), never one still held: two live slots sharing an id would show
	/// each other's bitmap. Give it back with releaseId when done.
	[[nodiscard]] uint32_t allocId();
	void releaseId(uint32_t id); ///< see allocId()
	/// allocId() while more than `keepFree` ids stay free (for slots yet to
	/// come), else 0. For pictures the terminal keeps (setTerminalCache).
	[[nodiscard]] uint32_t tryAllocId(size_t keepFree);

	/// Terminal image memory pictures kept there may take, in bytes (decoded
	/// RGBA, as the terminal holds them); 0: none kept. Kitty and Ghostty
	/// allow 320 MB by default and drop the oldest past that, so stay below.
	void setTerminalCache(size_t bytes)
	{
		m_cacheBudget = bytes;
	}
	/// What pictures kept may take: the agent's budget while one is
	/// connected (they are kept there), else the terminal's.
	[[nodiscard]] size_t terminalCache() const;

	/// Agent mode: pictures to keep go to `rega --agent` on the machine
	/// with the terminal instead (see ImageSlot), within `budget` bytes of its
	/// RAM. Null: none, kept in the terminal as before.
	void setAgent(std::shared_ptr<agent::AgentClient> agent, size_t budget);
	/// The agent, while connected; else null.
	[[nodiscard]] std::shared_ptr<agent::AgentClient> agent() const;
	static constexpr size_t DEFAULT_AGENT_CACHE = size_t(4) << 30;
	/// What the slots keep there now: pictures and bytes (decoded).
	struct CacheUse
	{
		size_t m_frames = 0, m_bytes = 0;
	};
	/// For the stats overlay (S): the link, the last picture sent, how
	/// many went out and how many showed from what the terminal keeps.
	struct LinkStats
	{
		LinkMeter::Mode m_mode = LinkMeter::Mode::ESTIMATE;
		bool m_inline = false; ///< Transfer::DIRECT
		double m_rate = 0;     ///< bytes a second; 0 unpaced
		bool m_measured = false;
		double m_roundTrip = 0, m_backlog = 0; ///< seconds
		double m_bytesPerPixel = 0;
		size_t m_lastBytes = 0;
		int m_lastW = 0, m_lastH = 0;
		size_t m_sent = 0, m_keptShown = 0;
	};
	[[nodiscard]] LinkStats linkStats() const;
	void countKeptShown(); ///< a slot showed a kept picture again
	/// Room for a picture of `bytes` to keep, taken if there is: false past
	/// the budget.
	[[nodiscard]] bool reserveCache(size_t bytes);
	void cacheDropped(size_t bytes); ///< a reserved or kept picture goes
	[[nodiscard]] CacheUse cacheUse() const;

	/// Escapes transmitting `img` (opt.m_tmux is set from this transmitter),
	/// through shared memory or a file when that is the transfer mode, else
	/// inline. Writes nothing: call write() with the result. Safe from any
	/// thread.
	[[nodiscard]] std::string encode(const Rgba8Image& img,
	                                 TransmitOptions opt);
	/// Send escapes to the terminal.
	void write(std::string_view bytes, bool flush = false);

	/// Transfer::DIRECT: what the link to the terminal carries, in bytes a
	/// second, until measured (0: unpaced, nothing measured). Nothing pushes
	/// back from inside tmux: it buffers everything a client over ssh can't
	/// take yet (a minute of playback at 1 MB/s, still playing out after
	/// pause), so the writer paces itself. See LinkMeter.
	void setLinkRate(double bytesPerSecond);
	/// How the link is measured (LinkMeter::Mode); ESTIMATE until set.
	void setLinkMode(LinkMeter::Mode mode);
	/// Transfer::DIRECT: bytes a second, measured or else the guess; 0
	/// unpaced or not inline.
	[[nodiscard]] double linkRate() const;
	/// LinkMeter::Mode::ACK: after a draw's writes, a probe the terminal
	/// answers once they are through (see linkAnswered). On the UI thread,
	/// after the draw wrote its pictures.
	void probeLink();
	/// The terminal answered the oldest probe (LinkMeter::isAnswer).
	void linkAnswered();
	/// LinkMeter::Mode::COUNTER: bytes the slowest ssh connection carried
	/// since the last call, and its round trip in seconds.
	void linkDelivered(uint64_t bytes, double roundTrip = 0);
	/// What an inline picture costs on the link per pixel, escapes and all:
	/// averaged over the last few sent (DEFAULT_BYTES_PER_PIXEL until one
	/// is). Film grain runs about 3, CG a tenth of that.
	[[nodiscard]] double linkBytesPerPixel() const;
	static constexpr double DEFAULT_BYTES_PER_PIXEL = 3.0;
	/// Seconds the link still needs for what was written (0 unpaced).
	[[nodiscard]] double linkBacklog() const;
	static constexpr double DEFAULT_LINK_RATE = 4e6;

	/// Queue deleting image `id`; takePendingDeletes() hands the escapes over
	/// for the caller to write when the frame no longer shows it.
	void queueDelete(uint32_t id);
	[[nodiscard]] std::string takePendingDeletes(); ///< empties the queue
	[[nodiscard]] bool hasPendingDeletes();

	/// Shared memory objects / files kept until removed, per image id: an
	/// ImageSlot sends every picture under its one id and has at most four
	/// alive (on screen, being prepared, two prepared). Terminals read them in
	/// order, so by this many newer ones under the same id the oldest is done
	/// with or was dropped (e.g. no terminal attached to tmux). One shared
	/// count deleted a tile sheet's files before the terminal read them.
	static constexpr size_t MAX_IN_FLIGHT = 8;

private:
	const int m_tmux;
	std::atomic<Transfer> m_transfer;
	const WriteFn m_write;
	mutable std::mutex m_mutex;
	LinkMeter m_link;
	std::atomic<size_t> m_cacheBudget = 0;
	std::shared_ptr<agent::AgentClient> m_agent; ///< m_mutex
	size_t m_agentBudget = 0;
	CacheUse m_cacheUse;
	double m_bytesPerPixel = 0; ///< inline pictures, averaged; 0: none yet
	size_t m_lastBytes = 0, m_sent = 0, m_keptShown = 0; ///< see LinkStats
	int m_lastW = 0, m_lastH = 0;
	std::array<bool, 256> m_used{};
	uint32_t m_next;
	std::string m_deletes;
	void removeLocal(const std::string& name) const;
	void removeStaleFiles() const;

	std::string m_fileDir; ///< Transfer::TEMP_FILE: where the files go
	/// Per image id: created, maybe not read; oldest first.
	std::unordered_map<uint32_t, std::deque<std::string>> m_local;
	uint64_t m_localCount = 0;
};

/// Placeholder cell text: U+10EEEE + row diacritic + column diacritic (UTF-8).
std::string placeholderCell(int row, int col);
constexpr int MAX_PLACEHOLDER_INDEX = 297;
/// A placeholder without diacritics: the terminal takes the row from the cell
/// to its left and the column as that cell's plus one (same image id).
std::string barePlaceholderCell();
/// Row and column of a placeholderCell(); nullopt for any other text.
std::optional<std::pair<int, int>> placeholderRowCol(std::string_view cell);

std::string utf8(char32_t cp); ///< one code point as UTF-8

} // namespace rega::kitty
