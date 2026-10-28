#include "util/Clipboard.h"

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>

using namespace rega;

TEST(Clipboard, Osc52CarriesTheTextInBase64)
{
	EXPECT_EQ(osc52("hi"), "\x1b]52;c;aGk=\x07");
	EXPECT_EQ(osc52(""), "\x1b]52;c;\x07");
}

TEST(Clipboard, OutsideTmuxWritesOsc52ToTheTerminal)
{
	std::ostringstream captured;
	std::streambuf* old = std::cout.rdbuf(captured.rdbuf());
	const bool ok = copyToClipboard("pixel: [1, 2]", false);
	std::cout.rdbuf(old);
	EXPECT_TRUE(ok);
	EXPECT_EQ(captured.str(), osc52("pixel: [1, 2]"));
}

TEST(Clipboard, TmuxSelectPaneIsFalseOutsideTmux)
{
	EXPECT_FALSE(tmuxSelectPane('L', false));
	EXPECT_FALSE(tmuxSelectPane('R', false));
}
