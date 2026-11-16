#include "gui/Settings.h"

#include "util/Paths.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace fs = std::filesystem;

namespace rega::gui
{

namespace
{

std::string_view trim(std::string_view s)
{
	const auto b = s.find_first_not_of(" \t\r");
	if(b == std::string_view::npos)
	{
		return {};
	}
	const auto e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

template <typename T>
void number(std::string_view v, T& out)
{
	if constexpr(std::is_floating_point_v<T>)
	{
		// Not from_chars: Apple's libc++ has it for floats only from macOS 26.
		// strtod in the "C" locale (rega never calls setlocale): '.' decimals.
		const std::string s(v);
		char* end = nullptr;
		const double x = std::strtod(s.c_str(), &end);
		if(!s.empty() && end == s.c_str() + s.size() && std::isfinite(x))
		{
			out = static_cast<T>(x);
		}
	}
	else
	{
		T x{};
		const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
		if(ec == std::errc() && p == v.data() + v.size())
		{
			out = x;
		}
	}
}

} // namespace

Settings Settings::load(const fs::path& file)
{
	Settings s;
	std::ifstream in(file);
	std::string line;
	while(std::getline(in, line))
	{
		const auto eq = line.find('=');
		if(eq == std::string::npos || trim(line).starts_with('#'))
		{
			continue;
		}
		const std::string_view key = trim(std::string_view(line).substr(0, eq));
		const std::string_view value =
		    trim(std::string_view(line).substr(eq + 1));
		if(key == "font_size")
		{
			number(value, s.m_fontSize);
		}
		else if(key == "width")
		{
			number(value, s.m_width);
		}
		else if(key == "height")
		{
			number(value, s.m_height);
		}
		else if(key == "maximized")
		{
			s.m_maximized = value == "1" || value == "true";
		}
		else if(key == "last_dir")
		{
			s.m_lastDir = fs::path(std::u8string(value.begin(), value.end()));
		}
	}
	s.m_fontSize = std::clamp(s.m_fontSize, MIN_FONT_SIZE, MAX_FONT_SIZE);
	s.m_width = std::clamp(s.m_width, 320, 16384);
	s.m_height = std::clamp(s.m_height, 240, 16384);
	return s;
}

bool Settings::save(const fs::path& file) const
{
	if(file.empty())
	{
		return false;
	}
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	const fs::path tmp = fs::path(file).concat(".tmp");
	{
		std::ofstream out(tmp, std::ios::trunc);
		if(!out)
		{
			return false;
		}
		const std::u8string dir = m_lastDir.u8string();
		out << "# rega --gui: the window as it was left\n"
		    << "font_size = " << m_fontSize << "\n"
		    << "width = " << m_width << "\n"
		    << "height = " << m_height << "\n"
		    << "maximized = " << (m_maximized ? 1 : 0) << "\n"
		    << "last_dir = " << std::string(dir.begin(), dir.end()) << "\n";
		if(!out)
		{
			return false;
		}
	}
	fs::rename(tmp, file, ec);
	return !ec;
}

fs::path Settings::defaultPath()
{
	const fs::path dir = paths::stateDir();
	return dir.empty() ? dir : dir / "window";
}

} // namespace rega::gui
