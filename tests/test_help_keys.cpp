#include "app/HelpKeys.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

TEST(HelpKeys, SectionIdsAreUniqueAndAnchored)
{
	const std::string md = rv::help::markdown();
	std::set<std::string> ids;
	for(const auto* sections : {&rv::help::browser(), &rv::help::viewer()})
	{
		for(const auto& s : *sections)
		{
			EXPECT_TRUE(ids.insert(s.m_id).second) << s.m_id;
			EXPECT_FALSE(s.m_rows.empty()) << s.m_id;
			EXPECT_FALSE(s.m_rows.front().m_key.empty())
			    << s.m_id << ": a continuation can't open a section";
			EXPECT_NE(md.find("<!-- ANCHOR: " + s.m_id + " -->"),
			          std::string::npos);
			EXPECT_NE(md.find("<!-- ANCHOR_END: " + s.m_id + " -->"),
			          std::string::npos);
		}
	}
}

TEST(HelpKeys, MarkdownEscapesPipesAndJoinsContinuations)
{
	const std::string md = rv::help::markdown();
	// "|A - B|" would end the cell; the continuation joins "n / p (N)".
	EXPECT_NE(md.find("| `n / p (N)` | next / previous image (marked files)"
	                  "<br>diff: next / previous pair (A, B, \\|A - B\\|) |"),
	          std::string::npos);
	// Sub-rows lose their indent.
	EXPECT_NE(md.find("| `+- wheel hjkl` |"), std::string::npos);
	EXPECT_EQ(md.find("|  |"), std::string::npos); // no empty key cells
}
