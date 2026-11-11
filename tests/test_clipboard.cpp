#include "util/Clipboard.h"
#include "util/Sandbox.h"

#include <gtest/gtest.h>

#include <cstdlib>
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

TEST(Clipboard, LocalOnlyIsOffUnlessSetToSomethingButZero)
{
	unsetenv("REGA_LOCAL_ONLY");
	EXPECT_FALSE(localOnly());
	setenv("REGA_LOCAL_ONLY", "", 1);
	EXPECT_FALSE(localOnly());
	setenv("REGA_LOCAL_ONLY", "0", 1);
	EXPECT_FALSE(localOnly());
	setenv("REGA_LOCAL_ONLY", "1", 1);
	EXPECT_TRUE(localOnly());
	unsetenv("REGA_LOCAL_ONLY");
}

TEST(Clipboard, LocalOnlySendsNothingToTheTerminal)
{
	setenv("REGA_LOCAL_ONLY", "1", 1);
	std::ostringstream captured;
	std::streambuf* old = std::cout.rdbuf(captured.rdbuf());
	const bool ok = copyToClipboard("shot.1001.exr", false);
	std::cout.rdbuf(old);
	unsetenv("REGA_LOCAL_ONLY");
	EXPECT_FALSE(ok);
	EXPECT_EQ(captured.str(), "");
}
