#pragma once

#include <string_view>

namespace rega::term
{

/// Raw terminal protocol bytes (kitty graphics escapes). Not logging: these
/// must go through the same stream FTXUI draws with (std::cout), so they reach
/// the terminal in order, ahead of the frame being drawn.
void writeRaw(std::string_view bytes, bool flush = false);

} // namespace rega::term
