#pragma once

// Shared fixtures for the app-layer tests: an AppContext with no terminal, a
// fresh temporary directory, small EXR files and ftxui key events.

#include "app/Annotations.h"
#include "app/AppContext.h"
#include "image/ImageService.h"
#include "term/Caps.h"

#include <ftxui/component/event.hpp>
#include <gtest/gtest.h>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace regatest
{

namespace fs = std::filesystem;

// An AppContext for a half-block terminal outside tmux. Quits are counted;
// redraws are ignored.
struct Context
{
	rega::TermCaps m_caps;
	rega::kitty::Transmitter m_kitty{false,
	                                 rega::Transfer::DIRECT,
	                                 [](std::string_view, bool) {}};
	rega::ImageService m_svc{nullptr, size_t(64) << 20, 1};
	rega::Annotations m_ann;
	rega::ColourManager m_colour; // no config, nothing saved
	std::atomic<int> m_quits{0};
	rega::AppContext m_ctx{.m_svc = m_svc,
	                       .m_caps = m_caps,
	                       .m_kitty = m_kitty,
	                       .m_ann = m_ann,
	                       .m_colour = m_colour,
	                       .m_redraw = [] {},
	                       .m_post = [](const std::function<void()>&) {},
	                       .m_quit = [this] { ++m_quits; },
	                       .m_message = {}};
};

// An empty directory of its own under the system temp dir. Keyed by the running
// test too: ctest runs each test as its own process, in parallel.
inline fs::path freshDir(std::string_view name)
{
	const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
	std::string leaf(name);
	if(test != nullptr)
	{
		leaf += std::string("-") + test->test_suite_name() + "." + test->name();
	}
	const fs::path d = fs::temp_directory_path() / "rega-tests" / leaf;
	fs::remove_all(d);
	fs::create_directories(d);
	return d;
}

inline fs::path touch(const fs::path& p, std::string_view bytes = {})
{
	std::ofstream(p, std::ios::binary) << bytes;
	return p;
}

// A small scanline EXR (FLOAT channels, all 0.5).
inline fs::path writeExr(const fs::path& p,
                         const std::vector<std::string>& channels,
                         int w = 8,
                         int h = 4)
{
	Imf::Header header(w, h);
	for(const auto& c : channels)
	{
		header.channels().insert(c, Imf::Channel(Imf::FLOAT));
	}
	std::vector<float> plane(static_cast<size_t>(w) * h, 0.5f);
	Imf::FrameBuffer fb;
	for(const auto& c : channels)
	{
		fb.insert(c,
		          Imf::Slice(Imf::FLOAT,
		                     reinterpret_cast<char*>(plane.data()),
		                     sizeof(float),
		                     sizeof(float) * w));
	}
	Imf::OutputFile out(p.string().c_str(), header);
	out.setFrameBuffer(fb);
	out.writePixels(h);
	return p;
}

// Poll `pred` until it holds (true) or `timeout` passes (false).
template <class Pred>
bool waitFor(Pred&& pred,
             std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
	const auto end = std::chrono::steady_clock::now() + timeout;
	while(!pred())
	{
		if(std::chrono::steady_clock::now() > end)
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

inline ftxui::Event key(std::string_view c)
{
	return ftxui::Event::Character(std::string(c));
}

} // namespace regatest
