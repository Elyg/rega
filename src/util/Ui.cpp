#include "util/Ui.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cctype>
#include <string>

using namespace ftxui;

namespace rega::ui
{

Element highlighted(std::string_view name,
                    std::span<const size_t> pos,
                    Decorator base)
{
	Elements runs;
	size_t i = 0, p = 0;
	while(i < name.size())
	{
		const bool hit = p < pos.size() && pos[p] == i;
		size_t j = i;
		while(j < name.size() && (p < pos.size() && pos[p] == j) == hit)
		{
			if(hit)
			{
				++p;
			}
			++j;
		}
		runs.push_back(text(std::string(name.substr(i, j - i))) |
		               (hit ? color(Color::Yellow) | bold | underlined : base));
		i = j;
	}
	return hbox(std::move(runs));
}

std::vector<size_t> findIgnoringCase(std::string_view hay,
                                     std::string_view needle)
{
	auto lower = [](char c)
	{ return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
	const auto it =
	    std::search(hay.begin(),
	                hay.end(),
	                needle.begin(),
	                needle.end(),
	                [&](char a, char b) { return lower(a) == lower(b); });
	std::vector<size_t> pos;
	if(needle.empty() || it == hay.end())
	{
		return pos;
	}
	const auto at = static_cast<size_t>(it - hay.begin());
	for(size_t i = 0; i < needle.size(); ++i)
	{
		pos.push_back(at + i);
	}
	return pos;
}

Element paneHints(std::string_view prefix,
                  std::initializer_list<PaneHint> hints)
{
	Elements parts{text(" ")};
	if(!prefix.empty())
	{
		parts.push_back(text(std::string(prefix) + " ") | dim);
	}
	for(const auto& [key, what] : hints)
	{
		parts.push_back(text("(") | dim);
		parts.push_back(text(std::string(key)) | bold);
		if(!what.empty())
		{
			parts.push_back(text(" " + std::string(what)) | dim);
		}
		parts.push_back(text(") ") | dim);
	}
	return hbox(std::move(parts));
}

Element statusLine(StatusLine line, int width)
{
	int used = 0;
	for(auto& el : line.m_status)
	{
		el->ComputeRequirement();
		used += el->requirement().min_x;
	}
	// A setup problem stays visible (yellow) until it is fixed.
	if(!line.m_warning.empty())
	{
		const std::string warn = " ⚠ " + std::string(line.m_warning) + " ";
		line.m_status.push_back(text(warn) | color(Color::Yellow));
		used += string_width(warn);
	}
	int room = width - used;
	// Same bar colour as the viewer HUD on top.
	Elements parts = std::move(line.m_status);
	const std::string tailText =
	    line.m_tail.empty() ? "" : " " + std::string(line.m_tail) + " ";
	const int tailW = string_width(tailText);
	if(!line.m_message.empty())
	{
		// The message replaces the hints; the tail only if it still fits.
		// Cut to the room left so the status on the left never shrinks.
		std::string msg;
		int msgW = 0;
		for(const auto& g :
		    Utf8ToGlyphs(" " + std::string(line.m_message) + " "))
		{
			if(g.empty())
			{
				continue; // the second cell of a wide glyph
			}
			const int w = std::max(1, string_width(g));
			if(msgW + w > std::max(0, room - 1))
			{
				msg += "…";
				++msgW;
				break;
			}
			msg += g;
			msgW += w;
		}
		const bool withTail = tailW > 0 && msgW + tailW <= room;
		// Clipboard results stand out: copied in yellow, failures in red.
		const Decorator tone =
		    line.m_message.starts_with("copied")        ? color(Color::Yellow)
		    : line.m_message.starts_with("copy failed") ? color(Color::Red)
		                                                : nothing;
		parts.push_back(text(msg) | bold | tone);
		parts.push_back(filler());
		if(withTail)
		{
			parts.push_back(text(tailText) | dim);
		}
		return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
	}
	parts.push_back(filler());
	const std::string hintText =
	    line.m_hints.empty() ? "" : std::string(line.m_hints) + " ";
	const bool withTail = tailW > 0 && tailW <= room;
	if(withTail)
	{
		room -= tailW;
	}
	if(!hintText.empty() && string_width(hintText) <= room)
	{
		parts.push_back(text(hintText) | dim);
	}
	if(withTail)
	{
		parts.push_back(text(tailText) | dim);
	}
	return hbox(std::move(parts)) | bgcolor(Color::GrayDark);
}

std::string paneTitle(std::string_view head, int width)
{
	std::string title(head);
	for(int i = string_width(title); i < width; ++i)
	{
		title += "─";
	}
	return title;
}

namespace
{

// The glyphs of `s` with their display width (the empty second cell FTXUI
// gives a wide glyph is dropped).
std::vector<std::pair<std::string, int>> sizedGlyphs(std::string_view s)
{
	std::vector<std::pair<std::string, int>> out;
	for(auto& g : Utf8ToGlyphs(std::string(s)))
	{
		if(!g.empty())
		{
			const int w = std::max(1, string_width(g));
			out.emplace_back(std::move(g), w);
		}
	}
	return out;
}

int totalWidth(const std::vector<std::pair<std::string, int>>& glyphs)
{
	int w = 0;
	for(const auto& g : glyphs)
	{
		w += g.second;
	}
	return w;
}

} // namespace

std::string ellipsizeEnd(std::string_view s, int width)
{
	const auto glyphs = sizedGlyphs(s);
	if(totalWidth(glyphs) <= width)
	{
		return std::string(s);
	}
	if(width <= 0)
	{
		return "";
	}
	std::string out;
	int used = 0;
	for(const auto& [g, w] : glyphs)
	{
		if(used + w > width - 1)
		{
			break;
		}
		out += g;
		used += w;
	}
	return out + "…";
}

std::string ellipsizeStart(std::string_view s, int width)
{
	const auto glyphs = sizedGlyphs(s);
	if(totalWidth(glyphs) <= width)
	{
		return std::string(s);
	}
	if(width <= 0)
	{
		return "";
	}
	std::string tail;
	int used = 0;
	for(auto it = glyphs.rbegin(); it != glyphs.rend(); ++it)
	{
		if(used + it->second > width - 1)
		{
			break;
		}
		tail.insert(0, it->first);
		used += it->second;
	}
	return "…" + tail;
}

std::string ellipsizeMiddle(std::string_view s, int width)
{
	return ellipsizeMiddle(s, width, {}).first;
}

std::pair<std::string, std::vector<size_t>>
ellipsizeMiddle(std::string_view s, int width, std::span<const size_t> pos)
{
	const auto glyphs = sizedGlyphs(s);
	if(totalWidth(glyphs) <= width)
	{
		return {std::string(s), {pos.begin(), pos.end()}};
	}
	if(width <= 0)
	{
		return {};
	}
	// The head gets the odd cell: "shaders.cam…anLeft".
	const int room = width - 1;
	const int headRoom = (room + 1) / 2, tailRoom = room / 2;
	size_t headBytes = 0;
	int used = 0;
	for(const auto& [g, w] : glyphs)
	{
		if(used + w > headRoom)
		{
			break;
		}
		headBytes += g.size();
		used += w;
	}
	size_t tailBytes = 0;
	used = 0;
	for(auto it = glyphs.rbegin(); it != glyphs.rend(); ++it)
	{
		if(used + it->second > tailRoom)
		{
			break;
		}
		tailBytes += it->first.size();
		used += it->second;
	}
	constexpr std::string_view DOTS = "…";
	const size_t tailStart = s.size() - tailBytes;
	std::vector<size_t> moved;
	for(const size_t p : pos)
	{
		if(p < headBytes)
		{
			moved.push_back(p);
		}
		else if(p >= tailStart)
		{
			moved.push_back(p - tailStart + headBytes + DOTS.size());
		}
	}
	return {std::string(s.substr(0, headBytes)) + std::string(DOTS) +
	            std::string(s.substr(tailStart)),
	        std::move(moved)};
}

std::vector<std::string> wrapWidth(std::string_view s, int width)
{
	width = std::max(1, width);
	std::vector<std::string> lines(1);
	int used = 0;
	for(const auto& [g, w] : sizedGlyphs(s))
	{
		if(used + w > width && used > 0)
		{
			lines.emplace_back();
			used = 0;
		}
		lines.back() += g;
		used += w;
	}
	return lines;
}

} // namespace rega::ui
