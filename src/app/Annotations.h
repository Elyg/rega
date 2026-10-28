#pragma once

#include "image/Overlay.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rega
{

/// Raw template lines per slot (before `[#…]` substitution).
struct AnnotationSet
{
	std::array<std::vector<std::string>, SLOT_COUNT> m_slots;

	std::vector<std::string>& lines(Slot s)
	{
		return m_slots[static_cast<int>(s)];
	}
	const std::vector<std::string>& lines(Slot s) const
	{
		return m_slots[static_cast<int>(s)];
	}
	bool empty() const; ///< no line in any slot
};

/// Value for a placeholder: `[#key]` (header attribute, builtin = false) or
/// `[#@key]` (always available, builtin = true). nullopt = unknown.
using KeyLookup =
    std::function<std::optional<std::string>(const std::string& key,
                                             bool builtin)>;

/// A command-line image: `path[:label=<slot>:<text>]…`, the slot one of
/// tl tc tr bl bc br, or top / bottom (tc / bc). The text runs to the next
/// `:label=` and may hold placeholders. An argument naming an existing file is
/// all path, and one whose labels don't parse is left whole (and reported
/// not found). Returns the path and its labels.
std::pair<std::string, AnnotationSet> splitLabels(const std::string& arg);

/// Substitute the placeholders of one template line. Unknown keys stay literal,
/// in a dim run.
OverlayLine resolveLine(const std::string& tmpl, const KeyLookup& lookup);

/// Quick burn-in text: one global set over every image plus one set per viewer
/// source (appended after the global lines). Session state, saved on quit and
/// restored on the next run, with a recall history of typed lines.
class Annotations
{
public:
	static constexpr size_t MAX_SOURCES = 200;
	static constexpr size_t MAX_HISTORY = 100;

	/// The lines shown over every image.
	AnnotationSet& global()
	{
		return m_global;
	}
	const AnnotationSet& global() const
	{
		return m_global;
	}
	/// The set for a source key (created on demand), moved to the front of the
	/// most-recently-used order.
	AnnotationSet& source(const std::string& key);
	/// The set for `key`, or nullptr; doesn't change the order.
	const AnnotationSet* findSource(const std::string& key) const;
	/// Mark a source as used (it is opened): keeps it from being pruned.
	void touch(const std::string& key);

	/// Committed lines, most recent first, no duplicates.
	const std::vector<std::string>& history() const
	{
		return m_history;
	}
	void addHistory(const std::string& line); ///< see history()

	bool m_visible = true;

	/// Line-based state file: `[global]`, `[source <key>]`, `[history]`
	/// sections of
	/// `<slot> = <text>` (history: `= <text>`) lines.
	std::string serialize() const;
	void parse(const std::string& text);             ///< replaces the state
	bool load(const std::filesystem::path& p);       ///< false if unreadable
	bool save(const std::filesystem::path& p) const; ///< false on error
	/// $XDG_STATE_HOME/rega/annotations (~/.local/state/rega/annotations).
	static std::filesystem::path defaultStatePath();

private:
	AnnotationSet m_global;
	std::vector<std::pair<std::string, AnnotationSet>> m_sources; ///< MRU first
	std::vector<std::string> m_history;
};

} // namespace rega
