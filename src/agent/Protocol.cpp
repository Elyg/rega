#include "agent/Protocol.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace rega::agent
{

namespace
{

void putU32(std::string& out, uint32_t v)
{
	for(int i = 0; i < 4; ++i)
	{
		out += static_cast<char>((v >> (8 * i)) & 0xff);
	}
}

void putU64(std::string& out, uint64_t v)
{
	putU32(out, static_cast<uint32_t>(v));
	putU32(out, static_cast<uint32_t>(v >> 32));
}

void putString(std::string& out, std::string_view s)
{
	putU32(out, static_cast<uint32_t>(s.size()));
	out += s;
}

// Reads fields off the front of a body; any read past the end sets m_bad.
struct Cursor
{
	std::string_view m_in;
	bool m_bad = false;

	uint32_t u32()
	{
		if(m_in.size() < 4)
		{
			m_bad = true;
			return 0;
		}
		uint32_t v = 0;
		for(int i = 0; i < 4; ++i)
		{
			v |= uint32_t(static_cast<uint8_t>(m_in[i])) << (8 * i);
		}
		m_in.remove_prefix(4);
		return v;
	}
	uint64_t u64()
	{
		const uint64_t lo = u32();
		return lo | uint64_t(u32()) << 32;
	}
	uint8_t u8()
	{
		if(m_in.empty())
		{
			m_bad = true;
			return 0;
		}
		const auto v = static_cast<uint8_t>(m_in[0]);
		m_in.remove_prefix(1);
		return v;
	}
	std::string string()
	{
		const uint32_t n = u32();
		if(m_bad || m_in.size() < n)
		{
			m_bad = true;
			return {};
		}
		std::string s(m_in.substr(0, n));
		m_in.remove_prefix(n);
		return s;
	}
};

uint32_t readLength(const char* p)
{
	uint32_t n = 0;
	for(int i = 0; i < 4; ++i)
	{
		n |= uint32_t(static_cast<uint8_t>(p[i])) << (8 * i);
	}
	return n;
}

} // namespace

const char* codecName(Codec c)
{
	switch(c)
	{
		case Codec::JPEG:
			return "jpeg";
		case Codec::PNG:
			return "png";
		case Codec::RAW:
			break;
	}
	return "raw";
}

std::string encode(const Message& m)
{
	std::string body;
	body += static_cast<char>(m.m_type);
	switch(m.m_type)
	{
		case Type::HELLO:
			putU32(body, m.m_version);
			putString(body, m.m_token);
			break;
		case Type::FRAME:
			putU32(body, m.m_key);
			putU32(body, m.m_width);
			putU32(body, m.m_height);
			body += static_cast<char>(m.m_codec);
			body.reserve(body.size() + 4 + m.m_bytes.size());
			putString(body, m.m_bytes);
			break;
		case Type::READY:
			putU32(body, m.m_key);
			putString(body, m.m_name);
			putU64(body, m.m_size);
			break;
		case Type::DROP:
		case Type::SHOWN:
			putU32(body, m.m_key);
			break;
		case Type::BYE:
			break;
	}
	std::string out;
	out.reserve(4 + body.size());
	putU32(out, static_cast<uint32_t>(body.size()));
	out += body;
	return out;
}

std::optional<Message> decode(std::string_view body)
{
	Cursor in{body};
	Message m;
	m.m_type = static_cast<Type>(in.u8());
	switch(m.m_type)
	{
		case Type::HELLO:
			m.m_version = in.u32();
			m.m_token = in.string();
			break;
		case Type::FRAME:
			m.m_key = in.u32();
			m.m_width = in.u32();
			m.m_height = in.u32();
			m.m_codec = static_cast<Codec>(in.u8());
			m.m_bytes = in.string();
			if(m.m_codec != Codec::RAW && m.m_codec != Codec::JPEG &&
			   m.m_codec != Codec::PNG)
			{
				return std::nullopt;
			}
			break;
		case Type::READY:
			m.m_key = in.u32();
			m.m_name = in.string();
			m.m_size = in.u64();
			break;
		case Type::DROP:
		case Type::SHOWN:
			m.m_key = in.u32();
			break;
		case Type::BYE:
			break;
		default:
			return std::nullopt;
	}
	if(in.m_bad || !in.m_in.empty())
	{
		return std::nullopt;
	}
	return m;
}

bool writeAll(int fd, std::string_view bytes)
{
	while(!bytes.empty())
	{
		const ssize_t n = ::write(fd, bytes.data(), bytes.size());
		if(n < 0 && errno == EINTR)
		{
			continue;
		}
		if(n <= 0)
		{
			return false;
		}
		bytes.remove_prefix(static_cast<size_t>(n));
	}
	return true;
}

bool writeMessage(int fd, const Message& m)
{
	return writeAll(fd, encode(m));
}

namespace
{
bool readAll(int fd, char* p, size_t n)
{
	while(n > 0)
	{
		const ssize_t got = ::read(fd, p, n);
		if(got < 0 && errno == EINTR)
		{
			continue;
		}
		if(got <= 0)
		{
			return false;
		}
		p += got;
		n -= static_cast<size_t>(got);
	}
	return true;
}
} // namespace

std::optional<Message> readMessage(int fd)
{
	char len[4];
	if(!readAll(fd, len, 4))
	{
		return std::nullopt;
	}
	const uint32_t n = readLength(len);
	if(n == 0 || n > MAX_MESSAGE)
	{
		return std::nullopt;
	}
	std::string body(n, '\0');
	if(!readAll(fd, body.data(), n))
	{
		return std::nullopt;
	}
	return decode(body);
}

bool Reader::fill(int fd)
{
	// Drop what was taken before growing the buffer.
	if(m_pos > 0 && m_pos >= m_buf.size() / 2)
	{
		m_buf.erase(0, m_pos);
		m_pos = 0;
	}
	char buf[1 << 16];
	for(;;)
	{
		const ssize_t got = ::read(fd, buf, sizeof buf);
		if(got > 0)
		{
			m_buf.append(buf, static_cast<size_t>(got));
			continue;
		}
		if(got < 0 && errno == EINTR)
		{
			continue;
		}
		return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
	}
}

std::optional<Message> Reader::next()
{
	if(m_failed || m_buf.size() - m_pos < 4)
	{
		return std::nullopt;
	}
	const uint32_t n = readLength(m_buf.data() + m_pos);
	if(n == 0 || n > MAX_MESSAGE)
	{
		m_failed = true;
		return std::nullopt;
	}
	if(m_buf.size() - m_pos - 4 < n)
	{
		return std::nullopt;
	}
	auto m = decode(std::string_view(m_buf).substr(m_pos + 4, n));
	m_pos += 4 + n;
	if(!m)
	{
		m_failed = true;
	}
	return m;
}

} // namespace rega::agent
