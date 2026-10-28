#include "app/HelpKeys.h"

#include <spdlog/fmt/fmt.h>

namespace rega::help
{

const std::vector<Section>& browser()
{
	static const std::vector<Section> SECTIONS{
	    {.m_id = "browser",
	     .m_title = "Browser",
	     .m_heading = false,
	     .m_rows =
	         {
	             {"j/k ↑/↓", "move"},
	             {"h/l ←/→", "parent / enter dir or view image"},
	             {"gg / G", "top / bottom"},
	             {"/", "fuzzy filter (Enter keep, Esc clear)"},
	             {"e", "expand sequence into frames / collapse"},
	             {".", "toggle hidden files"},
	             {"space", "mark (view marked together)"},
	             {"a", "mark every image here (again: unmark)"},
	             {"t", "view tiled: the marked side by side"},
	             {"d", "diff the marked in pairs (one marked: with this)"},
	             {"click", "select (double: open) / parent dir"},
	             {"cc cd", "copy path / folder (marked: all of them)"},
	             {"cf cn", "copy file name / name without extension"},
	             {"o", "choose → --chooser-file, quit"},
	             {"q / Q", "quit"},
	         }},
	};
	return SECTIONS;
}

const std::vector<Section>& viewer()
{
	static const std::vector<Section> SECTIONS{
	    {.m_id = "viewer-image",
	     .m_title = "Image",
	     .m_column = 0,
	     .m_rows =
	         {
	             {"[ / ]", "previous / next layer (AOV)"},
	             {"n / p (N)", "next / previous image (marked files)"},
	             {"", "diff: next / previous pair (A, B, |A - B|)"},
	             {"+ / - / wheel", "zoom"},
	             {"f / z", "fit / 1:1"},
	             {"hjkl ←↓↑→ HJKL", "pan (shift = faster)"},
	             {"right drag", "pan (middle button too)"},
	             {"t", "tiles: all layers, or all marked images"},
	             {"  +- wheel hjkl",
	              "tiles: zoom / pan the tiled view (f fit, z 1:1)"},
	             {"click", "select tile / focus pane"},
	             {"drag", "pane edges: width; files title: height"},
	             {"Enter (tile)", "open the selected tile"},
	             {"T", "layer name on the image (tiles: each tile's)"},
	             {"Ctrl+click", "pick colour"},
	             {"", "[x, y] as in Nuke: from the bottom-left"},
	         }},
	    {.m_id = "viewer-sequence",
	     .m_title = "Sequence",
	     .m_column = 0,
	     .m_rows =
	         {
	             {"space", "play / pause"},
	             {", .  < >", "step frame / first, last"},
	             {":", "go to frame (file number, nearest)"},
	             {"G", "missing frames: skip / hold the nearest (timing)"},
	             {"I / O", "in / out point here (again: clear)"},
	             {"", "playback loops in to out; , . < > stay inside"},
	             {"left drag", "scrub (on the image or the playbar)"},
	             {"playbar", "click: go to frame, wheel: step"},
	             {"F", "cycle playback fps"},
	             {"P", "playback: capped resolution (keeps up) / full res"},
	             {"C", "agent mode: picture quality q60 / q85 / q95 / lossless"},
	             {"S", "stats: link, picture cost, caches (anywhere)"},
	         }},
	    {.m_id = "viewer-display",
	     .m_title = "Display",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"c r g b a u", "colour / R / G / B / alpha / luma"},
	             {"e / E", "exposure +/- 0.5 stop"},
	             {"y / Y", "gamma +/- 0.1"},
	             {"s", "view transform (sRGB / OCIO view) / raw"},
	             {"w", "outlines: display + data window / display window / off"},
	             {"w (tiles)",
	              "selection / windows + selection / windows / off"},
	             {"!", "paint NaN (magenta) / inf (cyan) pixels"},
	             {"0", "reset the display (exposure, gamma, channel, …)"},
	             {"A", "show / hide annotations"},
	         }},
	    {.m_id = "viewer-panes",
	     .m_title = "Panes",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"2 3 4 5 6", "metadata / files / inspector / layers / colour"},
	             {"m o/x i /", "the same, as letters"},
	             {"", "open + focus; again while focused closes"},
	             {"1", "focus the image"},
	             {"Tab", "hide all panes / bring them back"},
	         }},
	    {.m_id = "pane-metadata",
	     .m_title = "Metadata (2)",
	     .m_column = 0,
	     .m_rows =
	         {
	             {"j k gg G", "move (Ctrl+d / Ctrl+u half a page)"},
	             {"wheel { }", "scroll"},
	             {"/  click", "filter (Esc clear) / move there"},
	             {"v  drag", "select rows (Ctrl+click: to there)"},
	             {"y  Y", "copy line / value"},
	             {"d", "diff tile: differences / both whole"},
	         }},
	    {.m_id = "pane-files",
	     .m_title = "Files (3)",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"j k  Enter", "move / show that source"},
	             {"J / K  d", "move it down / up, drop it"},
	             {"e", "sequence ⇄ its frames (up to 500)"},
	             {"l / h", "into a row's annotations / back"},
	             {"D D", "clear all annotations (in a row: its own)"},
	         }},
	    {.m_id = "pane-inspector",
	     .m_title = "Inspector (4)",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"Ctrl+click", "pick a pixel (in the image)"},
	             {"j k  y  Y", "move, copy line / value"},
	             {"x", "clear the pick"},
	         }},
	    {.m_id = "pane-layers",
	     .m_title = "Layers (5)",
	     .m_column = 0,
	     .m_rows =
	         {
	             {"j k gg G  click", "show that layer"},
	         }},
	    {.m_id = "pane-colour",
	     .m_title = "Colour (6)",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"j k", "move between the rows"},
	             {"h / l  Enter", "change the row / pick from a list"},
	         }},
	    {.m_id = "pane-annotations",
	     .m_title = "Annotations (3, then l)",
	     .m_column = 1,
	     .m_rows =
	         {
	             {"a A Enter d", "add below / above, edit, delete"},
	             {"J / K  g / s", "reorder, to global / source"},
	             {"Tab ↑↓ Ctrl+n",
	              "typing: next slot, history, [#key] / [#@key]"},
	         }},
	    {.m_id = "viewer-leave",
	     .m_title = "Leaving the viewer",
	     .m_heading = false,
	     .m_column = 1,
	     .m_rows =
	         {
	             {"q / Esc", "back to browser"},
	             {"Q", "quit rega"},
	         }},
	};
	return SECTIONS;
}

