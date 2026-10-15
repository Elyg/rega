// Fake a bucket renderer killed mid-frame, for testing how viewers cope.
//
//     rvtui_buckets SOURCE.exr OUT_DIR
//
// A bucket renderer (Arnold, RenderMan) writes a tiled EXR as buckets finish:
// OpenEXR puts a zeroed tile offset table after the header when the file is
// opened, appends each tile with writeTile(), and fills the table in only
// when the file is closed. Killed mid-frame, the file holds the buckets done,
// in the order they finished, and an empty table. This does the same with
// OpenEXR itself, through an unbuffered stream, and never closes the file:
// what is on disk is exactly what a killed renderer leaves.
//
// From SOURCE's first part (all its channels, their types and attributes),
// in 64x64 buckets, it writes into OUT_DIR:
//   <name>.tiled.exr                the whole frame, closed: the reference
//   <name>.<order>-<pct>.exr        killed after <pct>% of the buckets, for
//                                   orders topdown, spiral (from the centre)
//                                   and random
//   <name>.spiral-50-halfbucket.exr killed while writing a bucket: half of
//                                   it on disk
//   <name>.copy-cut-60.exr          the closed file cut at 60% (a copy that
//                                   stopped: the table is there, the tiles
//                                   past the cut are not)

#include <sys/stat.h>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfLineOrderAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTiledOutputFile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

constexpr int BUCKET = 64;

/// Every write goes straight to the file (no buffer to lose or flush), as a
/// renderer's would be on disk by the time it is killed.
class RawOStream : public Imf::OStream
{
public:
	explicit RawOStream(const std::string& path)
	    : Imf::OStream(path.c_str()),
	      m_fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644))
	{
		if(m_fd < 0)
		{
			throw std::runtime_error("cannot write " + path);
		}
	}
	~RawOStream() override
	{
		::close(m_fd);
	}
	void write(const char c[], int n) override
	{
		for(int done = 0; done < n;)
		{
			const ssize_t w = ::pwrite(m_fd, c + done, n - done, m_pos);
			if(w <= 0)
			{
				throw std::runtime_error("write failed");
			}
			done += static_cast<int>(w);
			m_pos += w;
		}
	}
	uint64_t tellp() override
	{
		return m_pos;
	}
	void seekp(uint64_t pos) override
	{
		m_pos = static_cast<off_t>(pos);
	}
	int fd() const
	{
		return m_fd;
	}

private:
	int m_fd;
	off_t m_pos = 0;
};

/// The source's first part, every channel, in its own type.
struct Source
{
	Imf::Header m_header;
	std::vector<std::vector<char>> m_planes; ///< per channel, data window
	std::vector<std::pair<std::string, Imf::PixelType>> m_channels;
	Imf::FrameBuffer m_fb;
};

void load(const std::string& path, Source& src)
{
	Imf::InputFile in(path.c_str());
	src.m_header = in.header();
	const auto dw = src.m_header.dataWindow();
	const size_t w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
	for(auto it = src.m_header.channels().begin();
	    it != src.m_header.channels().end();
	    ++it)
	{
		const Imf::PixelType t = it.channel().type;
		const size_t px = t == Imf::HALF ? 2 : 4;
		auto& plane = src.m_planes.emplace_back(w * h * px);
		src.m_channels.emplace_back(it.name(), t);
		// Slices address pixel (x, y) of the data window from its origin.
		char* origin = plane.data() - (dw.min.x + dw.min.y * w) * px;
		src.m_fb.insert(it.name(), Imf::Slice(t, origin, px, px * w));
	}
	in.setFrameBuffer(src.m_fb);
	in.readPixels(dw.min.y, dw.max.y);
}

using Tiles = std::vector<std::pair<int, int>>;

/// The buckets in the order a renderer finishes them.
Tiles order(const std::string& how, int nx, int ny)
{
	Tiles t;
	for(int y = 0; y < ny; ++y)
	{
		for(int x = 0; x < nx; ++x)
		{
			t.emplace_back(x, y);
		}
	}
	if(how == "spiral")
	{
		// Out from the middle, ring by ring, round each ring by angle.
		const double cx = (nx - 1) / 2.0, cy = (ny - 1) / 2.0;
		auto key = [&](const std::pair<int, int>& p)
		{
			const double dx = p.first - cx, dy = p.second - cy;
			return std::pair(std::max(std::abs(dx), std::abs(dy)),
			                 std::atan2(dy, dx));
		};
		std::ranges::stable_sort(t,
		                         [&](const auto& a, const auto& b)
		                         { return key(a) < key(b); });
	}
	else if(how == "random")
	{
		std::mt19937 rng(7);
		std::ranges::shuffle(t, rng);
	}
	return t;
}

