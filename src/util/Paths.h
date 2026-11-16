#pragma once

#include <filesystem>

namespace rega::paths
{

/// The user's home directory ($HOME; %USERPROFILE% on Windows); empty when
/// unknown.
[[nodiscard]] std::filesystem::path home();

/// Where rega keeps what it remembers between runs (annotations, the colour
/// config picked, the window's size and folder): $XDG_STATE_HOME/rega, else
/// ~/.local/state/rega; %LOCALAPPDATA%\rega on Windows. Empty when there is
/// no home to put it in.
[[nodiscard]] std::filesystem::path stateDir();

/// Where rega keeps what it can make again (the log, the tutorial images):
/// $XDG_CACHE_HOME/rega, else ~/.cache/rega; %LOCALAPPDATA%\rega\cache on
/// Windows. Empty when there is no home to put it in.
[[nodiscard]] std::filesystem::path cacheDir();

} // namespace rega::paths
