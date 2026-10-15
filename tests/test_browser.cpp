#include "TestContext.h"
#include "app/Browser.h"

#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <utility>

using namespace rv;
using namespace rvtest;
using ftxui::Event;

namespace
{

// A directory with a sub-directory, a 3-frame sequence, an image, a text
// file and a hidden file.
struct BrowserTest : ::testing::Test
{
	Context m_c;
	fs::path m_dir = freshDir("browser");
	std::optional<std::vector<Entry>> m_opened;
	bool m_openedTiled = false;
	std::optional<std::vector<Entry>> m_diffed;
	Browser m_b{m_c.m_ctx,
	            "",
	            [this](std::vector<Entry> entries, bool tile)
	            {
		            m_opened = std::move(entries);
		            m_openedTiled = tile;
	            },
	            [this](std::vector<Entry> marked)
	            { m_diffed = std::move(marked); }};

	BrowserTest()
	{
		fs::create_directories(m_dir / "sub");
		touch(m_dir / "sub" / "inner.png");
		for(const char* f : {"a.0001.exr", "a.0002.exr", "a.0003.exr"})
		{
			touch(m_dir / f);
		}
		touch(m_dir / "b.png");
		touch(m_dir / "notes.txt", "hello\n");
		touch(m_dir / ".hidden");
		m_b.openDirectory(m_dir);
	}

	void press(const Event& e)
	{
		EXPECT_TRUE(m_b.event(e));
	}
	void type(const std::string& s)
	{
		for(char c : s)
		{
			press(Event::Character(c));
		}
	}
	std::string selected() const
	{
		const Entry* e = m_b.selectedEntry();
		return e ? e->m_name : "";
	}
	// Move the cursor onto the entry named `name`.
	void select(const std::string& name)
	{
		press(key("g"));
		press(key("g"));
		for(size_t i = 0; i < m_b.entries().size() && selected() != name; ++i)
		{
			press(key("j"));
		}
		ASSERT_EQ(selected(), name);
	}
	std::string sequenceName() const
	{
		for(const auto& e : m_b.entries())
		{
			if(e.m_kind == Entry::Kind::SEQUENCE)
			{
				return e.m_name;
			}
		}
		return "";
	}
	// Draw the browser; the screen cell where `s` first shows at or right
	// of column `from` ({-1, -1}: nowhere).
	std::pair<int, int> find(const std::string& s, int from = 0)
	{
		ftxui::Screen screen(80, 24);
		ftxui::Render(screen, m_b.render());
		for(int y = 0; y < screen.dimy(); ++y)
		{
			std::string row;
			std::vector<int> cols; // screen column of each byte of `row`
			for(int x = from; x < screen.dimx(); ++x)
			{
				const std::string& c = screen.PixelAt(x, y).character;
				row += c.empty() ? " " : c;
				cols.resize(row.size(), x);
			}
			if(const auto at = row.find(s); at != std::string::npos)
			{
				return {cols[at], y};
			}
		}
		return {-1, -1};
	}
	void click(std::pair<int, int> at)
	{
		ftxui::Mouse m;
		m.button = ftxui::Mouse::Left;
		m.motion = ftxui::Mouse::Pressed;
		m.x = at.first;
		m.y = at.second;
		press(Event::Mouse("", m));
	}
};

} // namespace

TEST_F(BrowserTest, ListsDirsFirstThenFilesWithoutHiddenOnes)
{
	const auto& es = m_b.entries();
	ASSERT_EQ(es.size(), 4u);
	EXPECT_EQ(es[0].m_name, "sub");
	EXPECT_EQ(es[1].m_kind, Entry::Kind::SEQUENCE);
	EXPECT_EQ(es[2].m_name, "b.png");
	EXPECT_EQ(es[3].m_name, "notes.txt");
	press(key("."));
	EXPECT_EQ(m_b.entries().size(), 5u); // .hidden shown
}

TEST_F(BrowserTest, MovesWithJkAndJumpsWithGgAndG)
{
	EXPECT_EQ(selected(), "sub");
	press(key("j"));
	press(key("j"));
	EXPECT_EQ(selected(), "b.png");
	press(key("G"));
	EXPECT_EQ(selected(), "notes.txt");
	press(key("j")); // clamped
	EXPECT_EQ(selected(), "notes.txt");
	press(key("g"));
	EXPECT_EQ(selected(), "notes.txt"); // waits for the second g
	press(key("g"));
	EXPECT_EQ(selected(), "sub");
	press(key("k"));
	EXPECT_EQ(selected(), "sub");
}

TEST_F(BrowserTest, EntersADirectoryAndComesBackOntoIt)
{
	press(key("l"));
	EXPECT_EQ(m_b.cwd(), m_dir / "sub");
	EXPECT_EQ(selected(), "inner.png");
	press(key("h"));
	EXPECT_EQ(m_b.cwd(), m_dir);
	EXPECT_EQ(selected(), "sub");
}

