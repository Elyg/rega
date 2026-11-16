// The window's start screen and the recent list behind it.

#include "TestContext.h"
#include "app/Recent.h"
#include "app/StartScreen.h"
#include "util/Ui.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include <set>
#include <thread>

using namespace rega;
using regatest::freshDir;
using regatest::key;
using regatest::touch;

namespace fs = std::filesystem;

TEST(Recent, NewestFirstWithoutDuplicatesAndCapped)
{
	const fs::path dir = freshDir("recent");
	Recent r(dir / "recent");
	r.add({"/a.exr"}, 1);
	r.add({"/b.exr", "/c.exr"}, 2);
	r.add({"/a.exr"}, 3); // again: moves up, not twice
	ASSERT_EQ(r.items().size(), 2u);
	EXPECT_EQ(r.items()[0], (RecentItem{{"/a.exr"}, 3}));
	EXPECT_EQ(r.items()[1], (RecentItem{{"/b.exr", "/c.exr"}, 2}));
	for(int i = 0; i < 20; ++i)
	{
		r.add({"/f" + std::to_string(i) + ".exr"}, 10 + i);
	}
	ASSERT_EQ(r.items().size(), Recent::MAX_ITEMS);
	EXPECT_EQ(r.items().front().m_paths.front(), "/f19.exr");
}

TEST(Recent, SavedAndLoadedAgain)
{
	const fs::path dir = freshDir("recent");
	{
		Recent r(dir / "state" / "recent"); // the folder is made
		r.add({"/renders/shot.####.exr"}, 100);
		r.add({"/x y/é.png", "/x y/b.png"}, 200);
		r.add({"/bad\tname.exr"}, 300); // can't be kept: left out
	}
	Recent r(dir / "state" / "recent");
	r.load();
	ASSERT_EQ(r.items().size(), 2u);
	EXPECT_EQ(r.items()[0], (RecentItem{{"/x y/é.png", "/x y/b.png"}, 200}));
	EXPECT_EQ(r.items()[1], (RecentItem{{"/renders/shot.####.exr"}, 100}));
	r.remove(0);
	Recent again(dir / "state" / "recent");
	again.load();
	ASSERT_EQ(again.items().size(), 1u);
	EXPECT_EQ(again.items()[0].m_when, 100);
}

TEST(Recent, WithoutAFileNothingIsRemembered)
{
	Recent r;
	EXPECT_FALSE(r.enabled());
	r.add({"/a.exr"}, 1);
	EXPECT_TRUE(r.items().empty());
}

TEST(Recent, BrokenLinesAreSkipped)
{
	const fs::path dir = freshDir("recent");
	touch(dir / "recent",
	      "junk\n12\t/a.exr\nx\t/b.exr\n\n34\t/c.exr\t/d.exr\r\n");
	Recent r(dir / "recent");
	r.load();
	ASSERT_EQ(r.items().size(), 2u);
	EXPECT_EQ(r.items()[0], (RecentItem{{"/a.exr"}, 12}));
	EXPECT_EQ(r.items()[1], (RecentItem{{"/c.exr", "/d.exr"}, 34}));
}

TEST(Recent, PatternsExistWhenTheirFolderDoes)
{
	const fs::path dir = freshDir("recent");
	touch(dir / "a.exr");
	EXPECT_TRUE(recentPathExists((dir / "a.exr").string()));
	EXPECT_FALSE(recentPathExists((dir / "gone.exr").string()));
	EXPECT_TRUE(recentPathExists((dir / "shot.####.exr").string()));
	EXPECT_FALSE(recentPathExists((dir / "gone" / "shot.*.exr").string()));
}

TEST(Recent, Labels)
{
	const fs::path dir = freshDir("recent");
	const fs::path home = dir;
	fs::create_directories(dir / "plates");
	auto label = [&](std::vector<std::string> paths)
	{ return recentLabel({std::move(paths), 0}, home); };
	const std::string sep(1, fs::path::preferred_separator);

	RecentLabel l = label({(dir / "renders" / "shot.####.exr").string()});
	EXPECT_EQ(l.m_name, "shot.####.exr");
	EXPECT_EQ(l.m_where, "~" + sep + "renders");

	l = label({(dir / "plates").string()});
	EXPECT_EQ(l.m_name, "plates/");
	EXPECT_EQ(l.m_where, "~");

	l = label({(dir / "a.exr").string(),
	           (dir / "b.exr").string(),
	           (dir / "c.exr").string()});
	EXPECT_EQ(l.m_name, "a.exr +2 more");

	EXPECT_EQ(tildePath(dir.parent_path(), home), dir.parent_path().string());
}

