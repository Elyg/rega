#pragma once

#include <optional>
#include <string>
#include <vector>

namespace rega
{

/// Case-insensitive match of `query` in `name`: a substring if there is one,
/// else a subsequence ("bbl" finds "Beachball"). Returns the byte positions
/// matched (empty for an empty query), or nullopt when it does not match.
std::optional<std::vector<size_t>> fuzzyMatch(const std::string& name,
                                              const std::string& query);

} // namespace rega