TEST_F(BrowserTest, FuzzyFilterNarrowsTheListUntilCleared)
{
	press(key("/"));
	EXPECT_TRUE(m_b.typing());
	type("nts");
	EXPECT_EQ(m_b.filter(), "nts");
	ASSERT_EQ(m_b.visibleIndices().size(), 1u);
	EXPECT_EQ(selected(), "notes.txt");
	press(Event::Return); // keep it, stop typing
	EXPECT_FALSE(m_b.typing());
	EXPECT_EQ(m_b.filter(), "nts");
	press(Event::Escape); // clears it
	EXPECT_EQ(m_b.filter(), "");
	EXPECT_EQ(m_b.visibleIndices().size(), 4u);
	EXPECT_FALSE(m_b.event(key("Q"))); // not a browser key (App quits)
}

TEST_F(BrowserTest, MarksOnlyImagesAndKeepsThemAcrossDirectories)
{
	press(key(" ")); // on "sub"
	EXPECT_EQ(m_c.m_ctx.m_message, "only images can be marked");
	EXPECT_TRUE(m_b.marks().empty());
	select("b.png");
	press(key(" "));
	select(sequenceName());
	press(key(" "));
	ASSERT_EQ(m_b.marks().size(), 2u);
	select("sub");
	press(key("l"));
	press(key(" ")); // inner.png
	press(key("h"));
	EXPECT_EQ(m_b.marks().size(), 3u);
	select("b.png");
	press(key(" ")); // again: unmarked
	ASSERT_EQ(m_b.marks().size(), 2u);
	EXPECT_EQ(m_b.marks()[0].m_kind, Entry::Kind::SEQUENCE); // marking order
	EXPECT_EQ(m_b.marks()[1].m_name, "inner.png");
	press(Event::Escape); // no filter: Esc clears the marks
	EXPECT_TRUE(m_b.marks().empty());
}

TEST_F(BrowserTest, AMarksEveryImageListedAndAgainNone)
{
	press(key("a"));
	EXPECT_EQ(m_b.marks().size(), 2u); // the sequence and b.png only
	press(key("a"));
	EXPECT_TRUE(m_b.marks().empty());
	// Filtered: what passes it.
	type("/b.p");
	press(Event::Return);
	press(key("a"));
	ASSERT_EQ(m_b.marks().size(), 1u);
	EXPECT_EQ(m_b.marks()[0].m_name, "b.png");
}

TEST_F(BrowserTest, ClickSelectsAndASecondClickOpens)
{
	const auto at = find("b.png", 13); // the current column, past the parent
	ASSERT_GE(at.first, 0);
	click(at);
	EXPECT_EQ(selected(), "b.png");
	EXPECT_FALSE(m_opened);
	click(at); // straight after: a double click
	ASSERT_TRUE(m_opened);
	ASSERT_EQ(m_opened->size(), 1u);
	EXPECT_EQ((*m_opened)[0].m_name, "b.png");
}

TEST_F(BrowserTest, ClickingADirInTheParentColumnGoesThere)
{
	fs::create_directories(m_dir / "other");
	select("sub");
	press(key("l")); // in sub/: the parent column lists sub and other
	const auto at = find("other");
	ASSERT_GE(at.first, 0);
	click(at);
	EXPECT_EQ(m_b.cwd(), m_dir / "other");
}

TEST_F(BrowserTest, LongNamesAreCutSoTheColumnStaysPut)
{
	// The column used to scroll sideways to show a long selected name.
	touch(m_dir / "test_BasisCurve_ribbon_varying_invalid_normals_"
	              "Interactive.png");
	press(key("R"));
	const auto before = find("b.png", 13);
	select("test_BasisCurve_ribbon_varying_invalid_normals_Interactive.png");
	EXPECT_EQ(find("b.png", 13), before);
	EXPECT_GE(find("…", 13).first, 0); // the long name, cut
}

TEST_F(BrowserTest, MarkTakesImagesOnceInOrder)
{
	// What rvtui was started with comes in marked.
	const Entry* seq = nullptr;
	const Entry* png = nullptr;
	const Entry* dir = nullptr;
	for(const auto& e : m_b.entries())
	{
		seq = e.m_kind == Entry::Kind::SEQUENCE ? &e : seq;
		png = e.m_name == "b.png" ? &e : png;
		dir = e.m_kind == Entry::Kind::DIR ? &e : dir;
	}
	ASSERT_TRUE(seq && png && dir);
	m_b.mark({*png, *dir, *seq, *png});
	ASSERT_EQ(m_b.marks().size(), 2u); // no dir, b.png once
	EXPECT_EQ(m_b.marks()[0].m_name, "b.png");
	EXPECT_EQ(m_b.marks()[1].m_kind, Entry::Kind::SEQUENCE);
	m_b.mark({*png}); // already marked: stays, and stays first
	EXPECT_EQ(m_b.marks().size(), 2u);
	EXPECT_EQ(m_b.marks()[0].m_name, "b.png");
}

