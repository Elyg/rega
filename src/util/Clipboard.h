#pragma once

#include <string>
#include <string_view>

namespace rega
{

/// The OSC 52 sequence that puts `text` on the terminal's clipboard.
[[nodiscard]] std::string osc52(std::string_view text);

/// Put text on the system clipboard. In tmux, through tmux's buffer (`-w` also
/// sets the outer terminal's clipboard); otherwise OSC 52, which Ghostty and
/// kitty honour.
[[nodiscard]] bool copyToClipboard(std::string_view text, bool tmux);

/// Move to the neighbouring tmux pane ('L','R','U','D'); false outside tmux.
bool tmuxSelectPane(char dir, bool tmux);

} // namespace rega
