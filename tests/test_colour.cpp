#include "TestContext.h"
#include "image/Colour.h"
#include "image/Loader.h"

#include <OpenColorIO/OpenColorIO.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ranges>

using namespace rv;
using namespace rvtest;
namespace OCIO = OCIO_NAMESPACE;

namespace
{

// No $OCIO while it lives (the developer's own would leak into the tests).
struct NoOcioEnv
{
	std::string m_saved;
	bool m_had = false;
	NoOcioEnv()
	{
		if(const char* e = std::getenv("OCIO"))
		{
			m_saved = e;
			m_had = true;
		}
		unsetenv("OCIO");
	}
	~NoOcioEnv()
	{
		if(m_had)
		{
			setenv("OCIO", m_saved.c_str(), 1);
		}
	}
};

} // namespace

TEST(Colour, NoConfigMeansNoTransform)
{
	NoOcioEnv env;
	ColourManager m;
	m.loadDefault();
	EXPECT_FALSE(m.active());
	EXPECT_EQ(m.configLabel(), "none (sRGB)");
	EXPECT_EQ(m.transformFor("/x/a.exr", "a", false), nullptr);
	EXPECT_EQ(m.configChoices().back().m_id, "");
}

TEST(Colour, ABrokenOcioEnvSaysSo)
{
	NoOcioEnv env;
	setenv("OCIO", "/nope/config.ocio", 1);
	ColourManager m;
	m.loadDefault();
	EXPECT_FALSE(m.active());
	EXPECT_TRUE(m.error().starts_with("$OCIO /nope/config.ocio: "))
	    << m.error();
}

TEST(Colour, BuiltInConfigListsDisplaysAndViews)
{
	NoOcioEnv env;
	ColourManager m;
	ASSERT_TRUE(m.useConfig(ColourManager::STUDIO)) << m.error();
	EXPECT_TRUE(m.active());
	EXPECT_FALSE(m.displays().empty());
	EXPECT_FALSE(m.views().empty());
	EXPECT_FALSE(m.display().empty());
	const auto views = m.views();
	EXPECT_NE(std::ranges::find(views, m.view()), views.end());
	EXPECT_EQ(m.looks().front(), "None");
	EXPECT_GT(m.colourSpaces().size(), 10u);
	// The file rules give every file a colour space.
	EXPECT_FALSE(m.inputFor("/x/a.exr", "a").empty());

	// A config that does not load leaves the current one.
	EXPECT_FALSE(m.useConfig("/nope/config.ocio"));
	EXPECT_FALSE(m.error().empty());
	EXPECT_EQ(m.configId(), ColourManager::STUDIO);
	ASSERT_TRUE(m.useConfig(""));
	EXPECT_FALSE(m.active());
}

TEST(Colour, BakedLutMatchesOcio)
{
	NoOcioEnv env;
	ColourManager m;
	ASSERT_TRUE(m.useConfig(ColourManager::STUDIO)) << m.error();
	m.setInput("a", "ACEScg");
	const auto t0 = std::chrono::steady_clock::now();
	const ColourTransformPtr lut = m.transformFor("/x/a.exr", "a", false);
	const auto ms = std::chrono::duration<double, std::milli>(
	                    std::chrono::steady_clock::now() - t0)
	                    .count();
	ASSERT_NE(lut, nullptr) << m.error();
	RecordProperty("bake_ms", static_cast<int>(ms));
	EXPECT_EQ(m.transformFor("/x/b.exr", "a", false), lut); // cached per input

	// The same transform, exact, per pixel.
	auto config = OCIO::Config::CreateFromFile(ColourManager::STUDIO);
	auto dvt = OCIO::DisplayViewTransform::Create();
	dvt->setSrc("ACEScg");
	dvt->setDisplay(m.display().c_str());
	dvt->setView(m.view().c_str());
	auto cpu = config->getProcessor(dvt)->getDefaultCPUProcessor();
	const float samples[][3] = {{0, 0, 0},
	                            {0.01f, 0.01f, 0.01f},
	                            {0.18f, 0.18f, 0.18f},
	                            {1, 1, 1},
	                            {16, 16, 16},
	                            {0.5f, 0.1f, 0.02f},
	                            {0.02f, 0.3f, 0.6f},
	                            {2.0f, 0.8f, 0.1f}};
	for(const auto& s : samples)
	{
		float exact[3] = {s[0], s[1], s[2]};
		cpu->applyRGB(exact);
		float r = s[0], g = s[1], b = s[2];
		lut->apply(r, g, b);
		const float got[3] = {r, g, b};
		for(int c = 0; c < 3; ++c)
		{
			EXPECT_NEAR(got[c], std::clamp(exact[c], 0.0f, 1.0f), 2.0f / 255)
			    << s[0] << "," << s[1] << "," << s[2] << " channel " << c;
		}
	}
}

