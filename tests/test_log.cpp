#include "util/Log.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sys/stat.h>

#include <cstdio>
#include <sstream>
#include <unistd.h>

TEST(StderrToLog, LogsWhatIsWrittenToStderrThenPutsItBack)
{
	std::ostringstream os;
	auto logger = std::make_shared<spdlog::logger>(
	    "capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(os));
	logger->set_pattern("%l %v");
	const auto before = spdlog::default_logger();
	spdlog::set_default_logger(logger);
	struct stat was{}, now{};
	ASSERT_EQ(::fstat(STDERR_FILENO, &was), 0);
	{
		const rv::log::StderrToLog capture;
		// As OpenEXR prints, then a raw write: one line split across both,
		// a CRLF, an empty line, and an end with no newline.
		std::fprintf(stderr, "first line\nsecond");
		const char raw[] = " half\r\n\nlast without newline";
		ASSERT_EQ(::write(STDERR_FILENO, raw, sizeof raw - 1),
		          static_cast<ssize_t>(sizeof raw - 1));
	}
	spdlog::set_default_logger(before);
	EXPECT_EQ(os.str(),
	          "warning stderr: first line\n"
	          "warning stderr: second half\n"
	          "warning stderr: last without newline\n");
	ASSERT_EQ(::fstat(STDERR_FILENO, &now), 0);
	EXPECT_EQ(now.st_dev, was.st_dev);
	EXPECT_EQ(now.st_ino, was.st_ino);
}
