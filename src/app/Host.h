#pragma once

#include <functional>
#include <string_view>

namespace rega
{

/// What runs App: the terminal (FTXUI's ScreenInteractive) or the window
/// (`--gui`). The event loop, and the way back into it from other threads.
class Host
{
public:
	virtual ~Host() = default;

	/// Ask for a redraw (App::onEvent sees Event::Custom first). Safe from
	/// any thread.
	virtual void redraw() = 0;
	/// Run `task` on the UI thread, without a redraw of its own. Safe from
	/// any thread.
	virtual void post(std::function<void()> task) = 0;
	/// Leave the event loop.
	virtual void quit() = 0;
	/// The window (true) or the terminal.
	[[nodiscard]] virtual bool gui() const noexcept
	{
		return false;
	}
	/// Put `text` on the system clipboard; false when nothing was copied.
	/// The terminal leaves it to copyToClipboard() (tmux / OSC 52).
	virtual bool copyText(std::string_view /*text*/)
	{
		return false;
	}
};

} // namespace rega
