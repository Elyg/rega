#include "TestContext.h"
#include "app/Tutorial.h"
#include "image/Loader.h"

#include <gtest/gtest.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{

constexpr rega::tutorial::Options SMALL{.m_first = 1001,
                                        .m_frames = 3,
                                        .m_width = 48,
                                        .m_height = 27};

std::vector<std::string> layerLabels(const fs::path& p)
{
	std::vector<std::string> out;
	for(const auto& l : rega::probeImage(p).m_layers)
	{
		out.push_back(l.label());
	}
	return out;
}

} // namespace

TEST(Tutorial, WritesTheSequenceStillsAndReadme)
{
	const fs::path dir = regatest::freshDir("tutorial");
	int last = 0, total = 0;
	rega::tutorial::write(dir,
	                      SMALL,
	                      [&](int done, int n)
	                      {
		                      last = done;
		                      total = n;
	                      });
	EXPECT_EQ(last, total);
	EXPECT_EQ(total, 3 + 6 + 20);
	for(int f = 1001; f <= 1003; ++f)
	{
		EXPECT_TRUE(fs::exists(dir / ("shot." + std::to_string(f) + ".exr")));
	}
	EXPECT_FALSE(fs::exists(dir / "shot.1004.exr"));
	for(const char* still : {"chart.exr",
	                         "chart-v2.exr",
	                         "hdr-sky.exr",
	                         "luma-ramp.exr",
	                         "beachball.exr",
	                         "nan-inf.exr",
	                         "README.txt"})
	{
		EXPECT_TRUE(fs::exists(dir / still)) << still;
	}

	const auto layers = layerLabels(dir / "shot.1002.exr");
	for(const char* want : {"albedo", "diffuse", "specular", "N", "mask"})
	{
		EXPECT_NE(std::find_if(layers.begin(),
		                       layers.end(),
		                       [&](const std::string& l)
		                       { return l.find(want) != std::string::npos; }),
		          layers.end())
		    << want;
	}
	for(const char* still : {"chart.exr",
	                         "hdr-sky.exr",
	                         "luma-ramp.exr",
	                         "beachball.exr",
	                         "nan-inf.exr"})
	{
		EXPECT_GE(layerLabels(dir / still).size(), 3u) << still;
	}

	// The data window: a box inside the display window.
	const rega::ImageInfo beachball = rega::probeImage(dir / "beachball.exr");
	const rega::Box2i data = beachball.dataWindow();
	const rega::Box2i frame = beachball.displayWindow();
	EXPECT_GT(data.m_x0, frame.m_x0);
	EXPECT_GT(data.m_y0, frame.m_y0);
	EXPECT_LT(data.m_x1, frame.m_x1);
	EXPECT_LT(data.m_y1, frame.m_y1);
}

TEST(Tutorial, WipSequenceHasGapsAndAHalfWrittenLastFrame)
{
	const fs::path dir = regatest::freshDir("tutorial");
	rega::tutorial::write(dir, SMALL);
	for(int f : {1006, 1007, 1008, 1015})
	{
		EXPECT_FALSE(fs::exists(dir / fmt::format("wip.{}.exr", f))) << f;
	}
	for(int f : {1001, 1005, 1009, 1023})
	{
		const rega::ImageInfo info =
		    rega::probeImage(dir / fmt::format("wip.{}.exr", f));
		EXPECT_FALSE(rega::loadLayer(info, 0).incomplete()) << f;
	}
	const rega::ImageInfo last = rega::probeImage(dir / "wip.1024.exr");
	EXPECT_TRUE(rega::loadLayer(last, 0).incomplete());
}

TEST(Tutorial, ChartV2DiffersFromChart)
{
	const fs::path dir = regatest::freshDir("tutorial");
	rega::tutorial::write(dir, SMALL);
	const rega::LayerImage a =
	    rega::loadLayer(rega::probeImage(dir / "chart.exr"), 0);
	const rega::LayerImage b =
	    rega::loadLayer(rega::probeImage(dir / "chart-v2.exr"), 0);
	ASSERT_EQ(a.m_planes.size(), b.m_planes.size());
	int differ = 0;
	for(size_t c = 0; c < a.m_planes.size(); ++c)
	{
		for(size_t i = 0; i < a.m_planes[c].size(); ++i)
		{
			differ += a.m_planes[c][i] != b.m_planes[c][i];
		}
	}
	EXPECT_GT(differ, 0);
}

TEST(Tutorial, BrokenRenderHasNanAndInf)
{
	const fs::path dir = regatest::freshDir("tutorial");
	rega::tutorial::write(dir, SMALL);
	const rega::ImageInfo info = rega::probeImage(dir / "nan-inf.exr");
	const rega::LayerImage img = rega::loadLayer(info, 0);
	int nans = 0, infs = 0;
	for(const rega::Plane& plane : img.m_planes)
	{
		for(size_t i = 0; i < plane.size(); ++i)
		{
			const float v = plane[i];
			nans += std::isnan(v);
			infs += std::isinf(v);
		}
	}
	EXPECT_GT(nans, 0);
	EXPECT_GT(infs, 0);
}

TEST(Tutorial, EnsureRendersOnceThenAgainWhenTheSetChanges)
{
	const fs::path dir = regatest::freshDir("tutorial");
	EXPECT_TRUE(rega::tutorial::ensure(dir, SMALL));
	EXPECT_FALSE(rega::tutorial::ensure(dir, SMALL));
	rega::tutorial::Options fewer = SMALL;
	fewer.m_frames = 2;
	EXPECT_TRUE(rega::tutorial::ensure(dir, fewer));
	EXPECT_FALSE(fs::exists(dir / "shot.1003.exr")); // stale frame gone
	EXPECT_TRUE(fs::exists(dir / "shot.1002.exr"));
}
