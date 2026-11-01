#include "image/Loader.h"
#include "util/Parallel.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// Movies are a frame source in a one-file-per-frame world: the viewer asks for
// frames by number, from three threads, mostly in order but also anywhere at
// random. Long-GOP frames can only be decoded forward from a keyframe, so each
// open file has one decoder, working under a lock (it threads internally), and
// keeps the frames it just made: threads asking slightly out of order, and
// stepping backwards, find theirs there rather than seeking. Turning a frame
// into planes happens outside the lock, so that part runs in parallel.
//
// Video is display-referred, so like PNG it goes through the sRGB EOTF and the
// default view round-trips the code values. No transfer function is honoured:
// PQ and HLG clips show their raw code values.

namespace rega
{

namespace
{

/// Frames kept per open file: this many bytes, but never fewer than the
/// frames. ImageService's budget does not count them.
constexpr size_t KEPT_BYTES = size_t(128) << 20;
constexpr size_t MIN_KEPT = 8;
/// Open files. One at a time per view, but a sheet tiles several.
constexpr size_t MAX_OPEN = 16;
/// Decoder threads per open file (frame threads hold a frame each).
constexpr int DECODE_THREADS = 8;

struct FormatFree
{
	void operator()(AVFormatContext* p) const
	{
		avformat_close_input(&p);
	}
};
struct CodecFree
{
	void operator()(AVCodecContext* p) const
	{
		avcodec_free_context(&p);
	}
};
struct PacketFree
{
	void operator()(AVPacket* p) const
	{
		av_packet_free(&p);
	}
};
struct FrameFree
{
	void operator()(AVFrame* p) const
	{
		av_frame_free(&p);
	}
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatFree>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;

std::string avError(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = "";
	av_strerror(code, buf, sizeof buf);
	return buf;
}

// FFmpeg logs to stderr by default, under the TUI.
void onLog(void* object, int level, const char* format, va_list args)
{
	if(level > AV_LOG_WARNING)
	{
		return;
	}
	char line[512] = "";
	int prefix = 1;
	av_log_format_line2(object, level, format, args, line, sizeof line, &prefix);
	std::string text(line);
	while(!text.empty() && (text.back() == '\n' || text.back() == '\r'))
	{
		text.pop_back();
	}
	spdlog::debug("ffmpeg: {}", text);
}

/// sRGB-decoded 16-bit codes, built once.
const std::vector<float>& linearFloats()
{
	static const std::vector<float> TABLE = []
	{
		std::vector<float> t(65536);
		for(size_t i = 0; i < t.size(); ++i)
		{
			t[i] = srgbToLinear(static_cast<float>(i) / 65535.0f);
		}
		return t;
	}();
	return TABLE;
}
const std::vector<uint16_t>& linearHalves()
{
	static const std::vector<uint16_t> TABLE = []
	{
		std::vector<uint16_t> t(65536);
		for(size_t i = 0; i < t.size(); ++i)
		{
			t[i] = imath_float_to_half(linearFloats()[i]);
		}
		return t;
	}();
	return TABLE;
}

/// `src` (w by h) turned `quarters` clockwise.
template <class T>
std::vector<T> turned(const T* src, int w, int h, int quarters)
{
	const bool sideways = quarters % 2 != 0;
	const int outW = sideways ? h : w;
	std::vector<T> out(static_cast<size_t>(w) * h);
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const int dx = quarters == 1   ? h - 1 - y
			               : quarters == 2 ? w - 1 - x
			                               : y;
			const int dy = quarters == 1   ? x
			               : quarters == 2 ? h - 1 - y
			                               : w - 1 - x;
			out[static_cast<size_t>(dy) * outW + dx] =
			    src[static_cast<size_t>(y) * w + x];
		}
	}
	return out;
}

/// The planes `plane` turns into when turned `quarters` clockwise.
Plane turnedPlane(const Plane& plane, int w, int h, int quarters)
{
	const size_t n = static_cast<size_t>(w) * h;
	if(plane.isHalf())
	{
		auto values = turned(plane.halfData(), w, h, quarters);
		Plane out = Plane::halves(n);
		std::copy(values.begin(), values.end(), out.halfData());
		return out;
	}
	return Plane(turned(plane.floatData(), w, h, quarters));
}

/// A scaler per worker thread: the context is slow to make and most frames
/// of a file share it.
struct Scaler
{
	SwsContext* m_context = nullptr;
	~Scaler()
	{
		sws_freeContext(m_context);
	}
	Scaler() = default;
	Scaler(const Scaler&) = delete;
	Scaler& operator=(const Scaler&) = delete;
};

class Movie
{
public:
	explicit Movie(const std::string& path);

