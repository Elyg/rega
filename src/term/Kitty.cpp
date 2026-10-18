#include "term/Kitty.h"

#include "util/Parallel.h"

#include <spdlog/spdlog.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <libdeflate.h>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rv::kitty
{

#include "term/Diacritics.inc"

static_assert(sizeof(DIACRITICS) / sizeof(DIACRITICS[0]) ==
              MAX_PLACEHOLDER_INDEX);

namespace
{

constexpr const char* B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode whole 3-byte groups of [data, data + n) into `out` (n % 3 == 0).
void base64Groups(const uint8_t* data, size_t n, char* out)
{
	for(size_t i = 0; i < n; i += 3, out += 4)
	{
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out[0] = B64[(v >> 18) & 63];
		out[1] = B64[(v >> 12) & 63];
		out[2] = B64[(v >> 6) & 63];
		out[3] = B64[v & 63];
	}
}

// Input bytes per parallel piece (a multiple of 3 for base64).
constexpr size_t PIECE = 3 * 128 * 1024;

} // namespace

std::string base64(const uint8_t* data, size_t n)
{
	const size_t whole = n / 3 * 3;
	std::string out((n + 2) / 3 * 4, '\0');
	const int pieces = static_cast<int>((whole + PIECE - 1) / PIECE);
	parallelFor(pieces,
	            1,
	            [&](int b, int e)
	            {
		            for(int p = b; p < e; ++p)
		            {
			            size_t lo = p * PIECE, hi = std::min(whole, lo + PIECE);
			            base64Groups(data + lo, hi - lo, &out[lo / 3 * 4]);
		            }
	            });
	if(whole < n)
	{
		uint32_t v = data[whole] << 16;
		if(whole + 1 < n)
		{
			v |= data[whole + 1] << 8;
		}
		char* tail = &out[whole / 3 * 4];
		tail[0] = B64[(v >> 18) & 63];
		tail[1] = B64[(v >> 12) & 63];
		tail[2] = whole + 1 < n ? B64[(v >> 6) & 63] : '=';
		tail[3] = '=';
	}
	return out;
}

std::string zlibCompress(const uint8_t* data, size_t n)
{
	// Deliberately ONE ordinary zlib stream, single-threaded. A pigz-style
	// parallel stream (pieces joined with Z_SYNC_FLUSH) is valid deflate, but
	// Ghostty 1.3.1 decodes kitty images with Zig 0.15's std.compress.flate,
	// which panics ("reached unreachable", Io.Writer.unreachableRebase) on
	// such streams and takes the whole terminal down. Reproduced outside
	// Ghostty with Zig 0.15.2; don't reintroduce flush points.
	// libdeflate writes exactly such a stream, 2-3x faster than zlib's
	// level 1 and a little smaller; level 1 is the sweet spot (higher levels
	// cost 40-150% more time for 1-8% less output).
	struct Compressor
	{
		libdeflate_compressor* m_c = libdeflate_alloc_compressor(1);
		~Compressor()
		{
			libdeflate_free_compressor(m_c);
		}
	};
	thread_local Compressor comp;
	if(!comp.m_c)
	{
		throw std::runtime_error("libdeflate: cannot allocate compressor");
	}
	std::string out(libdeflate_zlib_compress_bound(comp.m_c, n), '\0');
	const size_t len =
	    libdeflate_zlib_compress(comp.m_c, data, n, out.data(), out.size());
	if(len == 0)
	{
		throw std::runtime_error("libdeflate: compress failed");
	}
	out.resize(len);
	return out;
}

bool worthCompressing(const uint8_t* data, size_t n)
{
	// Four 64 KiB samples spread over the data: enough to tell film grain
	// (barely shrinks) from CG or flat areas, at a few % of a full compress.
	constexpr size_t SAMPLE = 64 * 1024;
	constexpr int SAMPLES = 4;
	if(n < SAMPLE * SAMPLES)
	{
		return true; // small: compressing costs next to nothing
	}
	size_t in = 0, out = 0;
	for(int i = 0; i < SAMPLES; ++i)
	{
		const size_t at = (n - SAMPLE) / (SAMPLES - 1) * i;
		in += SAMPLE;
		out += zlibCompress(data + at, SAMPLE).size();
	}
	return out < in * 9 / 10;
}

std::string tmuxWrap(std::string_view seq)
{
	// Double every ESC; copy the runs between them whole.
	std::string out;
	out.reserve(seq.size() + 16);
	out += "\x1bPtmux;";
	size_t pos = 0;
	for(size_t esc; (esc = seq.find('\x1b', pos)) != std::string_view::npos;
	    pos = esc + 1)
	{
		out.append(seq, pos, esc + 1 - pos);
		out += '\x1b';
	}
	out.append(seq, pos);
	out += "\x1b\\";
	return out;
}

std::string tmuxWrap(std::string seq, int tmuxes)
{
	for(; tmuxes > 0; --tmuxes)
	{
		seq = tmuxWrap(std::string_view(seq));
	}
	return seq;
}

namespace
{

// Control keys shared by both transfer modes (no medium, no compression).
std::string placementKeys(const Rgba8Image& img, const TransmitOptions& opt)
{
	std::string keys = "a=T,f=32,q=2,s=" + std::to_string(img.m_width) +
	                   ",v=" + std::to_string(img.m_height) +
	                   ",i=" + std::to_string(opt.m_id);
	if(opt.m_virtualPlacement)
	{
		keys += ",U=1,p=" + std::to_string(PLACEMENT_ID);
	}
	if(opt.m_cols > 0)
	{
		keys += ",c=" + std::to_string(opt.m_cols);
	}
	if(opt.m_rows > 0)
	{
		keys += ",r=" + std::to_string(opt.m_rows);
	}
	return keys;
}

} // namespace

std::string transmit(const Rgba8Image& img, const TransmitOptions& opt)
{
	const uint8_t* px = img.m_pixels.data();
	const size_t n = img.m_pixels.size();
	std::string keys = placementKeys(img, opt);
	std::string payload;
	if(worthCompressing(px, n))
	{
		const std::string z = zlibCompress(px, n);
		payload = base64(reinterpret_cast<const uint8_t*>(z.data()), z.size());
		keys.insert(keys.find(",q="), ",o=z");
	}
	else
	{
		payload = base64(px, n);
	}

	std::string out;
	out.reserve(payload.size() + payload.size() / opt.m_chunk * 32 + 128);
	size_t pos = 0;
	bool first = true;
	do
	{
		size_t len = std::min(opt.m_chunk, payload.size() - pos);
		bool more = pos + len < payload.size();
		std::string seq = "\x1b_G";
		if(first)
		{
			seq += keys + ",";
		}
		seq += std::string("m=") + (more ? "1" : "0");
		seq += ";";
		seq.append(payload, pos, len);
		seq += "\x1b\\";
		out += tmuxWrap(std::move(seq), opt.m_tmux);
		pos += len;
		first = false;
	} while(pos < payload.size());
	return out;
}

std::string transmitShared(const Rgba8Image& img,
                           std::string_view name,
                           const TransmitOptions& opt,
                           bool file)
{
	std::string keys = placementKeys(img, opt);
	// No S= with t=f: Ghostty 1.3.1 answers "EINVAL: invalid data" to a
	// file with a size given (it reads the whole file without one).
	keys.insert(keys.find(",q="),
	            file ? std::string(",t=f")
	                 : ",t=s,S=" + std::to_string(img.m_pixels.size()));
	const std::string seq =
	    "\x1b_G" + keys + ";" +
	    base64(reinterpret_cast<const uint8_t*>(name.data()), name.size()) +
	    "\x1b\\";
	return tmuxWrap(seq, opt.m_tmux);
}

bool writeFile(const std::string& path, const Rgba8Image& img)
{
	const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
	if(fd < 0)
	{
		return false;
	}
	const auto* p = reinterpret_cast<const char*>(img.m_pixels.data());
	size_t left = img.m_pixels.size();
	while(left > 0)
	{
		const ssize_t n = ::write(fd, p, left);
		if(n <= 0)
		{
			break;
		}
		p += n;
		left -= static_cast<size_t>(n);
	}
	close(fd);
	if(left > 0)
	{
		unlink(path.c_str());
		return false;
	}
	return true;
}

bool writeSharedMemory(const std::string& name, const Rgba8Image& img)
{
	const size_t n = img.m_pixels.size();
	const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
	if(fd < 0)
	{
		return false;
	}
	bool ok = ftruncate(fd, static_cast<off_t>(n)) == 0;
	if(ok)
	{
		void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		ok = p != MAP_FAILED;
		if(ok)
		{
			std::memcpy(p, img.m_pixels.data(), n);
			munmap(p, n);
		}
	}
	close(fd);
	if(!ok)
	{
		shm_unlink(name.c_str());
	}
	return ok;
}

std::string deleteImage(uint32_t id, int tmux)
{
	std::string seq = "\x1b_Ga=d,d=I,q=2,i=" + std::to_string(id) + "\x1b\\";
	return tmuxWrap(std::move(seq), tmux);
}

std::string deletePlacements(uint32_t id, int tmux)
{
	std::string seq = "\x1b_Ga=d,d=i,q=2,i=" + std::to_string(id) + "\x1b\\";
	return tmuxWrap(std::move(seq), tmux);
}

Transmitter::Transmitter(int tmux, Transfer transfer, WriteFn write)
    : m_tmux(tmux), m_transfer(transfer), m_write(std::move(write))
{
	// Whatever the transfer now: it can switch to files later.
	{
		std::error_code ec;
		m_fileDir = std::filesystem::temp_directory_path(ec).string();
		while(m_fileDir.size() > 1 && m_fileDir.back() == '/')
		{
			m_fileDir.pop_back();
		}
		if(ec || m_fileDir.empty())
		{
			m_fileDir = "/tmp";
		}
	}
	removeStaleFiles();
	// Random start per process keeps concurrent instances apart.
	auto t = static_cast<uint32_t>(
	    std::chrono::steady_clock::now().time_since_epoch().count());
	m_next = (t ^ static_cast<uint32_t>(getpid()) * 2654435761u) % 255u;
}

Transmitter::~Transmitter()
{
	for(const auto& [id, names] : m_local)
	{
		for(const std::string& name : names)
		{
			removeLocal(name); // ENOENT when the terminal got to it
		}
	}
}

void Transmitter::removeStaleFiles() const
{
	// A killed rvtui (terminal closed, SIGHUP) leaves its last
	// MAX_IN_FLIGHT files: rvtui-<pid>-<n>.rgba of pids no longer running.
	std::error_code ec;
	for(const auto& e : std::filesystem::directory_iterator(m_fileDir, ec))
	{
		const std::string name = e.path().filename().string();
		int pid = 0;
		unsigned long long n = 0;
		char ext[8] = {};
		if(std::sscanf(name.c_str(), "rvtui-%d-%llu.%7s", &pid, &n, ext) != 3 ||
		   std::string_view(ext) != "rgba" || pid <= 0 || pid == getpid())
		{
			continue;
		}
		if(kill(pid, 0) != 0 && errno == ESRCH)
		{
			std::filesystem::remove(e.path(), ec);
		}
	}
}

void Transmitter::removeLocal(const std::string& name) const
{
	// By the name, not the transfer now: it may have switched since.
	if(name.ends_with(".rgba"))
	{
		unlink(name.c_str());
	}
	else
	{
		shm_unlink(name.c_str());
	}
}

uint32_t Transmitter::allocId()
{
	// Ids fit in 8 bits so placeholder cells can use a 256-palette foreground,
	// which tmux passes through untouched (it may rewrite 24-bit colours).
	std::lock_guard lock(m_mutex);
	for(int tries = 0; tries < 255; ++tries)
	{
		const uint32_t id = m_next++ % 255u + 1u; // 1..255
		if(!m_used[id])
		{
			m_used[id] = true;
			return id;
		}
	}
	return m_next++ % 255u + 1u; // all 255 live: nothing better to do
}

void Transmitter::releaseId(uint32_t id)
{
	std::lock_guard lock(m_mutex);
	m_used[id & 0xffu] = false;
}

std::string Transmitter::encode(const Rgba8Image& img, TransmitOptions opt)
{
	opt.m_tmux = m_tmux;
	if(const Transfer transfer = m_transfer; transfer != Transfer::DIRECT)
	{
		const bool file = transfer == Transfer::TEMP_FILE;
		std::string name;
		{
			std::lock_guard lock(m_mutex);
			// Shared memory names are short: macOS allows 31 characters.
			name = (file ? m_fileDir : std::string()) + "/rvtui-" +
			       std::to_string(getpid()) + "-" +
			       std::to_string(m_localCount++) + (file ? ".rgba" : "");
		}
		if(file ? writeFile(name, img) : writeSharedMemory(name, img))
		{
			std::lock_guard lock(m_mutex);
			auto& names = m_local[opt.m_id];
			names.push_back(name);
			while(names.size() > MAX_IN_FLIGHT)
			{
				removeLocal(names.front());
				names.pop_front();
			}
			return transmitShared(img, name, opt, file);
		}
		// No shared memory or temp space: send it inline instead.
		spdlog::warn("{} {}: {}; sending inline",
		             file ? "file" : "shared memory",
		             name,
		             std::strerror(errno));
	}
	return transmit(img, opt);
}

void Transmitter::write(std::string_view bytes, bool flush)
{
	m_write(bytes, flush);
	std::lock_guard lock(m_mutex);
	if(m_transfer == Transfer::DIRECT && m_linkRate > 0)
	{
		const auto now = std::chrono::steady_clock::now();
		m_linkFree =
		    std::max(now, m_linkFree) +
		    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		        std::chrono::duration<double>(
		            static_cast<double>(bytes.size()) / m_linkRate));
	}
}

