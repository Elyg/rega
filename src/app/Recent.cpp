#include "app/Recent.h"

#include "util/Paths.h"

#include <charconv>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace rega
{

namespace
{

fs::path fromUtf8(std::string_view s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

std::string toUtf8(const fs::path& p)
{
	const std::u8string s = p.u8string();
	return {s.begin(), s.end()};
}

} // namespace

Recent::Recent(fs::path file) : m_file(std::move(file))
{
}

fs::path Recent::defaultPath()
{
	const fs::path dir = paths::stateDir();
	return dir.empty() ? dir : dir / "recent";
}

void Recent::load()
{
	m_items.clear();
	if(m_file.empty())
	{
		return;
	}
	std::ifstream in(m_file, std::ios::binary);
	std::string line;
	while(std::getline(in, line) && m_items.size() < MAX_ITEMS)
	{
		if(!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		RecentItem item;
		std::string_view rest(line);
		const size_t tab = rest.find('\t');
		if(tab == std::string_view::npos)
		{
			continue;
		}
		const std::string_view when = rest.substr(0, tab);
		const auto [p, ec] = std::from_chars(when.data(),
		                                     when.data() + when.size(),
		                                     item.m_when);
		if(ec != std::errc() || p != when.data() + when.size())
		{
			continue;
		}
		rest.remove_prefix(tab + 1);
		while(!rest.empty())
		{
			const size_t t = rest.find('\t');
			const std::string_view one = rest.substr(0, t);
			if(!one.empty())
			{
				item.m_paths.emplace_back(one);
			}
			rest.remove_prefix(t == std::string_view::npos ? rest.size()
			                                               : t + 1);
		}
		if(!item.m_paths.empty())
		{
			m_items.push_back(std::move(item));
		}
	}
}

bool Recent::save() const
{
	if(m_file.empty())
	{
		return false;
	}
	std::error_code ec;
	fs::create_directories(m_file.parent_path(), ec);
	const fs::path tmp = fs::path(m_file).concat(".tmp");
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if(!out)
		{
			return false;
		}
		for(const RecentItem& item : m_items)
		{
			out << item.m_when;
			for(const std::string& p : item.m_paths)
			{
				out << '\t' << p;
			}
			out << '\n';
		}
		if(!out)
		{
			return false;
		}
	}
	fs::rename(tmp, m_file, ec);
	return !ec;
}

void Recent::add(std::vector<std::string> paths, std::int64_t when)
{
	if(m_file.empty())
	{
		return;
	}
	std::erase_if(paths,
	              [](const std::string& p)
	              {
		              return p.empty() ||
		                     p.find_first_of("\t\r\n") != std::string::npos;
	              });
	if(paths.empty())
	{
		return;
	}
	std::erase_if(m_items,
	              [&](const RecentItem& i) { return i.m_paths == paths; });
	m_items.insert(m_items.begin(), RecentItem{std::move(paths), when});
	if(m_items.size() > MAX_ITEMS)
	{
		m_items.resize(MAX_ITEMS);
	}
	save();
}

void Recent::remove(size_t i)
{
	if(i < m_items.size())
	{
		m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(i));
		save();
	}
}

bool recentPathExists(const std::string& path)
{
	const fs::path p = fromUtf8(path);
	const std::string name = toUtf8(p.filename());
	std::error_code ec;
	if(name.find_first_of("#*") != std::string::npos)
	{
		return fs::is_directory(p.parent_path(), ec);
	}
	return fs::exists(p, ec);
}

std::string tildePath(const fs::path& dir, const fs::path& home)
{
	if(!home.empty())
	{
		const fs::path h = home.lexically_normal();
		const fs::path rel = dir.lexically_normal().lexically_relative(h);
		if(!rel.empty() && *rel.begin() != "..")
		{
			return rel == "."
			           ? std::string("~")
			           : "~" + std::string(1, fs::path::preferred_separator) +
			                 toUtf8(rel);
		}
	}
	return toUtf8(dir);
}

RecentLabel recentLabel(const RecentItem& item, const fs::path& home)
{
	RecentLabel l;
	if(item.m_paths.empty())
	{
		return l;
	}
	fs::path first = fromUtf8(item.m_paths.front());
	std::error_code ec;
	const bool dir = fs::is_directory(first, ec);
	if(!first.has_filename()) // "/renders/" names the folder
	{
		first = first.parent_path();
	}
	l.m_name = toUtf8(first.filename());
	if(l.m_name.empty())
	{
		l.m_name = toUtf8(first); // a root
	}
	if(dir && !l.m_name.ends_with('/') && !l.m_name.ends_with('\\'))
	{
		l.m_name += '/';
	}
	if(item.m_paths.size() > 1)
	{
		l.m_name += " +" + std::to_string(item.m_paths.size() - 1) + " more";
	}
	if(first.has_parent_path() && first.parent_path() != first)
	{
		l.m_where = tildePath(first.parent_path(), home);
	}
	return l;
}

std::string agoText(std::int64_t s)
{
	constexpr std::int64_t MIN = 60, HOUR = 60 * MIN, DAY = 24 * HOUR,
	                       WEEK = 7 * DAY;
	auto n = [](std::int64_t v, std::string_view one, std::string_view many)
	{
		return std::to_string(v) + " " + std::string(v == 1 ? one : many) +
		       " ago";
	};
	if(s < MIN)
	{
		return "just now";
	}
	if(s < HOUR)
	{
		return std::to_string(s / MIN) + " min ago";
	}
	if(s < DAY)
	{
		return std::to_string(s / HOUR) + " h ago";
	}
	if(s < WEEK)
	{
		return n(s / DAY, "day", "days");
	}
	return n(s / WEEK, "week", "weeks");
}

} // namespace rega