TEST(Recent, AgoText)
{
	EXPECT_EQ(agoText(5), "just now");
	EXPECT_EQ(agoText(5 * 60), "5 min ago");
	EXPECT_EQ(agoText(3 * 3600 + 10), "3 h ago");
	EXPECT_EQ(agoText(86400), "1 day ago");
	EXPECT_EQ(agoText(3 * 86400), "3 days ago");
	EXPECT_EQ(agoText(15 * 86400), "2 weeks ago");
}

namespace
{

struct Start
{
	regatest::Context m_c;
	fs::path m_dir = freshDir("start");
	Recent m_recent{m_dir / "recent"};
	std::vector<std::string> m_calls;
	StartScreen m_screen{m_c.m_ctx,
	                     m_recent,
	                     {.m_dialog = [this](bool folder)
	                      { m_calls.push_back(folder ? "folder" : "files"); },
	                      .m_browse = [this] { m_calls.push_back("browse"); },
	                      .m_tutorial = [this]
	                      { m_calls.push_back("tutorial"); },
	                      .m_open =
	                          [this](const std::vector<std::string>& paths)
	                      {
		                      std::string all;
		                      for(const auto& p : paths)
		                      {
			                      all += (all.empty() ? "" : " ") + p;
		                      }
		                      m_calls.push_back("open " + all);
	                      }}};
};

} // namespace

TEST(StartScreen, KeysDoWhatTheRowsSay)
{
	Start s;
	touch(s.m_dir / "a.exr");
	touch(s.m_dir / "b.exr");
	s.m_recent.add({(s.m_dir / "gone.exr").string()}, 1); // not listed
	s.m_recent.add({(s.m_dir / "a.exr").string()}, 2);
	s.m_recent.add({(s.m_dir / "b.exr").string()}, 3);
	s.m_screen.show();
	ASSERT_EQ(s.m_screen.shown().size(), 2u);

	EXPECT_TRUE(s.m_screen.event(key("o")));
	EXPECT_TRUE(s.m_screen.event(key("f")));
	EXPECT_TRUE(s.m_screen.event(key("b")));
	EXPECT_TRUE(s.m_screen.event(key("2")));
	EXPECT_TRUE(s.m_screen.event(key("9"))); // nothing there
	ASSERT_EQ(s.m_calls.size(), 4u);
	EXPECT_EQ(s.m_calls[0], "files");
	EXPECT_EQ(s.m_calls[1], "folder");
	EXPECT_EQ(s.m_calls[2], "browse");
	EXPECT_EQ(s.m_calls[3], "open " + (s.m_dir / "a.exr").string());

	// Down to the first recent one (b), Enter.
	s.m_calls.clear();
	for(int i = 0; i < s.m_screen.firstRecent(); ++i)
	{
		(void)s.m_screen.event(ftxui::Event::ArrowDown);
	}
	EXPECT_EQ(s.m_screen.selected(), s.m_screen.firstRecent());
	(void)s.m_screen.event(ftxui::Event::Return);
	ASSERT_EQ(s.m_calls.size(), 1u);
	EXPECT_EQ(s.m_calls[0], "open " + (s.m_dir / "b.exr").string());

	// x forgets it, for good.
	(void)s.m_screen.event(key("x"));
	ASSERT_EQ(s.m_screen.shown().size(), 1u);
	EXPECT_EQ(s.m_recent.items().size(), 2u); // the missing one stays saved
	EXPECT_EQ(s.m_screen.shown()[0].m_paths[0], (s.m_dir / "a.exr").string());

	EXPECT_TRUE(s.m_screen.event(key("q")));
	EXPECT_EQ(s.m_c.m_quits, 1);
}

TEST(StartScreen, ClicksOpenTheRowUnderThePointer)
{
	Start s;
	s.m_screen.show();
	ui::setScreenSize(ftxui::Dimensions{100, 30});
	ftxui::Element el = s.m_screen.render(s.m_dir);
	ftxui::Screen screen(100, 30);
	ftxui::Render(screen, el);
	ui::setScreenSize(std::nullopt);
	EXPECT_NE(screen.ToString().find("Open a folder"), std::string::npos);
	EXPECT_EQ(screen.ToString().find("Recent"), std::string::npos); // none yet

	// Find the "Open a folder" row and click it.
	int row = -1;
	for(int y = 0; y < 30 && row < 0; ++y)
	{
		std::string line;
		for(int x = 0; x < 100; ++x)
		{
			line += screen.PixelAt(x, y).character;
		}
		if(line.find("Open a folder") != std::string::npos)
		{
			row = y;
		}
	}
	ASSERT_GE(row, 0);
	ftxui::Mouse m;
	m.button = ftxui::Mouse::Left;
	m.motion = ftxui::Mouse::Pressed;
	m.x = 50;
	m.y = row;
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Mouse("", m)));
	ASSERT_EQ(s.m_calls.size(), 1u);
	EXPECT_EQ(s.m_calls[0], "folder");
	EXPECT_EQ(s.m_screen.selected(), StartScreen::OPEN_FOLDER);
}

