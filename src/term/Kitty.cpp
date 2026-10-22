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
#include <cstdlib>
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
std::string placementKeys(const Rgba8Image& img,
                          const TransmitOptions& opt,
                          bool png = false)
{
	// A PNG says its own size.
	std::string keys = png ? "a=T,f=100,q=2"
	                       : "a=T,f=32,q=2,s=" + std::to_string(img.m_width) +
	                             ",v=" + std::to_string(img.m_height);
	keys += ",i=" + std::to_string(opt.m_id);
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

bool isOpaque(const Rgba8Image& img)
{
	const std::vector<uint8_t>& px = img.m_pixels;
	for(size_t i = 3; i < px.size(); i += 4)
	{
		if(px[i] != 255)
		{
			return false;
		}
	}
	return true;
}

bool isGrey(const Rgba8Image& img)
{
	const std::vector<uint8_t>& px = img.m_pixels;
	for(size_t i = 0; i < px.size(); i += 4)
	{
		if(px[i] != px[i + 1] || px[i] != px[i + 2])
		{
			return false;
		}
	}
	return true;
}

namespace
{

void putBe32(std::string& out, uint32_t v)
{
	for(int shift = 24; shift >= 0; shift -= 8)
	{
		out += static_cast<char>((v >> shift) & 0xff);
	}
}

void pngChunk(std::string& out, const char* type, std::string_view data)
{
	putBe32(out, static_cast<uint32_t>(data.size()));
	const size_t at = out.size();
	out.append(type, 4);
	out.append(data);
	putBe32(out, libdeflate_crc32(0, out.data() + at, out.size() - at));
}

uint8_t paeth(int a, int b, int c)
{
	const int p = a + b - c;
	const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
	return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace

std::string encodePng(const Rgba8Image& img, PngFilter filter)
{
	const bool grey = isGrey(img), opaque = isOpaque(img);
	const int ch = (grey ? 1 : 3) + (opaque ? 0 : 1);
	const uint8_t colourType = grey ? (opaque ? 0 : 4) : (opaque ? 2 : 6);
	const size_t w = static_cast<size_t>(img.m_width);
	const size_t h = static_cast<size_t>(img.m_height);
	const size_t stride = w * ch;
	// The channels kept, row by row, then each row filtered against the one
	// above behind its filter-type byte.
	std::vector<uint8_t> packed(stride * h);
	std::vector<uint8_t> rows((stride + 1) * h);
	parallelFor(img.m_height,
	            16,
	            [&](int b, int e)
	            {
		            for(size_t y = b; y < static_cast<size_t>(e); ++y)
		            {
			            const uint8_t* src = &img.m_pixels[y * w * 4];
			            uint8_t* dst = &packed[y * stride];
			            for(size_t x = 0; x < w; ++x, src += 4)
			            {
				            if(grey)
				            {
					            *dst++ = src[0];
				            }
				            else
				            {
					            *dst++ = src[0];
					            *dst++ = src[1];
					            *dst++ = src[2];
				            }
				            if(!opaque)
				            {
					            *dst++ = src[3];
				            }
			            }
		            }
	            });
	parallelFor(img.m_height,
	            16,
	            [&](int b, int e)
	            {
		            for(size_t y = b; y < static_cast<size_t>(e); ++y)
		            {
			            const uint8_t* cur = &packed[y * stride];
			            const uint8_t* up = y > 0 ? cur - stride : nullptr;
			            uint8_t* out = &rows[y * (stride + 1)];
			            *out++ = static_cast<uint8_t>(filter);
			            for(size_t i = 0; i < stride; ++i)
			            {
				            const int a =
				                i >= static_cast<size_t>(ch) ? cur[i - ch] : 0;
				            const int u = up ? up[i] : 0;
				            const int c = up && i >= static_cast<size_t>(ch)
				                              ? up[i - ch]
				                              : 0;
				            switch(filter)
				            {
					            case PngFilter::NONE:
						            out[i] = cur[i];
						            break;
					            case PngFilter::SUB:
						            out[i] = static_cast<uint8_t>(cur[i] - a);
						            break;
					            case PngFilter::UP:
						            out[i] = static_cast<uint8_t>(cur[i] - u);
						            break;
					            case PngFilter::PAETH:
						            out[i] = static_cast<uint8_t>(
						                cur[i] - paeth(a, u, c));
						            break;
				            }
			            }
		            }
	            });
	std::string ihdr;
	putBe32(ihdr, static_cast<uint32_t>(w));
	putBe32(ihdr, static_cast<uint32_t>(h));
	ihdr += '\x08'; // bits per channel
	ihdr += static_cast<char>(colourType);
	ihdr.append(3, '\0'); // deflate, adaptive filtering, no interlace
	std::string png = "\x89PNG\r\n\x1a\n";
	pngChunk(png, "IHDR", ihdr);
	pngChunk(png, "IDAT", zlibCompress(rows.data(), rows.size()));
	pngChunk(png, "IEND", {});
	return png;
}

std::string transmit(const Rgba8Image& img, const TransmitOptions& opt)
{
	// PNG, not zlib'd pixels: its row filters take a third or more off, and
	// it keeps only the channels needed (one for a channel or alpha shown as
	// grey, none for an alpha that is all opaque).
	const std::string png = encodePng(img);
	const std::string keys = placementKeys(img, opt, true);
	const std::string payload =
	    base64(reinterpret_cast<const uint8_t*>(png.data()), png.size());

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

uint32_t Transmitter::tryAllocId(size_t keepFree)
{
	{
		std::lock_guard lock(m_mutex);
		const auto used = static_cast<size_t>(
		    std::count(m_used.begin() + 1, m_used.end(), true));
		if(255 - used <= keepFree)
		{
			return 0;
		}
	}
	return allocId();
}

bool Transmitter::reserveCache(size_t bytes)
{
	std::lock_guard lock(m_mutex);
	if(m_cacheUse.m_bytes + bytes > m_cacheBudget)
	{
		return false;
	}
	++m_cacheUse.m_frames;
	m_cacheUse.m_bytes += bytes;
	return true;
}

void Transmitter::cacheDropped(size_t bytes)
{
	std::lock_guard lock(m_mutex);
	m_cacheUse.m_frames -= std::min<size_t>(1, m_cacheUse.m_frames);
	m_cacheUse.m_bytes -= std::min(bytes, m_cacheUse.m_bytes);
}

Transmitter::LinkStats Transmitter::linkStats() const
{
	std::lock_guard lock(m_mutex);
	LinkStats s;
	s.m_mode = m_link.mode();
	s.m_inline = m_transfer == Transfer::DIRECT;
	s.m_rate = m_link.rate();
	s.m_measured = m_link.measured();
	s.m_roundTrip = m_link.roundTrip();
	s.m_backlog = m_link.backlog(std::chrono::steady_clock::now());
	s.m_bytesPerPixel = m_bytesPerPixel;
	s.m_lastBytes = m_lastBytes;
	s.m_lastW = m_lastW;
	s.m_lastH = m_lastH;
	s.m_sent = m_sent;
	s.m_keptShown = m_keptShown;
	return s;
}

void Transmitter::countKeptShown()
{
	std::lock_guard lock(m_mutex);
	++m_keptShown;
}

Transmitter::CacheUse Transmitter::cacheUse() const
{
	std::lock_guard lock(m_mutex);
	return m_cacheUse;
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
	std::string out = transmit(img, opt);
	const double pixels = static_cast<double>(img.m_width) * img.m_height;
	if(pixels > 0 && opt.m_measureCost)
	{
		// A black slate costs next to nothing: floor it, or the frame after
		// it would be sized for a link many times faster.
		constexpr double MIN_BYTES_PER_PIXEL = 0.1;
		const double bpp = std::max(MIN_BYTES_PER_PIXEL,
		                            static_cast<double>(out.size()) / pixels);
		std::lock_guard lock(m_mutex);
		m_bytesPerPixel =
		    m_bytesPerPixel > 0 ? 0.7 * m_bytesPerPixel + 0.3 * bpp : bpp;
		m_lastBytes = out.size();
		m_lastW = img.m_width;
		m_lastH = img.m_height;
		++m_sent;
		spdlog::debug("inline {}x{}: {} bytes, {:.3f} B/px (avg {:.3f})",
		              img.m_width,
		              img.m_height,
		              out.size(),
		              bpp,
		              m_bytesPerPixel);
	}
	return out;
}

void Transmitter::write(std::string_view bytes, bool flush)
{
	m_write(bytes, flush);
	if(m_transfer == Transfer::DIRECT)
	{
		std::lock_guard lock(m_mutex);
		m_link.wrote(bytes.size(), std::chrono::steady_clock::now());
	}
}

void Transmitter::setLinkRate(double bytesPerSecond)
{
	std::lock_guard lock(m_mutex);
	m_link.setGuess(bytesPerSecond);
}

void Transmitter::setLinkMode(LinkMeter::Mode mode)
{
	std::lock_guard lock(m_mutex);
	m_link.setMode(mode);
}

double Transmitter::linkRate() const
{
	std::lock_guard lock(m_mutex);
	return m_transfer == Transfer::DIRECT ? m_link.rate() : 0.0;
}

void Transmitter::probeLink()
{
	if(m_transfer != Transfer::DIRECT)
	{
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		if(!m_link.wantsProbe(std::chrono::steady_clock::now()))
		{
			return;
		}
	}
	// DA1: every terminal answers it, in order with what came before, and
	// FTXUI hands the answer on as an event (it keeps the quieter ones).
	m_write("\x1b[c", false);
	std::lock_guard lock(m_mutex);
	m_link.probed(std::chrono::steady_clock::now());
}

void Transmitter::linkAnswered()
{
	std::lock_guard lock(m_mutex);
	const bool was = m_link.measured();
	const double before = m_link.rate();
	m_link.answered(std::chrono::steady_clock::now());
	if(m_link.rate() != before)
	{
		spdlog::debug("link: {:.2f} MB/s measured, round trip {:.0f} ms",
		              m_link.rate() / 1e6,
		              m_link.roundTrip() * 1e3);
	}
	if(!was && m_link.measured())
	{
		spdlog::info("link: {:.1f} MB/s, first measured", m_link.rate() / 1e6);
	}
}

void Transmitter::linkDelivered(uint64_t bytes, double roundTrip)
{
	std::lock_guard lock(m_mutex);
	const bool was = m_link.measured();
	const double before = m_link.rate();
	m_link.delivered(bytes, std::chrono::steady_clock::now(), roundTrip);
	if(m_link.rate() != before)
	{
		spdlog::debug("link: {:.2f} MB/s measured on the ssh connection, "
		              "{:.0f} bytes behind",
		              m_link.rate() / 1e6,
		              m_link.backlog(std::chrono::steady_clock::now()) *
		                  m_link.rate());
	}
	if(!was && m_link.measured())
	{
		spdlog::info("link: {:.1f} MB/s, first measured", m_link.rate() / 1e6);
	}
}

double Transmitter::linkBytesPerPixel() const
{
	std::lock_guard lock(m_mutex);
	return m_bytesPerPixel > 0 ? m_bytesPerPixel : DEFAULT_BYTES_PER_PIXEL;
}

double Transmitter::linkBacklog() const
{
	std::lock_guard lock(m_mutex);
	return m_transfer == Transfer::DIRECT
	           ? m_link.backlog(std::chrono::steady_clock::now())
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
