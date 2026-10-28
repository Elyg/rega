#include "term/Doctor.h"

#include "term/Caps.h"
#include "util/Log.h"

#include <spdlog/fmt/fmt.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace rega
{

namespace
{

std::string env(const char* k)
{
	const char* v = std::getenv(k);
	return v ? v : "";
}

std::string orDash(const std::string& s)
{
	return s.empty() ? "-" : s;
}

} // namespace

std::vector<DoctorCheck> doctorChecks(const std::string& forced)
{
	using L = DoctorCheck::Level;
	std::vector<DoctorCheck> out;
	const TermCaps caps = detectCaps(forced);
	const bool tmux = caps.m_tmux;
	const std::string term = env("TERM"), prog = env("TERM_PROGRAM");
	const std::string client = tmux ? tmuxDisplay("#{client_termname}") : "";

	// Terminal: one with kitty graphics + Unicode placeholders? Outside tmux
	// it was asked (detectCaps); in it, only the environment can say.
	const bool asked = !tmux && !caps.m_outerTmux;
	const TerminalReply reply = asked ? queryTerminal() : TerminalReply{};
	const bool kittyTerm =
	    reply.m_answered
	        ? reply.m_kittyGraphics &&
	              (reply.m_version.empty() || isKittyTerminal(reply.m_version))
	        : isKittyTerminal(term) || isKittyTerminal(prog) ||
	              !env("KITTY_WINDOW_ID").empty() ||
	              !env("GHOSTTY_RESOURCES_DIR").empty() ||
	              isKittyTerminal(client) ||
	              (caps.m_outerTmux && isKittyTerminal(terminalHint(tmux)));
	{
		DoctorCheck c{kittyTerm ? L::OK : L::WARN, "terminal", "", ""};
		c.m_detail = fmt::format("TERM={} TERM_PROGRAM={}{}",
		                         orDash(term),
		                         orDash(prog),
		                         tmux ? " tmux client=" + orDash(client) : "");
		if(reply.m_answered)
		{
			c.m_detail += fmt::format("; asked: {}, kitty graphics {}",
			                          reply.m_version.empty() ? "no XTVERSION"
			                                                  : reply.m_version,
			                          reply.m_kittyGraphics ? "yes" : "no");
		}
		else if(asked)
		{
			c.m_detail += "; asked, no answer";
		}
		if(!kittyTerm)
		{
			c.m_detail += " — no kitty graphics with placeholders";
			c.m_fix = "Ghostty or kitty, without a multiplexer in between "
			          "that drops images (zellij), for full-resolution images "
			          "(others get half-blocks)";
		}
		out.push_back(c);
	}

	if(caps.m_outerTmux)
	{
		// ssh'd here from a tmux pane: TERM (no tmux here) or our tmux's
		// client (tmux in tmux) names that tmux.
		const std::string hint = terminalHint(tmux);
		DoctorCheck c{kittyTerm ? L::OK : L::WARN,
		              "outer tmux",
		              fmt::format("{}={}: a tmux beyond ssh (images need its "
		                          "allow-passthrough on), LC_TERMINAL={}",
		                          tmux ? "tmux client" : "TERM",
		                          tmux ? client : term,
		                          orDash(hint)),
		              ""};
		if(!kittyTerm)
		{
			c.m_detail += " — it hides the terminal";
			c.m_fix = "on the machine you ssh from, an `ssh` wrapper passing "
			          "LC_TERMINAL=$(tmux display -p '#{client_termname}') "
			          "(ssh sends LC_*; the server needs `AcceptEnv LC_*`), "
			          "or --graphics kitty here";
		}
		out.push_back(c);
		// The session keeps the LC_TERMINAL of the attach that created it.
		if(tmux)
		{
			const std::string update = tmuxOption("update-environment");
			const bool updated =
			    update.find("LC_TERMINAL") != std::string::npos;
			out.push_back(
			    {updated ? L::OK : L::WARN,
			     "update-env",
			     updated ? "tmux refreshes LC_TERMINAL on attach"
			             : "tmux keeps LC_TERMINAL from the first attach",
			     updated ? ""
			             : "add `set -ga update-environment LC_TERMINAL` "
			               "to this machine's tmux.conf"});
		}
	}

	if(tmux)
	{
		out.push_back(
		    {L::OK, "tmux", "version " + orDash(tmuxDisplay("#{version}")), ""});
		const std::string pt = tmuxOption("allow-passthrough");
		const bool ptOn = pt == "on" || pt == "all";
		DoctorCheck c{ptOn ? L::OK : (kittyTerm ? L::FAIL : L::INFO),
		              "passthrough",
		              "allow-passthrough " + orDash(pt),
		              ""};
		if(!ptOn)
		{
			c.m_detail += " — tmux drops kitty images";
			c.m_fix = "add `set -g allow-passthrough on` to tmux.conf, then "
			          "`tmux source-file ~/.config/tmux/tmux.conf`";
		}
		out.push_back(c);

		int w = 0, h = 0;
		const std::string cell =
		    tmuxDisplay("#{client_cell_width} #{client_cell_height}");
		if(std::sscanf(cell.c_str(), "%d %d", &w, &h) == 2 && w > 0 && h > 0)
		{
			out.push_back(
			    {L::OK, "cell size", fmt::format("{}x{} px", w, h), ""});
		}
		else if(client.empty())
		{
			out.push_back(
			    {L::INFO, "cell size", "no tmux client attached to ask", ""});
		}
		else
		{
			out.push_back({L::WARN,
			               "cell size",
			               "tmux does not report the client's cell size",
			               "tmux 3.4+ (images may be scaled wrong otherwise)"});
		}

		const std::string features = tmuxDisplay("#{client_termfeatures}");
		const bool rgb = features.find("RGB") != std::string::npos;
		if(client.empty())
		{
			out.push_back(
			    {L::INFO, "truecolor", "no tmux client attached", ""});
		}
		else
		{
			out.push_back(
			    {rgb ? L::OK : L::WARN,
			     "truecolor",
			     rgb ? "tmux client has RGB" : "tmux client lacks RGB",
			     rgb ? ""
			         : fmt::format("set -as terminal-features ',{}*:RGB'",
			                       client.starts_with("tmux") ? "tmux"
			                                                  : "xterm")});
		}

		const std::string clip = tmuxDisplay("#{set-clipboard}");
		const bool clipOn = clip == "on" || clip == "external";
		out.push_back({clipOn ? L::OK : L::WARN,
		               "clipboard",
		               "set-clipboard " + orDash(clip),
		               clipOn ? ""
		                      : "set -g set-clipboard on (y copies "
		                        "stay inside tmux otherwise)"});
	}
	else
	{
		struct winsize ws{};
		const bool px = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
		                ws.ws_xpixel > 0 && ws.ws_col > 0;
		out.push_back(
		    {px ? L::OK : (kittyTerm ? L::WARN : L::INFO),
		     "cell size",
		     px ? fmt::format("{}x{} px",
		                      ws.ws_xpixel / ws.ws_col,
		                      ws.ws_ypixel / std::max<int>(1, ws.ws_row))
		        : "the terminal reports no pixel size",
		     px ? "" : "images assume 10x20 px cells"});
		const std::string ct = env("COLORTERM");
		const bool rgb = ct == "truecolor" || ct == "24bit";
		out.push_back({rgb ? L::OK : L::WARN,
		               "truecolor",
		               "COLORTERM=" + orDash(ct),
		               rgb ? "" : "export COLORTERM=truecolor"});
		out.push_back({L::INFO, "clipboard", "OSC 52 to the terminal", ""});
	}

	const bool sshEnv =
	    !env("SSH_CONNECTION").empty() || !env("SSH_TTY").empty();
	if(caps.m_tmux && tmuxClientOverSsh())
	{
		out.push_back({L::INFO,
		               "ssh",
		               "a tmux client is attached over ssh: images are sent "
		               "over the link, paced to --link-rate",
		               ""});
	}
	else if(caps.m_tmux && sshEnv)
	{
		out.push_back({L::INFO,
		               "ssh",
		               "tmux was started over ssh, but no client is attached "
		               "over ssh now: images stay on this machine",
		               ""});
	}
	else if(sshEnv)
	{
		out.push_back({L::INFO,
		               "ssh",
		               "remote session: images are sent over the link, "
		               "paced to --link-rate",
		               ""});
	}
	if(forced == "kitty" && !kittyTerm)
	{
		out.push_back({L::WARN,
		               "forced",
		               "--graphics kitty on a terminal not detected as kitty",
		               ""});
	}
	out.push_back({L::INFO,
	               "graphics",
	               std::string(graphicsModeName(caps.m_graphics)) +
	                   (forced == "auto" ? "" : " (forced)"),
	               ""});
	if(caps.m_graphics == GraphicsMode::KITTY)
	{
		const bool local = caps.m_transfer != Transfer::DIRECT;
		out.push_back(
		    {L::INFO,
		     "transfer",
		     caps.m_transfer == Transfer::SHARED_MEMORY
		         ? "shm: pixels go through shared memory, not the pty"
		     : local ? "file: pixels go through temp files, not the pty"
		             : "direct: pixels are compressed and sent through the pty",
		     local ? "--transfer direct if images stay blank (the terminal "
		             "must run on this machine)"
		           : ""});
	}
	return out;
}

int runDoctor(const std::string& forced)
{
	using L = DoctorCheck::Level;
	auto& out = log::out();
	const bool color = isatty(STDOUT_FILENO);
	auto paint = [&](const char* code, const std::string& s)
	{ return color ? fmt::format("\x1b[{}m{}\x1b[0m", code, s) : s; };
	out.info("rega doctor");
	bool failed = false;
	for(const auto& c : doctorChecks(forced))
	{
		std::string mark;
		switch(c.m_level)
		{
			case L::OK:
				mark = paint("32", "✓");
				break;
			case L::INFO:
				mark = paint("2", "·");
				break;
			case L::WARN:
				mark = paint("33", "!");
				break;
			case L::FAIL:
				mark = paint("31", "✗");
				failed = true;
				break;
		}
		out.info("  {} {:<12} {}", mark, c.m_name, c.m_detail);
		if(!c.m_fix.empty())
		{
			out.info("    {} {}", paint("2", "fix:"), c.m_fix);
		}
	}
	return failed ? 1 : 0;
}

} // namespace rega