	const ImageInfo& info() const
	{
		return m_info;
	}
	/// Frame `n` (clamped) as decoded, else the nearest the file gives: a
	/// damaged stretch holds a neighbour rather than failing the playback.
	/// Throws LoadError when nothing decodes at all.
	FramePtr frame(int n);
	/// `frame` as the planes of a layer, in display orientation.
	LayerImage planes(const AVFrame& frame, const LayerInfo& layer) const;

private:
	void readFrameTable();
	void describe(const std::string& path);
	/// The next frame out of the decoder; null at the end of the stream.
	FramePtr decode();
	void seek(int64_t pts);
	/// The first frame: learns how the decoder's timestamps sit against the
	/// table (a stream with B-frames presents later than it decodes).
	void start();
	/// Seek so that decoding forward reaches `n`, from keyframe `key` (an
	/// index into m_keys) or the first before it that does.
	void seekFor(int n, size_t key);
	/// Which frame `f` is, from its timestamp; -1 if it has none.
	int indexOf(const AVFrame& f) const;
	void keep(FramePtr f, int index);
	const AVFrame* kept(int n) const;
	const AVFrame* keptNearest(int n) const;

	struct Kept
	{
		int m_index = 0;
		size_t m_bytes = 0;
		FramePtr m_frame;
	};

	FormatPtr m_format;
	CodecPtr m_codec;
	PacketPtr m_packet;
	AVStream* m_stream = nullptr;
	ImageInfo m_info;
	int m_width = 0, m_height = 0; ///< as stored, before any rotation
	int m_quarters = 0;            ///< clockwise, to show it upright
	bool m_alpha = false;
	bool m_halves = true;
	int m_depth = 8;

	std::mutex m_mu; ///< everything below, and the decoder
	/// Per frame, in file order: when it decodes (stream time base), frames
	/// the movie doesn't show (an edit list's lead-in) left out.
	std::vector<int64_t> m_times;
	/// Per frame, in display order: when it shows (see start()).
	std::vector<int64_t> m_shown;
	std::vector<int64_t> m_allTimes; ///< m_times, with those left out
	std::vector<int> m_keys; ///< frames (indices) a seek can land on, sorted
	int64_t m_offset = 0;    ///< first shown - first decoded: where seeks aim
	bool m_started = false;
	bool m_drained = false;
	int m_next = 0; ///< index of the frame the decoder gives next
	std::deque<Kept> m_kept;
	size_t m_keptBytes = 0;
	std::string m_error; ///< the decoder's last complaint
};

Movie::Movie(const std::string& path)
{
	static std::once_flag logging;
	std::call_once(logging, [] { av_log_set_callback(onLog); });

	// "file:" so that a name holding a colon is not taken for a protocol.
	const std::string url = "file:" + path;
	AVFormatContext* format = nullptr;
	if(const int r =
	       avformat_open_input(&format, url.c_str(), nullptr, nullptr);
	   r < 0)
	{
		throw LoadError("cannot read movie: " + avError(r));
	}
	m_format.reset(format);
	if(const int r = avformat_find_stream_info(format, nullptr); r < 0)
	{
		throw LoadError("cannot read movie: " + avError(r));
	}
	const int stream =
	    av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if(stream < 0)
	{
		throw LoadError("no video in this movie");
	}
	m_stream = format->streams[stream];
	const AVCodecParameters* par = m_stream->codecpar;
	const AVCodec* codec = avcodec_find_decoder(par->codec_id);
	if(!codec)
	{
		throw LoadError(fmt::format("no decoder for {} video",
		                            avcodec_get_name(par->codec_id)));
	}
	m_codec.reset(avcodec_alloc_context3(codec));
	m_packet.reset(av_packet_alloc());
	if(!m_codec || !m_packet ||
	   avcodec_parameters_to_context(m_codec.get(), par) < 0)
	{
		throw LoadError("cannot set up the decoder");
	}
	m_codec->pkt_timebase = m_stream->time_base;
	m_codec->thread_count = DECODE_THREADS;
	m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	if(const int r = avcodec_open2(m_codec.get(), codec, nullptr); r < 0)
	{
		throw LoadError("cannot open the decoder: " + avError(r));
	}
	readFrameTable();
	describe(path);
}

void Movie::readFrameTable()
{
	// Walking a file with every stream discarded reads no sample data, only
	// what lies between the samples: for a fragmented file that is where the
	// index is, so this is what completes it.
	for(unsigned i = 0; i < m_format->nb_streams; ++i)
	{
		m_format->streams[i]->discard = AVDISCARD_ALL;
	}
	while(av_read_frame(m_format.get(), m_packet.get()) >= 0)
	{
		av_packet_unref(m_packet.get());
	}
	m_stream->discard = AVDISCARD_DEFAULT;

	const int entries = avformat_index_get_entries_count(m_stream);
	m_times.reserve(static_cast<size_t>(entries));
	for(int i = 0; i < entries; ++i)
	{
		const AVIndexEntry* e = avformat_index_get_entry(m_stream, i);
		m_allTimes.push_back(e->timestamp);
		if(e->flags & AVINDEX_DISCARD_FRAME)
		{
			continue;
		}
		if(e->flags & AVINDEX_KEYFRAME)
		{
			m_keys.push_back(static_cast<int>(m_times.size()));
		}
		m_times.push_back(e->timestamp);
	}
	if(m_times.empty())
	{
		throw LoadError("no video frames in this movie");
	}
	if(m_keys.empty() || m_keys.front() != 0)
	{
		m_keys.insert(m_keys.begin(), 0);
	}
}

void Movie::describe(const std::string& path)
{
	const AVCodecParameters* par = m_stream->codecpar;
	m_width = par->width;
	m_height = par->height;
	if(m_width <= 0 || m_height <= 0)
	{
		throw LoadError("movie has no picture size");
	}
	const auto pix = static_cast<AVPixelFormat>(par->format);
	const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix);
	if(desc)
	{
		m_depth = desc->comp[0].depth;
		m_alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
	}
	// Half holds 10-bit code values through the sRGB curve; beyond that, float.
	m_halves = m_depth <= 10;