TEST(StartScreen, RecentRowsShowNameFolderAndAge)
{
	Start s;
	fs::create_directories(s.m_dir / "v002");
	touch(s.m_dir / "v002" / "comp.exr");
	s.m_recent.add({(s.m_dir / "v002" / "comp.exr").string()}, 0);
	s.m_screen.show();
	ui::setScreenSize(ftxui::Dimensions{120, 30});
	ftxui::Element el = s.m_screen.render(s.m_dir);
	ftxui::Screen screen(120, 30);
	ftxui::Render(screen, el);
	ui::setScreenSize(std::nullopt);
	const std::string out = screen.ToString();
	EXPECT_NE(out.find("comp.exr"), std::string::npos);
	EXPECT_NE(out.find("v002"), std::string::npos);
	EXPECT_NE(out.find("weeks ago"), std::string::npos);
	EXPECT_EQ(out.find("nothing opened yet"), std::string::npos);
}

TEST(StartScreen, PastedPathsAreCleanedUp)
{
	const fs::path home = "/home/me";
	EXPECT_EQ(pastedPath("  /a/b.exr \r", home), "/a/b.exr");
	EXPECT_EQ(pastedPath("\"/a b/c.exr\"", home), "/a b/c.exr");
	EXPECT_EQ(pastedPath("'/x.exr'", home), "/x.exr");
	EXPECT_EQ(pastedPath("file:///renders/v%20002/shot.%23%23%23%23.exr", home),
	          "/renders/v 002/shot.####.exr");
	EXPECT_EQ(pastedPath("file://localhost/a.exr", home), "/a.exr");
	EXPECT_EQ(pastedPath("file:///C:/shots/a.exr", home), "C:/shots/a.exr");
	EXPECT_EQ(pastedPath("~/renders", home), "/home/me/renders");
	EXPECT_EQ(pastedPath("   ", home), "");
	EXPECT_EQ(pastedPaths("/a.exr\n\n  /b.exr\r\n", home),
	          (std::vector<std::string>{"/a.exr", "/b.exr"}));
}

TEST(StartScreen, PastedPathsListUpAndOpenTogether)
{
	Start s;
	fs::create_directories(s.m_dir / "v001");
	fs::create_directories(s.m_dir / "v002");
	regatest::writeExr(s.m_dir / "v001" / "comp.exr", {"R", "G", "B"});
	regatest::writeExr(s.m_dir / "v002" / "comp.exr", {"R", "G", "B"});
	s.m_screen.show();
	const std::string a = (s.m_dir / "v001" / "comp.exr").string();
	const std::string b = (s.m_dir / "v002" / "comp.exr").string();
	const std::string gone = (s.m_dir / "gone.exr").string();

	// Two lines at once: two rows; the cursor on "Open these together".
	s.m_screen.paste(a + "\n" + gone + "\n");
	ASSERT_EQ(s.m_screen.pasted().size(), 2u);
	EXPECT_TRUE(s.m_screen.pasted()[0].m_found);
	EXPECT_FALSE(s.m_screen.pasted()[1].m_found);
	EXPECT_EQ(s.m_screen.selected(), s.m_screen.openPastedRow());
	s.m_screen.paste(a); // again: not twice
	EXPECT_EQ(s.m_screen.pasted().size(), 2u);

	// Typed into the field: Enter adds it, the field stays for the next.
	EXPECT_TRUE(s.m_screen.event(key("a")));
	EXPECT_TRUE(s.m_screen.typing());
	s.m_screen.paste(b); // one line, typing: into the field
	EXPECT_EQ(s.m_screen.pasted().size(), 2u);
	EXPECT_TRUE(s.m_screen.event(key("q"))); // text now, not quit
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Backspace));
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Return));
	EXPECT_TRUE(s.m_screen.typing());
	ASSERT_EQ(s.m_screen.pasted().size(), 3u);
	EXPECT_EQ(s.m_screen.pasted()[2].m_path, b);
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Escape));
	EXPECT_FALSE(s.m_screen.typing());
	EXPECT_EQ(s.m_c.m_quits, 0);

	// x on the missing one; Enter on the button opens the rest together.
	for(int i = 0;
	    i < 20 && s.m_screen.selected() != s.m_screen.firstPasted() + 1;
	    ++i)
	{
		(void)s.m_screen.event(ftxui::Event::ArrowDown);
	}
	EXPECT_TRUE(s.m_screen.event(key("x")));
	ASSERT_EQ(s.m_screen.pasted().size(), 2u);
	while(s.m_screen.selected() != s.m_screen.openPastedRow())
	{
		(void)s.m_screen.event(ftxui::Event::ArrowDown);
	}
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Return));
	ASSERT_EQ(s.m_calls.size(), 1u);
	EXPECT_EQ(s.m_calls[0], "open " + a + " " + b);
	EXPECT_TRUE(s.m_screen.event(key("X"))); // clear the list
	EXPECT_TRUE(s.m_screen.pasted().empty());
}

