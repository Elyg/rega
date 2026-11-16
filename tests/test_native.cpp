// GraphicsMode::NATIVE: pictures handed to the window instead of a terminal.

#include "image/ImageBuffer.h"
#include "term/BitmapSlot.h"
#include "term/ImageView.h"
#include "term/Kitty.h"
#include "term/Native.h"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace rega;
using namespace std::chrono_literals;

namespace
{

LayerImagePtr grey(float v)
{
	LayerImage img;
	img.m_width = img.m_height = 4;
	img.m_dataWindow = img.m_displayWindow = {0, 0, 3, 3};
	img.m_channelNames = {"R", "G", "B"};
	img.m_planes.assign(3, Plane(std::vector<float>(16, v)));
	return std::make_shared<const LayerImage>(std::move(img));
}

template <class Pred>
bool waitFor(Pred&& pred)
{
	const auto end = std::chrono::steady_clock::now() + 3s;
	while(!pred())
	{
		if(std::chrono::steady_clock::now() > end)
		{
			return false;
		}
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

// The id a marked cell carries.
uint32_t cellId(const ScreenCell& px)
{
	const std::string s = px.foreground_color.Print(false); // 38;2;r;g;b
	unsigned r = 0, g = 0, b = 0;
	std::sscanf(s.c_str(), "38;2;%u;%u;%u", &r, &g, &b);
	return nativeCellId(static_cast<uint8_t>(r),
	                    static_cast<uint8_t>(g),
	                    static_cast<uint8_t>(b));
}

// A window's slot on a 10x4 screen of 3x5 px cells.
struct NativeSlot
{
	NativeImages m_native;
	TermCaps m_caps;
	kitty::Transmitter m_tx{false,
	                        Transfer::DIRECT,
	                        [](std::string_view, bool) {}};
	std::atomic<int> m_ready{0};
	std::unique_ptr<ImageSlot> m_slot;
	ftxui::Screen m_screen{10, 4};

	NativeSlot()
	{
		ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
		m_caps.m_graphics = GraphicsMode::NATIVE;
		m_caps.m_cellW = 3;
		m_caps.m_cellH = 5;
		m_caps.m_native = &m_native;
		m_slot =
		    std::make_unique<ImageSlot>(m_caps, m_tx, [this] { ++m_ready; });
	}
	void draw(const LayerImagePtr& img,
	          ftxui::Box box = {0, 9, 0, 3},
	          const ViewParams& view = {})
	{
		m_screen.Clear();
		m_native.begin();
		m_slot->draw(m_screen, box, img, view, {});
	}
};

} // namespace

TEST(Native, PlaceMarksTheShownCellsWithTheId)
{
	ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
	NativeImages native;
	ftxui::Screen screen(8, 4);
	const uint32_t id = native.allocId() + 0x123456; // all three bytes used
	auto bmp = std::make_shared<const Rgba8Image>(Rgba8Image{6, 4, {}});
	native.place(screen, id, 1, 1, 5, 3, 3, 2, bmp);

	ASSERT_EQ(native.placements().size(), 1u);
	const auto& p = native.placements()[0];
	EXPECT_EQ(p.m_x, 1);
	EXPECT_EQ(p.m_cols, 5); // the picture's extent...
	EXPECT_EQ(p.m_rows, 3);
	for(int y = 0; y < 4; ++y)
	{
		for(int x = 0; x < 8; ++x)
		{
			// ...but only what shows is marked.
			const bool in = x >= 1 && x < 4 && y >= 1 && y < 3;
			EXPECT_EQ(screen.PixelAt(x, y).character == NATIVE_CELL, in)
			    << x << "," << y;
			if(in)
			{
				EXPECT_EQ(cellId(screen.PixelAt(x, y)), id);
			}
		}
	}
	native.begin();
	EXPECT_TRUE(native.placements().empty());
}

TEST(Native, SlotRendersAtScreenPixelsOnItsThread)
{
	NativeSlot s;
	const auto img = grey(0.5f);
	s.draw(img);
	// Prepared in the background; the next draw shows it.
	ASSERT_TRUE(waitFor([&] { return s.m_ready > 0; }));
	s.draw(img);
	ASSERT_EQ(s.m_native.placements().size(), 1u);
	const auto& p = s.m_native.placements()[0];
	EXPECT_EQ(p.m_cols, 10);
	EXPECT_EQ(p.m_rows, 4);
	ASSERT_TRUE(p.m_bitmap);
	EXPECT_EQ(p.m_bitmap->m_width, 30); // 10 cells of 3 px
	EXPECT_EQ(p.m_bitmap->m_height, 20);
	EXPECT_EQ(s.m_screen.PixelAt(0, 0).character, NATIVE_CELL);
	EXPECT_EQ(s.m_slot->pxPerCellX(), 3);
	EXPECT_EQ(s.m_slot->pxPerCellY(), 5);
}

TEST(Native, SameDrawHandsOverTheSamePicture)
{
	NativeSlot s;
	const auto img = grey(0.25f);
	s.draw(img);
	ASSERT_TRUE(waitFor([&] { return s.m_ready > 0; }));
	s.draw(img);
	ASSERT_EQ(s.m_native.placements().size(), 1u);
	const auto first = s.m_native.placements()[0].m_bitmap;
	s.draw(img);
	ASSERT_EQ(s.m_native.placements().size(), 1u);
	// Unchanged: the window keeps its texture (it compares the pointer).
	EXPECT_EQ(s.m_native.placements()[0].m_bitmap, first);
}

TEST(Native, ResizedBoxShowsTheOldPictureScaledUntilTheNewOne)
{
	NativeSlot s;
	const auto img = grey(0.5f);
	s.draw(img);
	ASSERT_TRUE(waitFor([&] { return s.m_ready > 0; }));
	s.draw(img);
	const int before = s.m_ready;
	s.draw(img, {0, 4, 0, 3}); // half as wide
	ASSERT_EQ(s.m_native.placements().size(), 1u);
	{
		// The old picture, scaled to where the new box fits the image: the
		// 4 px image was 5 px a pixel in 30 x 20, now 3.75 in 15 x 20.
		const auto& p = s.m_native.placements()[0];
		EXPECT_EQ(p.m_cols, 5);
		EXPECT_EQ(p.m_bitmap->m_width, 30);
		ASSERT_TRUE(p.m_at);
		EXPECT_DOUBLE_EQ(p.m_at->m_x, -3.75);
		EXPECT_DOUBLE_EQ(p.m_at->m_y, 2.5);
		EXPECT_DOUBLE_EQ(p.m_at->m_w, 22.5);
		EXPECT_DOUBLE_EQ(p.m_at->m_h, 15.0);
	}
	EXPECT_EQ(s.m_screen.PixelAt(4, 0).character, NATIVE_CELL);
	EXPECT_NE(s.m_screen.PixelAt(5, 0).character, NATIVE_CELL);
	ASSERT_TRUE(waitFor([&] { return s.m_ready > before; }));
	s.draw(img, {0, 4, 0, 3});
	EXPECT_EQ(s.m_native.placements()[0].m_cols, 5);
	EXPECT_EQ(s.m_native.placements()[0].m_bitmap->m_width, 15);
	EXPECT_FALSE(s.m_native.placements()[0].m_at); // its own: as made
}

TEST(Native, APanMovesTheLastPictureAtOnce)
{
	NativeSlot s;
	const auto img = grey(0.5f);
	ViewParams view;
	view.m_fit = false;
	view.m_zoom = 5.0;
	view.m_centerX = view.m_centerY = 2.0;
	s.draw(img, {0, 9, 0, 3}, view);
	ASSERT_TRUE(waitFor([&] { return s.m_ready > 0; }));
	s.draw(img, {0, 9, 0, 3}, view);
	const auto made = s.m_native.placements()[0].m_bitmap;
	EXPECT_FALSE(s.m_native.placements()[0].m_at);

	// One image pixel right: before its own picture is ready, the last one
	// drawn 5 window pixels to the left.
	view.m_centerX = 3.0;
	s.draw(img, {0, 9, 0, 3}, view);
	const auto& p = s.m_native.placements()[0];
	EXPECT_EQ(p.m_bitmap, made);
	ASSERT_TRUE(p.m_at);
	EXPECT_DOUBLE_EQ(p.m_at->m_x, -5.0);
	EXPECT_DOUBLE_EQ(p.m_at->m_y, 0.0);
	EXPECT_DOUBLE_EQ(p.m_at->m_w, 30.0);
	EXPECT_DOUBLE_EQ(p.m_at->m_h, 20.0);
}

TEST(Native, BitmapSlotPaintsAtScreenPixels)
{
	ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
	NativeImages native;
	TermCaps caps;
	caps.m_graphics = GraphicsMode::NATIVE;
	caps.m_cellW = 4;
	caps.m_cellH = 8;
	caps.m_native = &native;
	kitty::Transmitter tx{false,
	                      Transfer::DIRECT,
	                      [](std::string_view, bool) {}};
	BitmapSlot slot(caps, tx);
	ftxui::Screen screen(6, 2);
	int asked = 0;
	slot.draw(screen,
	          {0, 5, 0, 1},
	          [&](int w, int h)
	          {
		          ++asked;
		          EXPECT_EQ(w, 24);
		          EXPECT_EQ(h, 16);
		          return Rgba8Image{
		              w,
		              h,
		              std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 9)};
	          });
	EXPECT_EQ(asked, 1);
	ASSERT_EQ(native.placements().size(), 1u);
	EXPECT_EQ(native.placements()[0].m_bitmap->m_width, 24);
	EXPECT_EQ(screen.PixelAt(5, 1).character, NATIVE_CELL);
}

TEST(Native, PixelGraphicsModes)
{
	EXPECT_TRUE(drawsPixels(GraphicsMode::KITTY));
	EXPECT_TRUE(drawsPixels(GraphicsMode::NATIVE));
	EXPECT_FALSE(drawsPixels(GraphicsMode::HALF_BLOCK));
	EXPECT_STREQ(graphicsModeName(GraphicsMode::NATIVE), "window");
}