	const AVPacketSideData* side =
	    av_packet_side_data_get(par->coded_side_data,
	                            par->nb_coded_side_data,
	                            AV_PKT_DATA_DISPLAYMATRIX);
	if(side && side->size >= static_cast<int>(9 * sizeof(int32_t)))
	{
		const double degrees = av_display_rotation_get(
		    reinterpret_cast<const int32_t*>(side->data));
		if(!std::isnan(degrees))
		{
			// Counter-clockwise there; the nearest quarter, clockwise, here.
			const int quarters = -static_cast<int>(std::lround(degrees / 90.0));
			m_quarters = ((quarters % 4) + 4) % 4;
		}
	}
	const bool sideways = m_quarters % 2 != 0;
	const int w = sideways ? m_height : m_width;
	const int h = sideways ? m_width : m_height;

	m_info.m_path = path;
	m_info.m_format = path.ends_with("4") ? "mp4" : "mov";
	m_info.m_displayReferred = true;
	m_info.m_frameCount = static_cast<int>(m_times.size());
	AVRational rate = m_stream->avg_frame_rate;
	if(rate.num <= 0 || rate.den <= 0)
	{
		rate = m_stream->r_frame_rate;
	}
	if(rate.num > 0 && rate.den > 0)
	{
		m_info.m_fps = av_q2d(rate);
	}

