#include "util/Clipboard.h"

#include "term/Kitty.h"
#include "term/Output.h"
#include "util/Sandbox.h"

#include <cstdint>
#include <cstdio>

namespace rega
{

std::string osc52(std::string_view text)
{
	return "\x1b]52;c;" +
	       kitty::base64(reinterpret_cast<const uint8_t*>(text.data()),
	                     text.size()) +
	       "\x07";
}

bool copyToClipboard(std::string_view text, bool tmux)
{
	// -w also sends the buffer to the terminal's clipboard (OSC 52).
	const bool local = localOnly();
	if(tmux)
	{
		if(FILE* f = popen(local ? "tmux load-buffer - 2>/dev/null"
		                         : "tmux load-buffer -w - 2>/dev/null",
		                   "w"))
		{
			std::fwrite(text.data(), 1, text.size(), f);
			return pclose(f) == 0;
		}
		return false;
	}
	if(local)
	{
		return false; // the terminal's clipboard is the only one there is
	}
	term::writeRaw(osc52(text), true);
	return true;
}

bool tmuxSelectPane(char dir, bool tmux)
{
	if(!tmux)
	{
		return false;
	}
	std::string cmd = std::string("tmux select-pane -") + dir + " 2>/dev/null";
	if(FILE* f = popen(cmd.c_str(), "r"))
	{
		return pclose(f) == 0;
	}
	return false;
}

} // namespace rega
