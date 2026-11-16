#include "util/Paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace rega::paths
{

namespace
{

// An environment variable, or empty when unset or empty.
fs::path env(const char* name)
{
	const char* v = std::getenv(name);
	return v && *v ? fs::path(v) : fs::path();
}

#ifdef _WIN32
fs::path localAppData()
{
	fs::path base = env("LOCALAPPDATA");
	if(base.empty() && !home().empty())
	{
		base = home() / "AppData" / "Local";
	}
	return base;
}
#endif

} // namespace

fs::path home()
{
#ifdef _WIN32
	if(fs::path p = env("USERPROFILE"); !p.empty())
	{
		return p;
	}
#endif
	return env("HOME");
}

fs::path stateDir()
{
#ifdef _WIN32
	const fs::path base = localAppData();
	return base.empty() ? base : base / "rega";
#else
	fs::path base = env("XDG_STATE_HOME");
	if(base.empty() && !home().empty())
	{
		base = home() / ".local" / "state";
	}
	return base.empty() ? base : base / "rega";
#endif
}

fs::path cacheDir()
{
#ifdef _WIN32
	const fs::path base = localAppData();
	return base.empty() ? base : base / "rega" / "cache";
#else
	fs::path base = env("XDG_CACHE_HOME");
	if(base.empty() && !home().empty())
	{
		base = home() / ".cache";
	}
	return base.empty() ? base : base / "rega";
#endif
}

} // namespace rega::paths