/// Write `count` buckets of `tiles` into `path`. `close`: finish the file
/// (offset table filled in); else leave it as a killed renderer would.
/// `halfLast`: the last bucket only half on disk.
void render(const Source& src,
            const std::string& path,
            const Tiles& tiles,
            size_t count,
            bool close,
            bool halfLast = false)
{
	Imf::Header h = src.m_header;
	h.setTileDescription(Imf::TileDescription(BUCKET, BUCKET, Imf::ONE_LEVEL));
	h.lineOrder() = Imf::RANDOM_Y; // tiles in any order, as buckets finish
	auto* stream = new RawOStream(path);
	auto* out = new Imf::TiledOutputFile(*stream, h);
	out->setFrameBuffer(src.m_fb);
	off_t before = 0;
	for(size_t i = 0; i < count; ++i)
	{
		before = static_cast<off_t>(stream->tellp());
		out->writeTile(tiles[i].first, tiles[i].second);
	}
	if(close)
	{
		delete out; // writes the offset table
		delete stream;
		return;
	}
	// Killed: no table. The objects are dropped, never destroyed (that
	// would write it); the bytes are on disk already.
	const int fd = stream->fd();
	if(halfLast)
	{
		const off_t end = static_cast<off_t>(stream->tellp());
		if(::ftruncate(fd, before + (end - before) / 2) != 0)
		{
			throw std::runtime_error("cannot cut " + path);
		}
	}
	::fsync(fd);
}

void cutCopy(const std::string& from, const std::string& to, double fraction)
{
	fs::copy_file(from, to, fs::copy_options::overwrite_existing);
	fs::resize_file(to, static_cast<uintmax_t>(fs::file_size(to) * fraction));
}

} // namespace

int main(int argc, char** argv)
{
	if(argc != 3)
	{
		std::fprintf(stderr, "rvtui_buckets SOURCE.exr OUT_DIR\n");
		return 2;
	}
	try
	{
		Source src;
		load(argv[1], src);
		const fs::path dir = argv[2];
		fs::create_directories(dir);
		const std::string name = fs::path(argv[1]).stem().string();
		auto out = [&](const std::string& what)
		{ return (dir / (name + "." + what + ".exr")).string(); };

		const auto dw = src.m_header.dataWindow();
		const int nx = (dw.max.x - dw.min.x + BUCKET) / BUCKET;
		const int ny = (dw.max.y - dw.min.y + BUCKET) / BUCKET;
		const size_t total = static_cast<size_t>(nx) * ny;
		std::printf("%s: %dx%d buckets of %d px\n", argv[1], nx, ny, BUCKET);

		const std::string whole = out("tiled");
		render(src, whole, order("spiral", nx, ny), total, true);
		std::printf("  %s  closed, every bucket\n", whole.c_str());
		for(const auto& [how, pct] :
		    std::vector<std::pair<std::string, int>>{{"topdown", 50},
		                                             {"spiral", 30},
		                                             {"spiral", 70},
		                                             {"random", 50}})
		{
			const std::string p = out(how + "-" + std::to_string(pct));
			const size_t n = total * pct / 100;
			render(src, p, order(how, nx, ny), n, false);
			std::printf("  %s  killed after %zu of %zu buckets\n",
			            p.c_str(),
			            n,
			            total);
		}
		const std::string half = out("spiral-50-halfbucket");
		render(src, half, order("spiral", nx, ny), total / 2, false, true);
		std::printf("  %s  killed while writing bucket %zu\n",
		            half.c_str(),
		            total / 2);
		const std::string cut = out("copy-cut-60");
		cutCopy(whole, cut, 0.6);
		std::printf("  %s  the closed file cut at 60%%\n", cut.c_str());
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "rvtui_buckets: %s\n", e.what());
		return 1;
	}
	return 0;
}