namespace
{

// A table cell: no leading indent, and `|` escaped so it can't end the cell.
std::string cell(const std::string& s)
{
	std::string out;
	for(char c : s.substr(std::min(s.find_first_not_of(' '), s.size())))
	{
		if(c == '|')
		{
			out += '\\';
		}
		out += c;
	}
	return out;
}

void appendSections(std::string& out,
                    const std::string& mode,
                    const std::vector<Section>& sections)
{
	for(const Section& s : sections)
	{
		out += fmt::format("<!-- ANCHOR: {} -->\n", s.m_id);
		out += s.m_title == mode
		           ? fmt::format("### {}\n\n", mode)
		           : fmt::format("### {}: {}\n\n", mode, s.m_title);
		out += "| Key | Does |\n|-----|------|\n";
		for(size_t i = 0; i < s.m_rows.size(); ++i)
		{
			// Continuation rows join the row above.
			const std::string key = cell(s.m_rows[i].m_key);
			std::string text = cell(s.m_rows[i].m_text);
			while(i + 1 < s.m_rows.size() && s.m_rows[i + 1].m_key.empty())
			{
				text += "<br>" + cell(s.m_rows[++i].m_text);
			}
			out += fmt::format("| `{}` | {} |\n", key, text);
		}
		out += fmt::format("<!-- ANCHOR_END: {} -->\n\n", s.m_id);
	}
}

} // namespace

std::string markdown()
{
	std::string out =
	    "# Keys reference\n\n"
	    "<!-- Generated by `just guide-keys` (rega --help-keys) from "
	    "src/app/HelpKeys.cpp. Don't edit by hand. -->\n\n"
	    "Every key, as the `?` overlay lists it. In the overlay, type to "
	    "search.\n\n";
	appendSections(out, "Browser", browser());
	appendSections(out, "Viewer", viewer());
	out.pop_back(); // one newline at the end
	return out;
}

} // namespace rega::help
