#pragma once

#include <filesystem>
#include <string>

namespace rega::gui
{

/// What the window remembers between runs, in paths::stateDir()/window: one
/// `key = value` a line.
struct Settings
{
	static constexpr float DEFAULT_FONT_SIZE = 15.0f; ///< px at 100% scale
	static constexpr float MIN_FONT_SIZE = 8.0f, MAX_FONT_SIZE = 40.0f;

	float m_fontSize = DEFAULT_FONT_SIZE;
	int m_width = 1440, m_height = 900; ///< window size, in window units
	bool m_maximized = false;
	std::filesystem::path m_lastDir; ///< where the browser was at exit

	/// The settings in `file`, or the defaults for what it lacks.
	[[nodiscard]] static Settings load(const std::filesystem::path& file);
	/// Write them to `file` (its directory made if missing); false on failure.
	bool save(const std::filesystem::path& file) const;
	/// paths::stateDir()/window; empty without a state directory.
	[[nodiscard]] static std::filesystem::path defaultPath();
};

} // namespace rega::gui
