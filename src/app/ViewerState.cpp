#include "app/ViewerState.h"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace fs = std::filesystem;

namespace rega
{

bool Source::filled() const
{
	return m_fillGaps && m_entry.m_kind == Entry::Kind::SEQUENCE &&
	       !m_entry.m_frameNumbers.empty() &&
	       m_entry.m_frameNumbers.size() == m_entry.m_frames.size();
}

fs::path Source::frame(int i) const
{
	if(m_entry.m_kind != Entry::Kind::SEQUENCE)
	{
		return m_entry.m_path;
	}
	if(filled())
	{
		return m_entry.m_frames[nearestFile(frameNumber(i))];
	}
	return m_entry.m_frames[std::clamp(
	    i, 0, static_cast<int>(m_entry.m_frames.size()) - 1)];
}

fs::path Source::diffFrame(int i) const
{
	return m_diffA ? Source{*m_diffA, m_fillGaps}.frame(i) : frame(i);
}

int Source::frameCount() const
{
	if(filled())
	{
		return m_entry.m_frameNumbers.back() - m_entry.m_frameNumbers.front() +
		       1;
	}
	return m_entry.m_kind == Entry::Kind::SEQUENCE
	           ? static_cast<int>(m_entry.m_frames.size())
	           : 1;
}

int Source::frameNumber(int i) const
{
	const auto& nums = m_entry.m_frameNumbers;
	if(m_entry.m_kind != Entry::Kind::SEQUENCE || nums.empty())
	{
		return 0;
	}
	i = std::clamp(i, 0, frameCount() - 1);
	return filled() ? nums.front() + i : nums[i];
}

std::string Source::frameLabel(int i) const
{
	if(m_entry.m_kind != Entry::Kind::SEQUENCE)
	{
		return "";
	}
	return std::to_string(frameNumber(i));
}

bool Source::held(int i) const
{
	return filled() &&
	       !std::ranges::binary_search(m_entry.m_frameNumbers, frameNumber(i));
}

int Source::indexForFrameNumber(int number) const
{
	if(filled())
	{
		return std::clamp(number - m_entry.m_frameNumbers.front(),
		                  0,
		                  frameCount() - 1);
	}
	return nearestFile(number);
}

int Source::nearestFile(int number) const
{
	const auto& nums = m_entry.m_frameNumbers;
	if(m_entry.m_kind != Entry::Kind::SEQUENCE || nums.empty())
	{
		return 0;
	}
	const auto it = std::ranges::lower_bound(nums, number);
	if(it == nums.begin())
	{
		return 0;
	}
	if(it == nums.end())
	{
		return static_cast<int>(nums.size()) - 1;
	}
	const auto before = std::prev(it);
	const auto nearest = *it - number < number - *before ? it : before;
	return static_cast<int>(nearest - nums.begin());
}

std::string sourceKey(const Source& s)
{
	std::error_code ec;
	const fs::path dir = fs::absolute(s.m_entry.m_path, ec).parent_path();
	const std::string key =
	    (dir / s.m_entry.m_name).lexically_normal().string();
	return s.m_diffA ? key + " - " + sourceKey(Source{*s.m_diffA}) : key;
}

std::vector<OpenItem> openItems(const std::vector<OpenArg>& args,
                                std::vector<std::string>& missing)
{
	std::vector<OpenItem> items;
	// The one entry `path` (labels off) opens, if any.
	auto single = [&](const std::string& arg)
	    -> std::optional<std::pair<Entry, AnnotationSet>>
	{
		auto [path, labels] = splitLabels(arg);
		auto found = entriesForArgs({path}, missing);
		if(found.empty())
		{
			return std::nullopt;
		}
		return std::pair{std::move(found.front()), std::move(labels)};
	};
	for(size_t i = 0; i < args.size();)
	{
		if(args[i].m_diffWith)
		{
			auto a = single(args[i].m_path);
			auto b = single(*args[i].m_diffWith);
			if(a && b)
			{
				items.push_back({std::move(a->first),
				                 std::move(a->second),
				                 std::move(b->first),
				                 std::move(b->second)});
			}
			else if(a || b)
			{
				auto& one = a ? *a : *b;
				items.push_back({std::move(one.first), std::move(one.second)});
			}
			++i;
			continue;
		}
		// A run of plain paths: their files may make up sequences together.
		std::vector<std::string> run;
		std::vector<std::pair<std::string, AnnotationSet>> labelled;
		for(; i < args.size() && !args[i].m_diffWith; ++i)
		{
			auto [path, labels] = splitLabels(args[i].m_path);
			run.push_back(path);
			if(!labels.empty())
			{
				labelled.emplace_back(path, std::move(labels));
			}
		}
		std::vector<OpenItem> found;
		for(auto& e : entriesForArgs(run, missing))
		{
			found.push_back({std::move(e)});
		}
		for(const auto& [path, labels] : labelled)
		{
			std::vector<std::string> ignored;
			for(const auto& mine : entriesForArgs({path}, ignored))
			{
				for(auto& item : found)
				{
					const auto& fr = item.m_a.m_frames;
					if(item.m_a.m_path == mine.m_path ||
					   std::ranges::find(fr, mine.m_path) != fr.end())
					{
						for(int s = 0; s < SLOT_COUNT; ++s)
						{
							auto& to = item.m_labelsA.m_slots[s];
							to.insert(to.end(),
							          labels.m_slots[s].begin(),
							          labels.m_slots[s].end());
						}
						break;
					}
				}
			}
		}
		std::ranges::move(found, std::back_inserter(items));
	}
	return items;
}

std::vector<OpenItem> pairItems(std::vector<Entry> marked)
{
	std::vector<OpenItem> items;
	for(size_t i = 0; i < marked.size(); i += 2)
	{
		items.push_back({std::move(marked[i])});
		if(i + 1 < marked.size())
		{
			items.back().m_b = std::move(marked[i + 1]);
		}
	}
	return items;
}

std::vector<Source> sourcesFor(std::vector<OpenItem> items, bool fillGaps)
{
	const bool pairs = std::ranges::any_of(items,
	                                       [](const OpenItem& i)
	                                       { return i.m_b.has_value(); });
	std::vector<Source> out;
	int group = 0;
	for(auto& item : items)
	{
		const int g = pairs ? group++ : -1;
		if(!item.m_b)
		{
			out.push_back(
			    {std::move(item.m_a), fillGaps, std::move(item.m_labelsA)});
			out.back().m_group = g;
			continue;
		}
		Source diff{*item.m_b, fillGaps};
		diff.m_diffA = item.m_a;
		diff.m_labels.lines(Slot::TC).push_back("diff");
		diff.m_group = g;
		out.push_back(
		    {std::move(item.m_a), fillGaps, std::move(item.m_labelsA)});
		out.back().m_group = g;
		out.push_back(
		    {std::move(*item.m_b), fillGaps, std::move(item.m_labelsB)});
		out.back().m_group = g;
		out.push_back(std::move(diff));
	}
	return out;
}

fs::path ViewerState::currentFramePath() const
{
	return m_sources[m_current].frame(m_frame);
}

int ViewerState::frameCount() const
{
	int n = 1;
	for(const auto& s : m_sources)
	{
		n = std::max(n, s.frameCount());
	}
	return n;
}

FrameRange ViewerState::playRange() const
{
	const int last = frameCount() - 1;
	FrameRange r{std::clamp(m_in.value_or(0), 0, last),
	             std::clamp(m_out.value_or(last), 0, last)};
	return r.m_first <= r.m_last ? r : FrameRange{0, last};
}

const Source* ViewerState::numberedSource() const
{
	if(m_current >= 0 && m_current < static_cast<int>(m_sources.size()) &&
	   m_sources[m_current].m_entry.m_kind == Entry::Kind::SEQUENCE)
	{
		return &m_sources[m_current];
	}
	for(const auto& s : m_sources)
	{
		if(s.m_entry.m_kind == Entry::Kind::SEQUENCE)
		{
			return &s;
		}
	}
	return nullptr;
}

const AnnotationSet* ViewerState::sourceAnnotations(const Annotations& ann,
                                                    int i) const
{
	if(i < 0 || i >= static_cast<int>(m_sources.size()))
	{
		return nullptr;
	}
	return ann.findSource(sourceKey(m_sources[i]));
}

std::vector<int> ViewerState::sheetSources() const
{
	std::vector<int> out;
	const int g = m_sources.empty() ? -1 : m_sources[m_current].m_group;
	for(int i = 0; i < static_cast<int>(m_sources.size()); ++i)
	{
		if(g < 0 || m_sources[i].m_group == g)
		{
			out.push_back(i);
		}
	}
	return out;
}

bool ViewerState::grouped() const
{
	return !m_sources.empty() && m_sources.front().m_group >= 0;
}

int ViewerState::groupCount() const
{
	return m_sources.empty() ? 0 : m_sources.back().m_group + 1;
}

int ViewerState::groupIndex(int i) const
{
	return m_sources[i].m_group;
}

int ViewerState::stepGroup(int delta) const
{
	const int n = groupCount();
	if(n == 0)
	{
		return m_current;
	}
	const int g = ((m_sources[m_current].m_group + delta) % n + n) % n;
	for(int i = 0; i < static_cast<int>(m_sources.size()); ++i)
	{
		if(m_sources[i].m_group == g)
		{
			return i;
		}
	}
	return m_current;
}

namespace
{

// Dragged columns keep room for a short name, and leave the image some.
constexpr int MIN_LEFT = 16, MIN_RIGHT = 20, MIN_IMAGE = 10;

// A column `w` cells wide terminal asks for: `frac` of it once dragged (0:
// not), else `fallback`.
int askedWidth(double frac, int w, int fallback, int least)
{
	return frac > 0 ? std::max(least, static_cast<int>(std::lround(frac * w)))
	                : fallback;
}

int askedLeft(double frac, int w)
{
	return askedWidth(frac, w, std::max(26, w * 20 / 100), MIN_LEFT);
}

int askedRight(double frac, int w)
{
	return askedWidth(frac, w, std::max(32, w * 30 / 100), MIN_RIGHT);
}

} // namespace

int ViewerState::leftPanelWidth() const
{
	const int w = ftxui::Terminal::Size().dimx;
	const int asked = askedLeft(m_leftFrac, w);
	if(m_leftFrac <= 0)
	{
		return asked;
	}
	// Against the right column as asked (not as fitted: that is fitted to
	// this one).
	return std::min(asked,
	                std::max(MIN_LEFT,
	                         w - askedRight(m_rightFrac, w) - 2 - MIN_IMAGE));
}

int ViewerState::sidePanelWidth() const
{
	const int w = ftxui::Terminal::Size().dimx;
	const int asked = askedRight(m_rightFrac, w);
	if(m_rightFrac <= 0)
	{
		return asked;
	}
	return std::min(asked,
	                std::max(MIN_RIGHT, w - leftPanelWidth() - 2 - MIN_IMAGE));
}

} // namespace rega
