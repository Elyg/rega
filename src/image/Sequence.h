#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rega
{

/// A directory entry as shown by the browser: a directory, a single file, or a
/// collapsed numbered image sequence (`shot.1001.exr` … `shot.1100.exr`).
struct Entry
{
	enum class Kind
	{
		DIR,
		FILE,
		SEQUENCE
	};
	Kind m_kind = Kind::FILE;
	std::string m_name;           ///< display name
	std::filesystem::path m_path; ///< dir/file path, or first frame
	std::vector<std::filesystem::path> m_frames; ///< sequence frames (sorted)
	std::vector<int> m_frameNumbers;             ///< parallel to `frames`
	std::uintmax_t m_size = 0; ///< bytes (file) / total (sequence)
	/// Browser: a frame shown expanded out of its sequence carries that
	/// sequence's key (see App), so `e` can fold it back.
	std::string m_expandedFrom;

	/// A sequence, or a supported image file.
	bool isImage() const;
	std::string rangeString() const; ///< "1001-1050,1060-1100"
};

/// Split a filename into prefix/number/suffix if it looks like a frame
/// ("a.1001.exr", "a_0001.png"). Returns false otherwise.
bool splitFrame(const std::string& filename,
                std::string& prefix,
                std::string& digits,
                std::string& suffix);

/// Group plain filenames (in one directory) into entries; sequences need >= 2
/// frames.
std::vector<Entry> groupFiles(const std::filesystem::path& dir,
                              const std::vector<std::string>& filenames);

/// List a directory: dirs first, then files/sequences, each sorted by name.
std::vector<Entry> listDirectory(const std::filesystem::path& dir,
                                 bool showHidden);

/// Build a sequence entry for a single frame path by scanning its siblings
/// (used when rega is launched on a file). Returns a File entry if no sibling
/// frames exist.
Entry entryForPath(const std::filesystem::path& file);

/// Resolve command-line paths into viewer entries. A plain frame path opens
/// just that file; a sequence needs a glob — `shot.*.exr`, `shot.####.exr`
/// (each # one digit), `shot.#.exr` (a lone # takes any padding), or the
/// shell's expansion of one — whose matches are
/// grouped like a directory listing. Arguments that match nothing land in
/// `missing`; non-images are dropped.
std::vector<Entry> entriesForArgs(const std::vector<std::string>& args,
                                  std::vector<std::string>& missing);

/// "690B", "2.5M", "1.1G".
std::string humanSize(std::uintmax_t bytes);

} // namespace rega
