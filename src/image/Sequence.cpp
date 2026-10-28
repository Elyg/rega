#include "image/Sequence.h"

#include "image/Loader.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace rega
{

bool Entry::isImage() const
{
	return m_kind == Kind::SEQUENCE ||
	       (m_kind == Kind::FILE && isSupportedImage(m_path));
}

std::string Entry::rangeString() const
{
	std::string out;
	size_t i = 0;
	while(i < m_frameNumbers.size())
	{
		size_t j = i;
		while(j + 1 < m_frameNumbers.size() &&
		      m_frameNumbers[j + 1] == m_frameNumbers[j] + 1)
		{
			++j;
		}
		if(!out.empty())
		{
			out += ",";
		}
		out += std::to_string(m_frameNumbers[i]);
		if(j > i)
		{
			out += "-" + std::to_string(m_frameNumbers[j]);
		}
		i = j + 1;
	}
	return out;
}

bool splitFrame(const std::string& name,
                std::string& prefix,
                std::string& digits,
                std::string& suffix)
{
	auto dot = name.rfind('.');
	if(dot == std::string::npos || dot == 0)
	{
		return false;
	}
	suffix = name.substr(dot);
	size_t end = dot, begin = dot;
	while(begin > 0 &&
	      std::isdigit(static_cast<unsigned char>(name[begin - 1])))
	{
		--begin;
	}
	if(begin == end || begin == 0)
	{
		return false;
	}
	char sep = name[begin - 1];
	if(sep != '.' && sep != '_')
	{
		return false;
	}
	prefix = name.substr(0, begin);
	digits = name.substr(begin, end - begin);
	return true;
}

std::vector<Entry> groupFiles(const fs::path& dir,
                              const std::vector<std::string>& names)
{
	struct Group
	{
		std::vector<std::pair<int, std::string>> m_frames;
		std::vector<size_t> m_widths;
	};
	std::map<std::pair<std::string, std::string>, Group>
	    groups; // (prefix, suffix)
	std::vector<std::string> singles;

	for(const auto& n : names)
	{
		std::string prefix, digits, suffix;
		if(isSupportedImage(n) && splitFrame(n, prefix, digits, suffix) &&
		   digits.size() <= 9)
		{
			auto& g = groups[{prefix, suffix}];
			g.m_frames.emplace_back(std::stoi(digits), n);
			g.m_widths.push_back(digits.size());
		}
		else
		{
			singles.push_back(n);
		}
	}

	std::vector<Entry> out;
	for(auto& [key, g] : groups)
	{
		if(g.m_frames.size() < 2)
		{
			singles.push_back(g.m_frames.front().second);
			continue;
		}
		std::sort(g.m_frames.begin(), g.m_frames.end());
		Entry e;
		e.m_kind = Entry::Kind::SEQUENCE;
		size_t pad = *std::min_element(g.m_widths.begin(), g.m_widths.end());
		bool samePad = std::all_of(g.m_widths.begin(),
		                           g.m_widths.end(),
		                           [&](size_t w) { return w == pad; });
		e.m_name = key.first +
		           (samePad ? std::string(pad, '#') : std::string("@")) +
		           key.second;
		for(auto& [num, n] : g.m_frames)
		{
			e.m_frameNumbers.push_back(num);
			e.m_frames.push_back(dir / n);
		}
		e.m_path = e.m_frames.front();
		e.m_name += " [" + e.rangeString() + "]";
		out.push_back(std::move(e));
	}
	for(auto& n : singles)
	{
		Entry e;
		e.m_kind = Entry::Kind::FILE;
		e.m_name = n;
		e.m_path = dir / n;
		out.push_back(std::move(e));
	}
	std::sort(out.begin(),
	          out.end(),
	          [](const Entry& a, const Entry& b)
	          { return a.m_name < b.m_name; });
	return out;
}

std::vector<Entry> listDirectory(const fs::path& dir, bool showHidden)
{
	std::vector<Entry> dirs;
	std::vector<std::string> files;
	std::error_code ec;
	for(auto it = fs::directory_iterator(
	        dir, fs::directory_options::skip_permission_denied, ec);
	    !ec && it != fs::directory_iterator();
	    it.increment(ec))
	{
		std::string name = it->path().filename().string();
		if(!showHidden && !name.empty() && name[0] == '.')
		{
			continue;
		}
		std::error_code ec2;
		if(it->is_directory(ec2))
		{
			Entry e;
			e.m_kind = Entry::Kind::DIR;
			e.m_name = name;
			e.m_path = it->path();
			dirs.push_back(std::move(e));
		}
		else
		{
			files.push_back(name);
		}
	}
	std::sort(dirs.begin(),
	          dirs.end(),
	          [](const Entry& a, const Entry& b)
	          { return a.m_name < b.m_name; });
	auto grouped = groupFiles(dir, files);
	for(auto& e : grouped)
	{
		std::error_code ec3;
		if(e.m_kind == Entry::Kind::FILE)
		{
			e.m_size = fs::file_size(e.m_path, ec3);
		}
		else
		{
			for(auto& f : e.m_frames)
			{
				e.m_size += fs::file_size(f, ec3);
			}
		}
	}
	dirs.insert(dirs.end(),
	            std::make_move_iterator(grouped.begin()),
	            std::make_move_iterator(grouped.end()));
	return dirs;
}

namespace
{

// A filename glob as a regex: * ? [...] as fnmatch, `#` a digit (`####`
// exactly four), and a lone `#` any number of them (padding unknown).
std::regex globRegex(const std::string& glob)
{
	std::string re;
	for(size_t i = 0; i < glob.size(); ++i)
	{
		const char c = glob[i];
		if(c == '#')
		{
			size_t n = 1;
			while(i + n < glob.size() && glob[i + n] == '#')
			{
				++n;
			}
			re += n == 1 ? "[0-9]+" : "[0-9]{" + std::to_string(n) + "}";
			i += n - 1;
		}
		else if(c == '*')
		{
			re += ".*";
		}
		else if(c == '?')
		{
			re += '.';
		}
		else if(c == '[' && glob.find(']', i + 2) != std::string::npos)
		{
			// A bracket expression, `]` first allowed, `!` negates.
			const size_t close = glob.find(']', i + 2);
			std::string set = glob.substr(i + 1, close - i - 1);
			if(set.starts_with('!'))
			{
				set[0] = '^';
			}
			re += "[" + set + "]";
			i = close;
		}
		else if(c == '\\' && i + 1 < glob.size())
		{
			re += '\\';
			re += glob[++i];
		}
		else
		{
			if(std::string_view(".^$|()[]{}+\\").find(c) !=
			   std::string_view::npos)
			{
				re += '\\';
			}
			re += c;
		}
	}
	return std::regex(re);
}

} // namespace

std::vector<Entry> entriesForArgs(const std::vector<std::string>& args,
                                  std::vector<std::string>& missing)
{
	std::vector<fs::path> dirs; // in argument order
	std::map<fs::path, std::vector<std::string>> names;
	std::map<fs::path, size_t> order; // file → position it was given in
	auto add = [&](const fs::path& file)
	{
		std::error_code ec;
		fs::path abs = fs::absolute(file, ec).lexically_normal();
		order.emplace(abs, order.size());
		fs::path dir = abs.parent_path();
		if(!names.count(dir))
		{
			dirs.push_back(dir);
		}
		names[dir].push_back(abs.filename().string());
	};

	for(const auto& arg : args)
	{
		const fs::path p(arg);
		const std::string name = p.filename().string();
		std::error_code ec;
		if(fs::is_regular_file(p, ec))
		{
			add(p);
			continue;
		}
		if(name.find_first_of("*?[#") == std::string::npos)
		{
			missing.push_back(arg);
			continue;
		}
		// A glob the shell did not expand (quoted, or `#` frame padding).
		std::regex pattern;
		try
		{
			pattern = globRegex(name);
		}
		catch(const std::regex_error&)
		{
			missing.push_back(arg);
			continue;
		}
		const fs::path dir = p.parent_path().empty() ? "." : p.parent_path();
		bool any = false;
		for(fs::directory_iterator it(dir, ec), end; !ec && it != end;
		    it.increment(ec))
		{
			const std::string f = it->path().filename().string();
			if(it->is_regular_file(ec) && std::regex_match(f, pattern))
			{
				add(it->path());
				any = true;
			}
		}
		if(!any)
		{
			missing.push_back(arg);
		}
	}

	std::vector<Entry> out;
	for(const auto& dir : dirs)
	{
		auto& n = names[dir];
		std::sort(n.begin(), n.end());
		n.erase(std::unique(n.begin(), n.end()), n.end());
		for(auto& e : groupFiles(dir, n))
		{
			if(e.isImage())
			{
				out.push_back(std::move(e));
			}
		}
	}
	// Keep the order the files were given in (a sequence sits where its
	// earliest frame was).
	auto rank = [&](const Entry& e)
	{
		size_t r = order.size();
		for(const auto& f : e.m_kind == Entry::Kind::SEQUENCE
		                        ? e.m_frames
		                        : std::vector<fs::path>{e.m_path})
		{
			if(auto it = order.find(f); it != order.end())
			{
				r = std::min(r, it->second);
			}
		}
		return r;
	};
	std::stable_sort(out.begin(),
	                 out.end(),
	                 [&](const Entry& a, const Entry& b)
	                 { return rank(a) < rank(b); });
	return out;
}

Entry entryForPath(const fs::path& file)
{
	fs::path dir =
	    file.parent_path().empty() ? fs::path(".") : file.parent_path();
	std::string fname = file.filename().string();
	std::string prefix, digits, suffix;
	if(splitFrame(fname, prefix, digits, suffix))
	{
		for(auto& e : listDirectory(dir, true))
		{
			if(e.m_kind == Entry::Kind::SEQUENCE &&
			   std::find(e.m_frames.begin(), e.m_frames.end(), dir / fname) !=
			       e.m_frames.end())
			{
				return e;
			}
		}
	}
	Entry e;
	e.m_kind = Entry::Kind::FILE;
	e.m_name = fname;
	e.m_path = dir / fname;
	std::error_code ec;
	e.m_size = fs::file_size(e.m_path, ec);
	return e;
}

std::string humanSize(std::uintmax_t b)
{
	const char* units[] = {"B", "K", "M", "G", "T"};
	double v = static_cast<double>(b);
	int u = 0;
	while(v >= 1024 && u < 4)
	{
		v /= 1024;
		++u;
	}
	return u == 0 ? fmt::format("{:.0f}{}", v, units[u])
	              : fmt::format("{:.1f}{}", v, units[u]);
}

} // namespace rega
