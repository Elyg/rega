#include "app/LineEditor.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace rega;
using ftxui::Event;
using R = LineEditor::Result;

namespace
{

const std::vector<std::string> NO_HISTORY;

R send(LineEditor& ed,
       const Event& e,
       const std::vector<std::string>& history = NO_HISTORY)
{
	return ed.event(e, history);
}

void type(LineEditor& ed, const std::string& s)
{
	for(char c : s)
	{
		EXPECT_EQ(send(ed, Event::Character(c)), R::EDITED);
	}
}

const std::vector<std::string> BUILTINS{"file", "frame", "fps"};
const std::vector<std::string> KEYS{"compression", "channels", "comment"};

} // namespace

TEST(LineEditor, StartsWithTheCursorAtTheEnd)
{
	LineEditor ed("añb");
	EXPECT_EQ(ed.glyphs().size(), 3u);
	EXPECT_EQ(ed.cursor(), 3);
	EXPECT_EQ(ed.text(), "añb");
}

TEST(LineEditor, EditsByGlyphAtTheCursor)
{
	LineEditor ed("añb");
	send(ed, Event::ArrowLeft);
	EXPECT_EQ(send(ed, Event::Backspace), R::EDITED); // the ñ, both bytes
	EXPECT_EQ(ed.text(), "ab");
	EXPECT_EQ(ed.cursor(), 1);
	EXPECT_EQ(send(ed, Event::Character("é")), R::EDITED);
	EXPECT_EQ(ed.text(), "aéb");
	EXPECT_EQ(ed.cursor(), 2);
	send(ed, Event::Delete);
	EXPECT_EQ(ed.text(), "aé");
	send(ed, Event::Home);
	EXPECT_EQ(ed.cursor(), 0);
	send(ed, Event::Backspace); // nothing before the cursor
	EXPECT_EQ(ed.text(), "aé");
	send(ed, Event::End);
	EXPECT_EQ(ed.cursor(), 2);
	send(ed, Event::ArrowRight); // stays at the end
	EXPECT_EQ(ed.cursor(), 2);
}

TEST(LineEditor, CtrlWAndCtrlUDeleteBackwards)
{
	LineEditor ed("one two  ");
	send(ed, Event::CtrlW);
	EXPECT_EQ(ed.text(), "one ");
	type(ed, "three");
	send(ed, Event::ArrowLeft);
	send(ed, Event::ArrowLeft);
	send(ed, Event::CtrlU);
	EXPECT_EQ(ed.text(), "ee");
	EXPECT_EQ(ed.cursor(), 0);
}

TEST(LineEditor, EnterCommitsEscRestoresTheOriginal)
{
	LineEditor ed("orig");
	type(ed, "x");
	EXPECT_EQ(ed.text(), "origx");
	EXPECT_EQ(send(ed, Event::Escape), R::CANCEL);
	EXPECT_EQ(ed.text(), "orig");
	EXPECT_EQ(ed.original(), "orig");

	LineEditor ok("a");
	type(ok, "b");
	EXPECT_EQ(send(ok, Event::Return), R::COMMIT);
	EXPECT_EQ(ok.text(), "ab");
}

TEST(LineEditor, HistoryRecallsAndComesBackToWhatWasTyped)
{
	const std::vector<std::string> history{"two", "one"}; // recent first
	LineEditor ed("typed");
	send(ed, Event::ArrowUp, history);
	EXPECT_EQ(ed.text(), "two");
	send(ed, Event::ArrowUp, history);
	EXPECT_EQ(ed.text(), "one");
	send(ed, Event::ArrowUp, history); // the oldest stays
	EXPECT_EQ(ed.text(), "one");
	send(ed, Event::ArrowDown, history);
	EXPECT_EQ(ed.text(), "two");
	send(ed, Event::ArrowDown, history);
	EXPECT_EQ(ed.text(), "typed");
	EXPECT_EQ(ed.cursor(), 5);
	// Without history Up does nothing.
	EXPECT_EQ(send(ed, Event::ArrowUp), R::IGNORED);
	EXPECT_EQ(ed.text(), "typed");
}

TEST(LineEditor, CompletionCyclesTheMatchingKeys)
{
	LineEditor ed("x [#");
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "key 1/3");
	EXPECT_EQ(ed.text(), "x [#compression]");
	EXPECT_EQ(ed.cursor(), static_cast<int>(ed.glyphs().size()));
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "key 2/3");
	EXPECT_EQ(ed.text(), "x [#channels]");
	EXPECT_FALSE(ed.complete(BUILTINS, KEYS).empty());
	EXPECT_EQ(ed.text(), "x [#comment]");
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "key 1/3"); // round again
	EXPECT_EQ(ed.text(), "x [#compression]");
}

TEST(LineEditor, CompletionUsesWhatIsTypedOfTheKey)
{
	LineEditor ed("[#ch");
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "key 1/1");
	EXPECT_EQ(ed.text(), "[#channels]");

	LineEditor at("[#@f");
	EXPECT_EQ(at.complete(BUILTINS, KEYS), "key 1/3");
	EXPECT_EQ(at.text(), "[#@file]");
	EXPECT_FALSE(at.complete(BUILTINS, KEYS).empty());
	EXPECT_EQ(at.text(), "[#@frame]");
}

TEST(LineEditor, CompletionNeedsAnOpenToken)
{
	LineEditor ed("abc");
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "C-n completes after [# or [#@");
	EXPECT_EQ(ed.text(), "abc");

	LineEditor none("[#z");
	EXPECT_EQ(none.complete(BUILTINS, KEYS), "no key starts with 'z'");
	EXPECT_EQ(none.text(), "[#z");
}

TEST(LineEditor, AnyOtherKeyEndsTheCompletion)
{
	LineEditor ed("[#");
	EXPECT_FALSE(ed.complete(BUILTINS, KEYS).empty());
	type(ed, "x"); // after the closing ]
	EXPECT_EQ(ed.text(), "[#compression]x");
	// A fresh completion: the token is closed now.
	EXPECT_EQ(ed.complete(BUILTINS, KEYS), "C-n completes after [# or [#@");
}
