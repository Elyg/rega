#include "app/Annotations.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rv
{

namespace
{

std::optional<Slot> slotFromKey(const std::string& k)
{
	for(int s = 0; s < SLOT_COUNT; ++s)
	{
		if(k == slotKey(static_cast<Slot>(s)))
		{
			return static_cast<Slot>(s);
		}
	}
	return std::nullopt;
}

std::string trim(const std::string& s)
{
	const auto b = s.find_first_not_of(" \t\r");
	if(b == std::string::npos)
	{
		return "";
	}
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

void writeSet(std::ostringstream& out, const AnnotationSet& set)
{
	for(int s = 0; s < SLOT_COUNT; ++s)
	{
		for(const auto& line : set.m_slots[s])
		{
			out << slotKey(static_cast<Slot>(s)) << " = " << line << '\n';
		}
	}
}

} // namespace

bool AnnotationSet::empty() const
{
	return std::all_of(m_slots.begin(),
	                   m_slots.end(),
	                   [](const auto& v) { return v.empty(); });
}

OverlayLine resolveLine(const std::string& tmpl, const KeyLookup& lookup)
{
	OverlayLine out;
	auto append = [&](std::string text, bool dim)
	{
		if(text.empty())
		{
			return;
		}
		if(!out.empty() && out.back().m_dim == dim)
		{
			out.back().m_text += text;
		}
		else
		{
			out.push_back({std::move(text), dim});
		}
	};
	size_t i = 0;
	while(i < tmpl.size())
	{
		const size_t open = tmpl.find("[#", i);
		const size_t close =
		    open == std::string::npos ? open : tmpl.find(']', open + 2);
		if(close == std::string::npos)
		{
			append(tmpl.substr(i), false);
			break;
		}
		append(tmpl.substr(i, open - i), false);
		std::string key = tmpl.substr(open + 2, close - open - 2);
		const bool builtin = !key.empty() && key.front() == '@';
		if(builtin)
		{
			key.erase(0, 1);
		}
		auto value = key.empty() ? std::nullopt : lookup(key, builtin);
		if(value)
		{
			append(*value, false);
		}
		else
		{
			append(tmpl.substr(open, close - open + 1), true);
		}
		i = close + 1;
	}
	return out;
}

AnnotationSet& Annotations::source(const std::string& key)
{
	touch(key);
	return m_sources.front().second;
}

const AnnotationSet* Annotations::findSource(const std::string& key) const
{
	for(const auto& [k, set] : m_sources)
	{
		if(k == key)
		{
			return &set;
		}
	}
	return nullptr;
}

void Annotations::touch(const std::string& key)
{
	auto it = std::find_if(m_sources.begin(),
	                       m_sources.end(),
	                       [&](const auto& p) { return p.first == key; });
	if(it == m_sources.end())
	{
		m_sources.insert(m_sources.begin(), {key, AnnotationSet{}});
	}
	else
	{
		std::rotate(m_sources.begin(), it, it + 1);
	}
}

void Annotations::addHistory(const std::string& line)
{
	if(trim(line).empty())
	{
		return;
	}
	std::erase(m_history, line);
	m_history.insert(m_history.begin(), line);
	if(m_history.size() > MAX_HISTORY)
	{
		m_history.resize(MAX_HISTORY);
	}
}

std::string Annotations::serialize() const
{
	std::ostringstream out;
	out << "# rvtui annotations (session state, rewritten on quit)\n";
	out << "[global]\n";
	writeSet(out, m_global);
	size_t kept = 0;
	for(const auto& [key, set] : m_sources)
	{
		if(set.empty())
		{
			continue;
		}
		if(++kept > MAX_SOURCES)
		{
			break;
		}
		out << "[source " << key << "]\n";
		writeSet(out, set);
	}
	out << "[history]\n";
	for(const auto& h : m_history)
	{
		out << "= " << h << '\n';
	}
	return out.str();
}

void Annotations::parse(const std::string& text)
{
	m_global = {};
	m_sources.clear();
	m_history.clear();
	enum class Section
	{
		NONE,
		GLOBAL,
		SOURCE,
		HISTORY
	} section = Section::NONE;
	AnnotationSet* cur = nullptr;
	std::istringstream in(text);
	std::string line;
	while(std::getline(in, line))
	{
		if(!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		const std::string t = trim(line);
		if(t.empty() || t.front() == '#')
		{
			continue;
		}
		if(t.front() == '[' && t.back() == ']')
		{
			const std::string head = t.substr(1, t.size() - 2);
			if(head == "global")
			{
				section = Section::GLOBAL;
				cur = &m_global;
			}
			else if(head == "history")
			{
				section = Section::HISTORY;
				cur = nullptr;
			}
			else if(head.rfind("source ", 0) == 0)
			{
				section = Section::SOURCE;
				m_sources.emplace_back(trim(head.substr(7)), AnnotationSet{});
				cur = &m_sources.back().second;
			}
			else
			{
				section = Section::NONE;
				cur = nullptr;
			}
			continue;
		}
		const size_t eq = line.find('=');
		if(eq == std::string::npos)
		{
			continue;
		}
		// The value is everything after "= " (inner spacing is kept).
		std::string value = line.substr(eq + 1);
		if(!value.empty() && value.front() == ' ')
		{
			value.erase(0, 1);
		}
		if(section == Section::HISTORY)
		{
			if(m_history.size() < MAX_HISTORY &&
			   std::find(m_history.begin(), m_history.end(), value) ==
			       m_history.end())
			{
				m_history.push_back(value);
			}
		}
		else if(cur)
		{
			if(auto s = slotFromKey(trim(line.substr(0, eq))))
			{
				cur->lines(*s).push_back(value);
			}
		}
	}
	if(m_sources.size() > MAX_SOURCES)
	{
		m_sources.resize(MAX_SOURCES);
	}
}

bool Annotations::load(const fs::path& p)
{
	std::ifstream in(p);
	if(!in)
	{
		return false;
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	parse(ss.str());
	return true;
}

bool Annotations::save(const fs::path& p) const
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	const fs::path tmp = p.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		if(!out)
		{
			spdlog::warn("annotations: cannot write {}", tmp.string());
			return false;
		}
		out << serialize();
	}
	fs::rename(tmp, p, ec);
	if(ec)
	{
		spdlog::warn("annotations: cannot save {}: {}",
		             p.string(),
		             ec.message());
		return false;
	}
	return true;
}

fs::path Annotations::defaultStatePath()
{
	fs::path base;
	if(const char* x = std::getenv("XDG_STATE_HOME"); x && *x)
	{
		base = x;
	}
	else if(const char* home = std::getenv("HOME"); home && *home)
	{
		base = fs::path(home) / ".local" / "state";
	}
	else
	{
		return {};
	}
	return base / "rvtui" / "annotations";
}

std::pair<std::string, AnnotationSet> splitLabels(const std::string& arg)
{
	static const std::string MARK = ":label=";
	std::error_code ec;
	const size_t first = arg.find(MARK);
	if(first == std::string::npos || fs::exists(arg, ec))
	{
		return {arg, {}};
	}
	AnnotationSet labels;
	for(size_t at = first; at != std::string::npos;)
	{
		const size_t slotAt = at + MARK.size();
		const size_t colon = arg.find(':', slotAt);
		if(colon == std::string::npos)
		{
			return {arg, {}};
		}
		std::string key = arg.substr(slotAt, colon - slotAt);
		key = key == "top" ? "tc" : key == "bottom" ? "bc" : key;
		const auto slot = slotFromKey(key);
		if(!slot)
		{
			return {arg, {}};
		}
		const size_t next = arg.find(MARK, colon);
		labels.lines(*slot).push_back(arg.substr(colon + 1,
		                                         next == std::string::npos
		                                             ? std::string::npos
		                                             : next - colon - 1));
		at = next;
	}
	return {arg.substr(0, first), std::move(labels)};
}

} // namespace rv