TEST_F(BrowserTest, OpensTheMarkedImagesElseTheSelectedOne)
{
	select("b.png");
	press(key("l"));
	ASSERT_TRUE(m_opened);
	ASSERT_EQ(m_opened->size(), 1u);
	EXPECT_EQ(m_opened->front().m_name, "b.png");
	EXPECT_FALSE(m_openedTiled);

	select(sequenceName());
	press(key(" "));
	select("b.png");
	press(key(" "));
	press(key("t")); // all marked, as a contact sheet
	ASSERT_EQ(m_opened->size(), 2u);
	EXPECT_EQ(m_opened->front().m_kind, Entry::Kind::SEQUENCE);
	EXPECT_TRUE(m_openedTiled);
}

TEST_F(BrowserTest, TextFilesFocusThePreviewInsteadOfOpening)
{
	select("notes.txt");
	press(key("l"));
	EXPECT_FALSE(m_opened);
	press(key("j")); // scrolls the preview, not the list
	EXPECT_EQ(selected(), "notes.txt");
	press(key("h")); // back out of the preview
	press(key("k"));
	EXPECT_EQ(selected(), "b.png");
}

TEST_F(BrowserTest, ExpandsASequenceIntoFramesAndFoldsItBack)
{
	const std::string seq = sequenceName();
	select(seq);
	press(key("e"));
	ASSERT_EQ(m_b.entries().size(), 6u);
	EXPECT_EQ(selected(), "a.0001.exr");
	EXPECT_FALSE(m_b.selectedEntry()->m_expandedFrom.empty());
	press(key("j"));
	press(key("e")); // on a frame: fold back, onto the sequence
	EXPECT_EQ(m_b.entries().size(), 4u);
	EXPECT_EQ(selected(), seq);
	select("b.png");
	press(key("e"));
	EXPECT_EQ(m_c.m_ctx.m_message, "not a sequence");
}

TEST_F(BrowserTest, RefreshPicksUpNewFilesAndKeepsTheSelection)
{
	select("b.png");
	EXPECT_FALSE(m_b.refresh());
	touch(m_dir / "0first.png"); // sorts before everything else
	EXPECT_TRUE(m_b.refresh());
	EXPECT_EQ(m_b.entries().size(), 5u);
	EXPECT_EQ(selected(), "b.png");
	EXPECT_FALSE(m_b.refresh());
	const auto dirs = m_b.shownDirs();
	ASSERT_FALSE(dirs.empty());
	EXPECT_EQ(dirs.front(), m_dir);
}

TEST_F(BrowserTest, QuitAndChooseWithoutAFile)
{
	press(key("o"));
	EXPECT_EQ(m_c.m_ctx.m_message, "no --chooser-file given");
	EXPECT_EQ(m_c.m_quits, 0);
	press(key("q"));
	EXPECT_EQ(m_c.m_quits, 1);
}

TEST(Browser, ChooserFileGetsTheMarkedPaths)
{
	Context c;
	const fs::path dir = freshDir("browser-chooser");
	touch(dir / "a.png");
	touch(dir / "b.png");
	const fs::path out = dir / "chosen";
	Browser b(c.m_ctx, out.string(), [](std::vector<Entry>, bool) {});
	b.openDirectory(dir);
	EXPECT_TRUE(b.event(key("j")));
	EXPECT_TRUE(b.event(key(" "))); // mark b.png
	EXPECT_TRUE(b.event(key("o")));
	EXPECT_EQ(c.m_quits, 1);
	std::ifstream in(out);
	std::string line;
	std::getline(in, line);
	EXPECT_EQ(line, (dir / "b.png").string());
}

TEST_F(BrowserTest, PreviewSaysWhenTheImageIsIncomplete)
{
	// 40 lines of ZIP are three chunks; one copy loses the end of the last.
	writeExr(m_dir / "whole.exr", {"R", "G", "B"}, 8, 40);
	const fs::path cut = writeExr(m_dir / "cut.exr", {"R", "G", "B"}, 8, 40);
	fs::resize_file(cut, fs::file_size(cut) - 10);
	m_b.openDirectory(m_dir);
	// Each draw asks for what is missing: the header, then the pixels.
	select("cut.exr");
	EXPECT_TRUE(waitFor([&] { return find("incomplete image").first >= 0; }));
	select("whole.exr");
	EXPECT_TRUE(waitFor(
	    [&]
	    {
		    return find("loading…").first < 0 &&
		           find("decoding…").first < 0 &&
		           m_c.m_svc.pendingJobs() == 0;
	    }));
	EXPECT_LT(find("incomplete image").first, 0);
}

TEST_F(BrowserTest, DDiffsTheMarkedOrOneMarkedWithTheOneHere)
{
	press(key("d")); // nothing marked: a hint, no diff
	EXPECT_FALSE(m_diffed);
	EXPECT_FALSE(m_c.m_ctx.m_message.empty());

	select("b.png");
	press(key(" "));
	select(sequenceName());
	press(key("d"));
	ASSERT_TRUE(m_diffed);
	ASSERT_EQ(m_diffed->size(), 2u);
	EXPECT_EQ((*m_diffed)[0].m_name, "b.png"); // marked first: A
	EXPECT_EQ((*m_diffed)[1].m_name, sequenceName());
}
