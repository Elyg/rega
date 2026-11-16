#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rega
{

/// One thing opened: the paths that open it again (files, sequence patterns
/// such as `/renders/shot.####.exr`, or one folder), and when.
struct RecentItem
{
	std::vector<std::string> m_paths;
	std::int64_t m_when = 0; ///< seconds since the epoch
	bool operator==(const RecentItem&) const = default;
};

/// What the window opened lately, newest first, for its start screen. Kept
/// in a file, one item a line: `<when>\t<path>\t<path>…`.
class Recent
{
public:
	static constexpr size_t MAX_ITEMS = 10;
	/// More paths than this in one open are remembered as their folder.
	static constexpr size_t MAX_PATHS = 20;

	/// `file` empty: nothing is read or written (and nothing remembered).
	explicit Recent(std::filesystem::path file = {});

	/// paths::stateDir()/recent; empty without a state directory.
	[[nodiscard]] static std::filesystem::path defaultPath();

	[[nodiscard]] bool enabled() const noexcept
	{
		return !m_file.empty();
	}
	/// Read the file (what it lacks or can't parse is skipped).
	void load();
	/// Write the file; false on failure.
	bool save() const;

	/// `paths` were opened at `when`: first in the list now, an older item
	/// with the same paths dropped, the oldest past MAX_ITEMS too. Paths with
	/// a tab or a newline can't be kept and are left out. Saves.
	void add(std::vector<std::string> paths, std::int64_t when);
	/// Drop the item at `i` (as items() lists them). Saves.
	void remove(size_t i);

	[[nodiscard]] const std::vector<RecentItem>& items() const noexcept
	{
		return m_items;
	}

private:
	std::filesystem::path m_file;
	std::vector<RecentItem> m_items;
};

/// Whether what `path` names is still there: the file or folder, or for a
/// sequence pattern (`#` or `*` in its name) the folder it is in.
[[nodiscard]] bool recentPathExists(const std::string& path);

/// How an item reads in a list: its name (a folder's with a trailing `/`,
/// "+N more" after it when it is several) and where it is (`~` for home).
struct RecentLabel
{
	std::string m_name, m_where;
};
[[nodiscard]] RecentLabel recentLabel(const RecentItem& item,
                                      const std::filesystem::path& home);

/// `dir` with `home` shown as `~`.
[[nodiscard]] std::string tildePath(const std::filesystem::path& dir,
                                    const std::filesystem::path& home);

/// "just now", "5 min ago", "3 h ago", "2 days ago", "4 weeks ago".
[[nodiscard]] std::string agoText(std::int64_t seconds);

} // namespace rega