void Transmitter::setLinkRate(double bytesPerSecond)
{
	std::lock_guard lock(m_mutex);
	m_linkRate = std::max(0.0, bytesPerSecond);
}

double Transmitter::linkRate() const
{
	std::lock_guard lock(m_mutex);
	return m_transfer == Transfer::DIRECT ? m_linkRate : 0.0;
}

double Transmitter::linkBacklog() const
{
	std::lock_guard lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	return m_linkFree > now
	           ? std::chrono::duration<double>(m_linkFree - now).count()
	           : 0.0;
}

void Transmitter::queueDelete(uint32_t id)
{
	std::lock_guard lock(m_mutex);
	m_deletes += deleteImage(id, m_tmux);
}

bool Transmitter::hasPendingDeletes()
{
	std::lock_guard lock(m_mutex);
	return !m_deletes.empty();
}

std::string Transmitter::takePendingDeletes()
{
	std::lock_guard lock(m_mutex);
	return std::exchange(m_deletes, {});
}

std::string utf8(char32_t cp)
{
	std::string s;
	if(cp < 0x80)
	{
		s += static_cast<char>(cp);
	}
	else if(cp < 0x800)
	{
		s += static_cast<char>(0xC0 | (cp >> 6));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else if(cp < 0x10000)
	{
		s += static_cast<char>(0xE0 | (cp >> 12));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else
	{
		s += static_cast<char>(0xF0 | (cp >> 18));
		s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return s;
}

std::string placeholderCell(int row, int col)
{
	return utf8(0x10EEEE) + utf8(DIACRITICS[row % MAX_PLACEHOLDER_INDEX]) +
	       utf8(DIACRITICS[col % MAX_PLACEHOLDER_INDEX]);
}

std::string barePlaceholderCell()
{
	return utf8(0x10EEEE);
}

std::optional<std::pair<int, int>> placeholderRowCol(std::string_view cell)
{
	// Diacritic code point → index, built once.
	static const std::unordered_map<char32_t, int> INDEX = []
	{
		std::unordered_map<char32_t, int> m;
		for(int i = 0; i < MAX_PLACEHOLDER_INDEX; ++i)
		{
			m.emplace(DIACRITICS[i], i);
		}
		return m;
	}();
	// Decode UTF-8: exactly U+10EEEE, a row and a column diacritic.
	char32_t cps[3];
	size_t n = 0, pos = 0;
	while(pos < cell.size())
	{
		const auto b = static_cast<unsigned char>(cell[pos]);
		const int len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
		if(n == 3 || pos + len > cell.size())
		{
			return std::nullopt;
		}
		char32_t cp = len == 1   ? b
		              : len == 2 ? b & 0x1F
		              : len == 3 ? b & 0x0F
		                         : b & 0x07;
		for(int i = 1; i < len; ++i)
		{
			cp = (cp << 6) | (static_cast<unsigned char>(cell[pos + i]) & 0x3F);
		}
		cps[n++] = cp;
		pos += len;
	}
	if(n != 3 || cps[0] != 0x10EEEE)
	{
		return std::nullopt;
	}
	const auto row = INDEX.find(cps[1]), col = INDEX.find(cps[2]);
	if(row == INDEX.end() || col == INDEX.end())
	{
		return std::nullopt;
	}
	return std::pair{row->second, col->second};
}

} // namespace rv::kitty