TEST(Colour, SrgbDecodedPixelsReachOcioAsTheirCodes)
{
	NoOcioEnv env;
	ColourManager m;
	ASSERT_TRUE(m.useConfig(ColourManager::STUDIO)) << m.error();
	// A PNG / 8-bit TIFF: the file rules call it sRGB-encoded, and the loader
	// has already linearised its codes. OCIO must decode them once, not twice.
	const std::string input = m.fileRuleFor("/x/a.png");
	ASSERT_FALSE(input.empty());
	const ColourTransformPtr lut = m.transformFor("/x/a.png", "a", true);
	ASSERT_NE(lut, nullptr) << m.error();
	EXPECT_NE(m.transformFor("/x/a.png", "a", false), lut);

	auto config = OCIO::Config::CreateFromFile(ColourManager::STUDIO);
	auto dvt = OCIO::DisplayViewTransform::Create();
	dvt->setSrc(input.c_str());
	dvt->setDisplay(m.display().c_str());
	dvt->setView(m.view().c_str());
	auto cpu = config->getProcessor(dvt)->getDefaultCPUProcessor();
	const float codes[][3] = {{0, 0, 0},
	                          {0.02f, 0.02f, 0.02f},
	                          {0.5f, 0.5f, 0.93f},
	                          {0.47f, 0.55f, 1},
	                          {1, 1, 1},
	                          {0.8f, 0.3f, 0.1f}};
	for(const auto& s : codes)
	{
		float exact[3] = {s[0], s[1], s[2]};
		cpu->applyRGB(exact);
		float r = srgbToLinear(s[0]), g = srgbToLinear(s[1]),
		      b = srgbToLinear(s[2]);
		lut->apply(r, g, b);
		const float got[3] = {r, g, b};
		for(int c = 0; c < 3; ++c)
		{
			EXPECT_NEAR(got[c], std::clamp(exact[c], 0.0f, 1.0f), 2.0f / 255)
			    << s[0] << "," << s[1] << "," << s[2] << " channel " << c;
		}
	}
}

TEST(Colour, ChoicesAreRememberedPerConfig)
{
	NoOcioEnv env;
	const fs::path state = freshDir("colour") / "colour";
	std::string view;
	{
		ColourManager m(state);
		ASSERT_TRUE(m.useConfig(ColourManager::STUDIO)) << m.error();
		const auto views = m.views();
		ASSERT_GT(views.size(), 1u);
		view = views.back() == m.view() ? views.front() : views.back();
		m.setView(view);
	}
	ColourManager again(state);
	again.loadDefault(); // no $OCIO: the config picked last time
	EXPECT_EQ(again.configId(), ColourManager::STUDIO);
	EXPECT_EQ(again.view(), view);

	// Back to none: remembered too.
	ASSERT_TRUE(again.useConfig(""));
	ColourManager none(state);
	none.loadDefault();
	EXPECT_FALSE(none.active());
}
