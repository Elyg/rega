// Movies: which frame comes back for which number, whatever the stream's layout,
// and what its pixels are. See scripts/make-test-movies.sh for tests/data.

#include "TestContext.h"
#include "app/ViewerState.h"
#include "image/ImageService.h"
#include "image/Loader.h"
#include "image/Sequence.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace rega;
namespace fs = std::filesystem;

namespace
{

fs::path data(const std::string& name)
{
	return fs::path(REGA_TEST_DATA) / name;
}

float linearToSrgb(float v)
{
	return v <= 0.0031308f ? v * 12.92f
	                       : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
}

// An 8-bit code value of plane `c`, at pixel `i`.
int code(const LayerImage& img, int c, size_t i)
{
	return static_cast<int>(
	    std::lround(linearToSrgb(img.m_planes[c][i]) * 255));
}

LayerImage load(const fs::path& path, int frame)
{
	const ImageInfo info = probeImage(movieFramePath(path, frame));
	return loadLayer(info, 0);
}

// Which ramp frame these pixels are: its luma is 16 + 4 * frame, video range.
int rampFrame(const LayerImage& img)
{
	const double full = code(img, 0, img.m_planes[0].size() / 2);
	return static_cast<int>(std::lround(full * 219.0 / 255.0 / 4.0));
}

// Every frame of a ramp `first`.. in `order`, one at a time.
void expectFrames(const fs::path& path,
                  const std::vector<int>& order,
                  int first = 0)
{
	for(const int f : order)
	{
		EXPECT_EQ(rampFrame(load(path, f)), first + f) << "asked for " << f;
	}
}

std::vector<int> upTo(int n)
{
	std::vector<int> all(n);
	for(int i = 0; i < n; ++i)
	{
		all[i] = i;
	}
	return all;
}

std::vector<int> shuffled(int n)
{
	std::vector<int> all = upTo(n);
	std::mt19937 rng(7);
	std::shuffle(all.begin(), all.end(), rng);
	return all;
}

std::vector<int> backwards(int n)
{
	std::vector<int> all = upTo(n);
	std::reverse(all.begin(), all.end());
	return all;
}

} // namespace

TEST(MoviePaths, FramePathsSplitBackIntoFileAndFrame)
{
	const fs::path frame = movieFramePath("/shots/a.mov", 41);
	EXPECT_EQ(frame, "/shots/a.mov#41");
	EXPECT_EQ(splitMovieFrame(frame).first, "/shots/a.mov");
	EXPECT_EQ(splitMovieFrame(frame).second, 41);
	EXPECT_EQ(splitMovieFrame("/shots/A.MP4#0").first, "/shots/A.MP4");
}

TEST(MoviePaths, OtherPathsAreThemselves)
{
	for(const char* p : {"/shots/a.mov",
	                     "/shots/seq#1/a.mov",
	                     "/shots/a.exr#3",
	                     "/shots/a.mov#",
	                     "/shots/a.mov#x",
	                     "/shots/a.mov#12345678901"})
	{
		EXPECT_EQ(splitMovieFrame(p).first, p);
		EXPECT_EQ(splitMovieFrame(p).second, 0);
	}
}

TEST(MovieListing, MoviesAreImagesButNeverGroupIntoASequence)
{
	const auto list =
	    groupFiles("/shots", {"take_v001.mov", "take_v002.mov", "b.MP4"});
	ASSERT_EQ(list.size(), 3u);
	for(const auto& e : list)
	{
		EXPECT_EQ(e.m_kind, Entry::Kind::FILE);
		EXPECT_TRUE(e.isImage());
	}
}

TEST(MovieEntry, OpensAsASequenceNumberedFromOne)
{
	const Entry e = movieEntry(data("gop.mp4"));
	EXPECT_TRUE(e.isMovie());
	EXPECT_EQ(e.m_kind, Entry::Kind::SEQUENCE);
	EXPECT_EQ(e.m_name, "gop.mp4");
	EXPECT_EQ(e.m_path, data("gop.mp4"));
	ASSERT_EQ(e.m_frames.size(), 48u);
	EXPECT_EQ(e.m_frames[3], movieFramePath(data("gop.mp4"), 3));
	EXPECT_EQ(e.m_frameNumbers.front(), 1);
	EXPECT_EQ(e.m_frameNumbers.back(), 48);
	EXPECT_EQ(e.rangeString(), "1-48");

	const Source s{e};
	EXPECT_EQ(s.frameCount(), 48);
	EXPECT_EQ(s.frameLabel(0), "1");
	EXPECT_EQ(s.frame(47), e.m_frames.back());
}

