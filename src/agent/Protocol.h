#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// Agent mode (prototype): rega on a remote machine sends rendered frames
/// over an ssh-forwarded unix socket to `rega --agent` on the machine with
/// the terminal, which keeps them there as shared memory the terminal reads
/// (kitty t=s). Only the shared memory names cross the pty.
namespace rega::agent
{

/// Bumped whenever a message changes shape.
constexpr uint32_t PROTOCOL_VERSION = 1;

enum class Type : uint8_t
{
	HELLO = 1, ///< both ways, first: m_version; the agent's carries m_token
	FRAME,     ///< remote → agent: m_key, m_width, m_height, m_codec, m_bytes
	READY,     ///< agent → remote: frame m_key is in shared memory m_name
	DROP,  ///< agent → remote: m_key is gone, send it again; remote → agent:
	       ///< done with it
	SHOWN, ///< remote → agent: m_key's name went to the terminal
	BYE    ///< either way: closing
};

/// How FRAME's m_bytes hold the pixels.
enum class Codec : uint8_t
{
	RAW = 0, ///< RGBA8, m_width * m_height * 4 bytes
	JPEG,
	PNG
};
const char* codecName(Codec c); ///< "raw" | "jpeg" | "png"

/// One message; the fields its Type doesn't use stay as they are.
struct Message
{
	Type m_type = Type::BYE;
	uint32_t m_version = 0;
	std::string m_token; ///< HELLO from the agent: the shm name prefix
	uint32_t m_key = 0;  ///< frame, the remote's choice
	uint32_t m_width = 0, m_height = 0;
	Codec m_codec = Codec::RAW;
	std::string m_bytes; ///< FRAME: encoded pixels
	std::string m_name;  ///< READY: shared memory object name
	uint64_t m_size = 0; ///< READY: its size in bytes (kitty S=)
	bool operator==(const Message&) const = default;
};

/// Biggest message accepted: a 16K RGBA frame and change.
constexpr uint32_t MAX_MESSAGE = 1u << 30;

/// `m` as on the wire: a little-endian u32 length, then the type and fields.
std::string encode(const Message& m);
/// A message's body (what follows the length); nullopt when malformed.
std::optional<Message> decode(std::string_view body);

/// Write all of `bytes` to `fd` (EINTR retried). False on error.
bool writeAll(int fd, std::string_view bytes);
/// encode() + writeAll().
bool writeMessage(int fd, const Message& m);
/// Block for the next whole message on `fd`; nullopt on EOF, error or a
/// malformed one.
std::optional<Message> readMessage(int fd);

/// Buffers a non-blocking socket's bytes until whole messages are in: the
/// agent's poll loop reads whatever is there and takes what completed.
class Reader
{
public:
	/// Read what `fd` has. False at EOF or on an error other than EAGAIN.
	bool fill(int fd);
	/// The next whole message, if in. Sets failed() on a malformed one.
	std::optional<Message> next();
	[[nodiscard]] bool failed() const noexcept
	{
		return m_failed;
	}

private:
	std::string m_buf;
	size_t m_pos = 0;
	bool m_failed = false;
};

} // namespace rega::agent
