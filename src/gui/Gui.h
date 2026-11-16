#pragma once

#include <string>

namespace rega
{

struct AppOptions;

namespace gui
{

/// `--gui` only options.
struct GuiOptions
{
	/// Hidden `--gui-screenshot <png>`: draw until the app settles, save the
	/// window's pixels there, quit (CI checks the window draws).
	std::string m_screenshot;
	/// Hidden `--gui-keys <text>`: typed once the window is up, as if at the
	/// keyboard ("\n" is Enter, "\e" Escape), for screenshots of a state.
	std::string m_keys;
	/// Hidden `--gui-script <file>`: after --gui-keys, timed input and
	/// frames saved, for looking at motion (zooming, panning) frame by
	/// frame. One command a line: `wait MS`, `wheel X Y DY` (window
	/// coordinates; DY notches, fractions too), `drag X0 Y0 X1 Y1 STEPS`
	/// (left button), `shot PNG`, `frames DIR N` (the next N frames as
	/// DIR/NNNN.png). It quits at the end.
	std::string m_script;
};

/// Run rega in a window of its own until it is closed or quits; returns the
/// exit code. The same app as in the terminal (App), drawn by the window:
/// its cells in the built-in font, its pictures as textures.
int run(const AppOptions& opts, const GuiOptions& gui = {});

} // namespace gui
} // namespace rega