TEST(MovieEntry, AnUnreadableFileStaysAPlainFile)
{
	const fs::path p = regatest::freshDir("movie") / "notes.mov";
	std::ofstream(p) << "not a movie";
	const Entry e = movieEntry(p);
	EXPECT_EQ(e.m_kind, Entry::Kind::FILE);
	EXPECT_FALSE(e.isMovie());
	EXPECT_THROW(probeImage(p), LoadError);
}

TEST(MovieProbe, ReportsWhatTheViewerNeeds)
{
	const ImageInfo info = probeImage(data("gop.mp4"));
	EXPECT_EQ(info.m_format, "mp4");
	EXPECT_EQ(info.m_path, data("gop.mp4").string());
	EXPECT_TRUE(info.m_displayReferred);
	EXPECT_EQ(info.m_frameCount, 48);
	EXPECT_EQ(info.m_frame, 0);
	ASSERT_TRUE(info.m_fps.has_value());
	EXPECT_DOUBLE_EQ(*info.m_fps, 24.0);
	EXPECT_EQ(info.displayWindow().width(), 64);
	EXPECT_EQ(info.displayWindow().height(), 32);
	EXPECT_EQ(info.m_parts[0].m_compression, "h264");
	ASSERT_EQ(info.m_layers.size(), 1u);
	EXPECT_EQ(info.m_layers[0].m_channels,
	          (std::vector<std::string>{"R", "G", "B"}));

	const ImageInfo frame = probeImage(movieFramePath(data("gop.mp4"), 12));
	EXPECT_EQ(frame.m_frame, 12);
	EXPECT_EQ(frame.m_path, data("gop.mp4").string()); // shown and copied
}

TEST(MovieProbe, ACodecLeftOutOfTheBuildSaysWhich)
{
	EXPECT_THROW(
	    try { probeImage(data("av1.mp4")); } catch(const LoadError& e) {
		    EXPECT_STREQ(e.what(), "no decoder for av1 video");
		    throw;
	    },
	    LoadError);
}

// Each layout, read in order, at random and backwards.
class MovieFrames
    : public ::testing::TestWithParam<std::tuple<const char*, int, int>>
{
protected:
	fs::path path() const
	{
		return data(std::get<0>(GetParam()));
	}
	int count() const
	{
		return std::get<1>(GetParam());
	}
	int first() const
	{
		return std::get<2>(GetParam());
	}
};

TEST_P(MovieFrames, CountsOnlyTheFramesShown)
{
	EXPECT_EQ(probeImage(path()).m_frameCount, count());
}

TEST_P(MovieFrames, InOrder)
{
	expectFrames(path(), upTo(count()), first());
}

TEST_P(MovieFrames, AtRandom)
{
	expectFrames(path(), shuffled(count()), first());
}

TEST_P(MovieFrames, Backwards)
{
	expectFrames(path(), backwards(count()), first());
}

TEST_P(MovieFrames, AnyFrameFirstInAFreshlyOpenedFile)
{
	const fs::path dir = regatest::freshDir("movie");
	for(int f = 0; f < count(); ++f)
	{
		// A file of its own: nothing decoded yet to find the frame among.
		const fs::path copy =
		    dir / (std::to_string(f) + path().extension().string());
		fs::copy_file(path(), copy);
		EXPECT_EQ(rampFrame(load(copy, f)), first() + f);
	}
}

INSTANTIATE_TEST_SUITE_P(Layouts,
                         MovieFrames,
                         ::testing::Values(std::tuple{"gop.mp4", 48, 0},
                                           std::tuple{"open_gop.mp4", 48, 0},
                                           std::tuple{"fragmented.mp4", 48, 0},
                                           std::tuple{"vfr.mp4", 48, 0},
                                           std::tuple{"trimmed.mov", 33, 15}),
                         [](const auto& info)
                         {
	                         std::string name = std::get<0>(info.param);
	                         std::replace(name.begin(), name.end(), '.', '_');
	                         return name;
                         });

TEST(MovieFrames, PastTheEndClampsToTheLast)
{
	EXPECT_EQ(rampFrame(load(data("gop.mp4"), 1000)), 47);
}

TEST(MovieFrames, APlainPathIsTheFirstFrame)
{
	EXPECT_EQ(rampFrame(loadLayer(probeImage(data("gop.mp4")), 0)), 0);
}

