#pragma once

#include <functional>

namespace rega
{

/// Number of threads parallelFor() spreads work over (workers + the caller).
int parallelism();

/// Call fn(begin, end) over [0, n) in chunks of about `grain`, spread across a
/// process-wide pool of std::jthread workers; the caller works too and returns
/// once every chunk is done. Called from inside a pool task it runs serially,
/// so nesting cannot deadlock.
void parallelFor(int n, int grain, const std::function<void(int, int)>& fn);

} // namespace rega