TEST(StartScreen, RecentItemsGoToTheListToOpenTogether)
{
	Start s;
	for(const char* n : {"a.exr", "b.exr", "c.exr"})
	{
		touch(s.m_dir / n);
	}
	s.m_recent.add({(s.m_dir / "c.exr").string()}, 1);
	s.m_recent.add({(s.m_dir / "b.exr").string()}, 2);
	s.m_recent.add({(s.m_dir / "a.exr").string()}, 3); // a, b, c
	s.m_screen.show();
	auto listed = [&]
	{
		std::string out;
		for(const auto& p : s.m_screen.pasted())
		{
			out += fs::path(p.m_path).filename().string() + " ";
		}
		return out;
	};
	while(s.m_screen.selected() != s.m_screen.firstRecent())
	{
		(void)s.m_screen.event(ftxui::Event::ArrowDown);
	}

	// Space puts it in the list (the cursor stays on it); J a range more.
	EXPECT_TRUE(s.m_screen.event(key(" ")));
	EXPECT_EQ(listed(), "a.exr ");
	EXPECT_TRUE(s.m_screen.inList(0));
	EXPECT_EQ(s.m_screen.selected(), s.m_screen.firstRecent());
	EXPECT_TRUE(s.m_screen.event(key("J")));
	EXPECT_TRUE(s.m_screen.event(key("J")));
	EXPECT_EQ(listed(), "a.exr b.exr c.exr ");
	EXPECT_TRUE(s.m_screen.event(key(" "))); // c again: out
	EXPECT_EQ(listed(), "a.exr b.exr ");

	// Enter on a recent item opens it alone; Open (n) the list.
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Return));
	ASSERT_EQ(s.m_calls.size(), 1u);
	EXPECT_EQ(s.m_calls[0], "open " + (s.m_dir / "c.exr").string());
	while(s.m_screen.selected() != s.m_screen.openPastedRow())
	{
		(void)s.m_screen.event(ftxui::Event::ArrowUp);
	}
	EXPECT_TRUE(s.m_screen.event(ftxui::Event::Return));
	ASSERT_EQ(s.m_calls.size(), 2u);
	EXPECT_EQ(s.m_calls[1],
	          "open " + (s.m_dir / "a.exr").string() + " " +
	              (s.m_dir / "b.exr").string());
}

TEST(StartScreen, ClickingRecentAddsItADoubleClickOpensIt)
{
	Start s;
	touch(s.m_dir / "a.exr");
	s.m_recent.add({(s.m_dir / "a.exr").string()}, 1);
	s.m_screen.show();
	auto rowOf = [&](std::string_view what)
	{
		ui::setScreenSize(ftxui::Dimensions{100, 30});
		ftxui::Element el = s.m_screen.render(s.m_dir);
		ftxui::Screen screen(100, 30);
		ftxui::Render(screen, el);
		ui::setScreenSize(std::nullopt);
		int last = -1; // the recent row is below the pasted one
		for(int y = 0; y < 30; ++y)
		{
			std::string line;
			for(int x = 0; x < 100; ++x)
			{
				line += screen.PixelAt(x, y).character;
			}
			if(line.find(what) != std::string::npos)
			{
				last = y;
			}
		}
		return last;
	};
	auto click = [&](int y)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = ftxui::Mouse::Pressed;
		m.x = 20;
		m.y = y;
		EXPECT_TRUE(s.m_screen.event(ftxui::Event::Mouse("", m)));
	};
	click(rowOf("a.exr"));
	ASSERT_EQ(s.m_screen.pasted().size(), 1u); // in the list
	EXPECT_TRUE(s.m_calls.empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(450));
	click(rowOf("a.exr")); // a while later: out again
	EXPECT_TRUE(s.m_screen.pasted().empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(450));
	const int y = rowOf("a.exr");
	click(y);
	click(rowOf("a.exr")); // at once: a double click opens it
	ASSERT_EQ(s.m_calls.size(), 1u);
	EXPECT_EQ(s.m_calls[0], "open " + (s.m_dir / "a.exr").string());
	EXPECT_TRUE(s.m_screen.pasted().empty()); // the first click undone
}