TEST(MovieService, WorkersAskingAllAtOnceEachGetTheirOwnFrame)
{
	ImageService svc(nullptr, size_t(64) << 20, 3);
	const Entry e = movieEntry(data("open_gop.mp4"));
	for(const int f : shuffled(48))
	{
		svc.layer(e.m_frames[f], "", 1);
	}
	for(int f = 47; f >= 0; --f)
	{
		EXPECT_TRUE(regatest::waitFor(
		    [&] { return svc.hasLayer(e.m_frames[f], "", 1); }));
		const LayerImagePtr img = svc.layer(e.m_frames[f], "", 1);
		ASSERT_TRUE(img);
		EXPECT_EQ(rampFrame(*img), f);
	}
	EXPECT_EQ(svc.originOf(e.m_frames[3], "", 1).find(e.m_frames[3].string()),
	          2u);
}

TEST(MovieService, AnOverwrittenMovieIsReadAgain)
{
	const fs::path p = regatest::freshDir("movie") / "clip.mp4";
	fs::copy_file(data("gop.mp4"), p);
	ImageService svc(nullptr, size_t(64) << 20, 1);
	const fs::path frame = movieFramePath(p, 20);
	svc.layer(frame, "", 1);
	ASSERT_TRUE(regatest::waitFor([&] { return svc.hasLayer(frame, "", 1); }));
	EXPECT_EQ(rampFrame(*svc.layer(frame, "", 1)), 20);

	// Same frames, later: frame 20 now holds what was frame 35.
	fs::copy_file(data("trimmed.mov"), p, fs::copy_options::overwrite_existing);
	fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(5));
	EXPECT_TRUE(svc.checkForChanges({p}));
	EXPECT_FALSE(svc.hasLayer(frame, "", 1));
	svc.layer(frame, "", 1);
	ASSERT_TRUE(regatest::waitFor([&] { return svc.hasLayer(frame, "", 1); }));
	EXPECT_EQ(rampFrame(*svc.layer(frame, "", 1)), 35);
}

TEST(MoviePixels, LimitedBt709ComesBackAsTheCodeValuesEncoded)
{
	const LayerImage img = load(data("bt709_limited.mp4"), 0);
	EXPECT_NEAR(code(img, 0, 0), 0x30, 2);
	EXPECT_NEAR(code(img, 1, 0), 0x60, 2);
	EXPECT_NEAR(code(img, 2, 0), 0xC0, 2);
}

TEST(MoviePixels, FullRangeBt601ComesBackAsTheCodeValuesEncoded)
{
	const LayerImage img = load(data("bt601_full.mp4"), 0);
	EXPECT_NEAR(code(img, 0, 0), 0x30, 2);
	EXPECT_NEAR(code(img, 1, 0), 0x60, 2);
	EXPECT_NEAR(code(img, 2, 0), 0xC0, 2);
}

TEST(MoviePixels, RotationIsApplied)
{
	const ImageInfo info = probeImage(data("rotated.mp4"));
	EXPECT_EQ(info.displayWindow().width(), 16);
	EXPECT_EQ(info.displayWindow().height(), 32);
	const LayerImage img = load(data("rotated.mp4"), 0);
	ASSERT_EQ(img.m_width, 16);
	ASSERT_EQ(img.m_height, 32);
	// Stored left half bright, turned a quarter clockwise: top half bright.
	EXPECT_GT(code(img, 0, 8), 200);
	EXPECT_LT(code(img, 0, size_t(31) * 16 + 8), 70);
}

TEST(MoviePixels, AlphaIsAChannelAndHighBitDepthsStayFloat)
{
	const ImageInfo info = probeImage(data("alpha.mov"));
	EXPECT_EQ(info.m_layers[0].m_channels,
	          (std::vector<std::string>{"R", "G", "B", "A"}));
	EXPECT_EQ(info.m_layers[0].m_types[0], "uint16");
	const LayerImage first = load(data("alpha.mov"), 0);
	const LayerImage last = load(data("alpha.mov"), info.m_frameCount - 1);
	ASSERT_EQ(first.m_planes.size(), 4u);
	EXPECT_FALSE(first.m_planes[0].isHalf()); // 12-bit
	EXPECT_LT(first.m_planes[3][0], last.m_planes[3][0]);
	EXPECT_NEAR(first.m_planes[3][0], 256 / 1023.0f, 0.02f); // not through sRGB
}