	PartInfo part;
	part.m_dataWindow = part.m_displayWindow = Box2i{0, 0, w - 1, h - 1};
	part.m_compression = avcodec_get_name(par->codec_id);
	auto& a = part.m_attributes;
	a.push_back({"size", "V2i", fmt::format("{}x{}", w, h)});
	a.push_back({"codec", "string", part.m_compression});
	if(const char* profile = avcodec_profile_name(par->codec_id, par->profile))
	{
		a.push_back({"profile", "string", profile});
	}
	if(desc)
	{
		a.push_back({"pixelFormat", "string", desc->name});
	}
	a.push_back({"bitDepth", "int", std::to_string(m_depth)});
	a.push_back({"frames", "int", std::to_string(m_times.size())});
	if(m_info.m_fps)
	{
		a.push_back({"fps", "double", fmt::format("{:.3f}", *m_info.m_fps)});
		a.push_back(
		    {"duration",
		     "string",
		     fmt::format("{:.2f}s",
		                 static_cast<double>(m_times.size()) / *m_info.m_fps)});
	}
	if(par->color_space != AVCOL_SPC_UNSPECIFIED)
	{
		a.push_back(
		    {"matrix", "string", av_color_space_name(par->color_space)});
	}
	if(par->color_primaries != AVCOL_PRI_UNSPECIFIED)
	{
		a.push_back({"primaries",
		             "string",
		             av_color_primaries_name(par->color_primaries)});
	}
	if(par->color_trc != AVCOL_TRC_UNSPECIFIED)
	{
		a.push_back(
		    {"transfer", "string", av_color_transfer_name(par->color_trc)});
	}
	if(par->color_range != AVCOL_RANGE_UNSPECIFIED)
	{
		a.push_back({"range", "string", av_color_range_name(par->color_range)});
	}
	if(m_quarters != 0)
	{
		a.push_back({"rotation", "int", std::to_string(m_quarters * 90)});
	}
	const AVRational aspect = par->sample_aspect_ratio;
	if(aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
	{
		a.push_back({"pixelAspect",
		             "string",
		             fmt::format("{}:{}", aspect.num, aspect.den)});
	}
	for(const AVDictionary* tags : {m_format->metadata, m_stream->metadata})
	{
		const AVDictionaryEntry* tag = nullptr;
		while((tag = av_dict_iterate(tags, tag)))
		{
			a.push_back({tag->key, "string", tag->value});
		}
	}
	m_info.m_parts.push_back(std::move(part));

	LayerInfo layer;
	const std::string type = m_depth > 8 ? "uint16" : "uint8";
	layer.m_channels = {"R", "G", "B"};
	if(m_alpha)
	{
		layer.m_channels.emplace_back("A");
	}
	layer.m_types.assign(layer.m_channels.size(), type);
	m_info.m_layers.push_back(std::move(layer));
}

FramePtr Movie::decode()
{
	FramePtr frame(av_frame_alloc());
	if(!frame)
	{
		throw LoadError("out of memory");
	}
	for(;;)
	{
		const int got = avcodec_receive_frame(m_codec.get(), frame.get());
		if(got >= 0)
		{
			return frame;
		}
		if(got == AVERROR_EOF)
		{
			return nullptr;
		}
		if(got != AVERROR(EAGAIN))
		{
			m_error = avError(got); // a bad frame: on to the next
			continue;
		}
		if(m_drained)
		{
			return nullptr;
		}
		const int read = av_read_frame(m_format.get(), m_packet.get());
		if(read < 0)
		{
			if(read != AVERROR_EOF)
			{
				m_error = avError(read);
			}
			m_drained = true; // what the decoder still holds comes out
			avcodec_send_packet(m_codec.get(), nullptr);
			continue;
		}
		if(m_packet->stream_index == m_stream->index)
		{
			if(const int sent =
			       avcodec_send_packet(m_codec.get(), m_packet.get());
			   sent < 0)
			{
				m_error = avError(sent);
			}
		}
		av_packet_unref(m_packet.get());
	}
}

void Movie::seek(int64_t pts)
{
	const int r = av_seek_frame(m_format.get(),
	                            m_stream->index,
	                            pts,
	                            AVSEEK_FLAG_BACKWARD);
	if(r < 0)
	{
		throw LoadError("cannot seek in movie: " + avError(r));
	}
	avcodec_flush_buffers(m_codec.get());
	m_drained = false;
}

void Movie::start()
{
	seek(m_stream->start_time != AV_NOPTS_VALUE ? m_stream->start_time
	                                            : m_times.front());
	FramePtr f = decode();
	if(!f)
	{
		throw LoadError(m_error.empty() ? "cannot decode movie"
		                                : "cannot decode movie: " + m_error);
	}
	// The file only says when frames decode. Decoding runs a few frames ahead
	// of showing, and an encoder gives a frame the decode time of the one
	// that many places later, so from the first frame shown on the decode
	// times are the display times. (A fixed time apart they are not: that
	// goes wrong wherever a variable rate changes.)
	const int64_t first = f->best_effort_timestamp != AV_NOPTS_VALUE
	                          ? f->best_effort_timestamp
	                          : m_times.front();
	m_offset = first - m_times.front();
	m_shown.assign(std::ranges::lower_bound(m_allTimes, first),
	               m_allTimes.end());
	if(m_shown.empty() || m_shown.front() != first)
	{
		m_shown.insert(m_shown.begin(), first);
	}
	m_shown.resize(std::min(m_shown.size(), m_times.size()));
	// The last few decode before they show: on past the end, at the last pace.
	const int64_t step =
	    m_shown.size() > 1 ? m_shown.back() - m_shown[m_shown.size() - 2] : 1;
	while(m_shown.size() < m_times.size())
	{
		m_shown.push_back(m_shown.back() + step);
	}
	m_allTimes = {};
	keep(std::move(f), 0);
	m_next = 1;
	m_started = true;
}

void Movie::seekFor(int n, size_t key)
{
	// A keyframe's table position is where it decodes, and it may show later
	// than that (leading frames of an open GOP), past `n`: then go back one.
	for(size_t k = key;; --k)
	{
		seek(m_times[static_cast<size_t>(m_keys[k])] + m_offset);
		FramePtr f = decode();
		const int at = f ? indexOf(*f) : -1;
		if(f && (at <= n || k == 0))
		{
			const int index = std::max(at, 0);
			keep(std::move(f), index);
			m_next = index + 1;
			return;
		}
		if(k == 0)
		{
			m_next = static_cast<int>(m_times.size());
			return;
		}
	}
}

int Movie::indexOf(const AVFrame& f) const
{
	if(f.best_effort_timestamp == AV_NOPTS_VALUE)
	{
		return -1;
	}
	const int64_t t = f.best_effort_timestamp;
	const auto after = std::ranges::lower_bound(m_shown, t);
	if(after == m_shown.begin())
	{
		return 0;
	}
	if(after == m_shown.end())
	{
		return static_cast<int>(m_shown.size()) - 1;
	}
	const auto before = after - 1;
	const auto nearest = *after - t < t - *before ? after : before;
	return static_cast<int>(nearest - m_shown.begin());
}

void Movie::keep(FramePtr f, int index)
{
	if(kept(index))
	{
		return;
	}
	const int bytes =
	    av_image_get_buffer_size(static_cast<AVPixelFormat>(f->format),
	                             f->width,
	                             f->height,
	                             1);
	Kept k{index, static_cast<size_t>(std::max(bytes, 0)), std::move(f)};
	m_keptBytes += k.m_bytes;
	m_kept.push_back(std::move(k));
	while(m_kept.size() > MIN_KEPT && m_keptBytes > KEPT_BYTES)
	{
		m_keptBytes -= m_kept.front().m_bytes;
		m_kept.pop_front();
	}
}

const AVFrame* Movie::kept(int n) const
{
	for(const auto& k : m_kept)
	{
		if(k.m_index == n)
		{
			return k.m_frame.get();
		}
	}
	return nullptr;
}

const AVFrame* Movie::keptNearest(int n) const
{
	const Kept* best = nullptr;
	for(const auto& k : m_kept)
	{
		if(!best || std::abs(k.m_index - n) < std::abs(best->m_index - n))
		{
			best = &k;
		}
	}
	return best ? best->m_frame.get() : nullptr;
}

FramePtr Movie::frame(int n)
{
	std::lock_guard lk(m_mu);
	n = std::clamp(n, 0, static_cast<int>(m_times.size()) - 1);
	if(!m_started)
	{
		start();
	}
	const AVFrame* found = kept(n);
	if(!found)
	{
		// Decode on unless it is behind, or a keyframe gets there sooner.
		const auto after = std::ranges::upper_bound(m_keys, n);
		const auto key = static_cast<size_t>(after - m_keys.begin()) - 1;
		if(n < m_next || m_keys[key] > m_next)
		{
			seekFor(n, key);
		}
		while(m_next <= n)
		{
			FramePtr f = decode();
			if(!f)
			{
				break;
			}
			// Timestamps only ever move forward from here, however close
			// two frames of a variable-rate file are.
			const int index = std::max(indexOf(*f), m_next);
			keep(std::move(f), index);
			m_next = index + 1;
		}
		found = kept(n);
		if(!found)
		{
			found = keptNearest(n);
		}
	}
	if(!found)
	{
		throw LoadError(m_error.empty() ? "cannot decode movie frame"
		                                : "cannot decode movie: " + m_error);
	}
	return FramePtr(av_frame_clone(found));
}

LayerImage Movie::planes(const AVFrame& f, const LayerInfo& layer) const
{
	const int w = m_width, h = m_height;
	const size_t n = static_cast<size_t>(w) * h;
	const int count = m_alpha ? 4 : 3;
	std::vector<uint16_t> stored[4];
	uint8_t* dst[4] = {};
	int strides[4] = {};
	for(int c = 0; c < count; ++c)
	{
		stored[c].resize(n);
		dst[c] = reinterpret_cast<uint8_t*>(stored[c].data());
		strides[c] = w * static_cast<int>(sizeof(uint16_t));
	}

	const auto source = static_cast<AVPixelFormat>(f.format);
	static thread_local Scaler scaler;
	scaler.m_context =
	    sws_getCachedContext(scaler.m_context,
	                         f.width,
	                         f.height,
	                         source,
	                         w,
	                         h,
	                         m_alpha ? AV_PIX_FMT_GBRAP16 : AV_PIX_FMT_GBRP16,
	                         SWS_BILINEAR | SWS_ACCURATE_RND |
	                             SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP,
	                         nullptr,
	                         nullptr,
	                         nullptr);
	if(!scaler.m_context)
	{
		throw LoadError(fmt::format("cannot convert {} video",
		                            av_get_pix_fmt_name(source)
		                                ? av_get_pix_fmt_name(source)
		                                : "this"));
	}
	// Untagged video is BT.709 from HD up and BT.601 below, as in players.
	const int matrix = f.colorspace != AVCOL_SPC_UNSPECIFIED ? f.colorspace
	                   : h >= 720                            ? SWS_CS_ITU709
	                                                         : SWS_CS_ITU601;
	sws_setColorspaceDetails(scaler.m_context,
	                         sws_getCoefficients(matrix),
	                         f.color_range == AVCOL_RANGE_JPEG ? 1 : 0,
	                         sws_getCoefficients(SWS_CS_DEFAULT),
	                         1,
	                         0,
	                         1 << 16,
	                         1 << 16);
	sws_scale(scaler.m_context, f.data, f.linesize, 0, f.height, dst, strides);

	LayerImage img;
	img.m_info = layer;
	const bool sideways = m_quarters % 2 != 0;
	img.m_width = sideways ? h : w;
	img.m_height = sideways ? w : h;
	img.m_dataWindow = img.m_displayWindow =
	    Box2i{0, 0, img.m_width - 1, img.m_height - 1};
	img.m_channelNames = layer.m_channels;

	// GBRA as stored; R, G, B, A in the image.
	static constexpr int STORED_AT[] = {2, 0, 1, 3};
	const auto& halves = linearHalves();
	const auto& floats = linearFloats();
	for(int c = 0; c < count; ++c)
	{
		const uint16_t* in = stored[STORED_AT[c]].data();
		Plane plane = m_halves ? Plane::halves(n) : Plane::floats(n);
		uint16_t* outHalf = plane.halfData();
		float* outFloat = plane.floatData();
		const bool alpha = c == 3;
		parallelFor(h,
		            32,
		            [&](int begin, int end)
		            {
			            const size_t from = static_cast<size_t>(begin) * w;
			            const size_t to = static_cast<size_t>(end) * w;
			            for(size_t i = from; i < to; ++i)
			            {
				            if(alpha)
				            {
					            plane.set(i, in[i] / 65535.0f);
				            }
				            else if(outHalf)
				            {
					            outHalf[i] = halves[in[i]];
				            }
				            else
				            {
					            outFloat[i] = floats[in[i]];
				            }
			            }
		            });
		img.m_planes.push_back(m_quarters == 0
		                           ? std::move(plane)
		                           : turnedPlane(plane, w, h, m_quarters));
	}
	return img;
}

/// The open movie for `file` as it is now: a changed file is a new movie.
std::shared_ptr<Movie> openMovie(const std::filesystem::path& file)
{
	std::error_code ec;
	const auto mtime = std::filesystem::last_write_time(file, ec);
	const auto size = std::filesystem::file_size(file, ec);
	const std::string key = fmt::format("{}|{}:{}",
	                                    file.string(),
	                                    mtime.time_since_epoch().count(),
	                                    ec ? 0 : size);
	static std::mutex mu;
	static std::list<std::pair<std::string, std::shared_ptr<Movie>>> open;
	std::lock_guard lk(mu);
	for(auto it = open.begin(); it != open.end(); ++it)
	{
		if(it->first == key)
		{
			open.splice(open.begin(), open, it);
			return it->second;
		}
	}
	open.emplace_front(key, std::make_shared<Movie>(file.string()));
	if(open.size() > MAX_OPEN)
	{
		open.pop_back();
	}
	return open.front().second;
}

} // namespace

ImageInfo probeMovie(const std::filesystem::path& p)
{
	const auto [file, frame] = splitMovieFrame(p);
	ImageInfo info = openMovie(file)->info();
	info.m_frame = std::clamp(frame, 0, info.m_frameCount - 1);
	return info;
}

LayerImage loadMovieLayer(const ImageInfo& info, int layerIndex)
{
	const auto movie = openMovie(info.m_path);
	const FramePtr frame = movie->frame(info.m_frame);
	return movie->planes(*frame, info.m_layers[layerIndex]);
}

} // namespace rega
