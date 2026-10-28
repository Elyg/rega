#include "util/Fuzzy.h"

#include <algorithm>
#include <cctype>

namespace rega
{

namespace
{

std::string lower(std::string s)
{
	std::transform(s.begin(),
	               s.end(),
	               s.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return s;
}

} // namespace

std::optional<std::vector<size_t>> fuzzyMatch(const std::string& name,
                                              const std::string& query)
{
	std::vector<size_t> pos;
	if(query.empty())
	{
		return pos;
	}
	const std::string n = lower(name), q = lower(query);
	if(size_t at = n.find(q); at != std::string::npos)
	{
		for(size_t i = 0; i < q.size(); ++i)
		{
			pos.push_back(at + i);
		}
		return pos;
	}
	size_t qi = 0;
	for(size_t i = 0; i < n.size() && qi < q.size(); ++i)
	{
		if(n[i] == q[qi])
		{
			pos.push_back(i);
			++qi;
		}
	}
	if(qi < q.size())
	{
		return std::nullopt;
	}
	return pos;
}

} // namespace rega
