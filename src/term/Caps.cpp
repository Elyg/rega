#include "term/Caps.h"

#include <sys/ioctl.h>
#include <sys/select.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace rega
{

namespace
{
// Output of a shell command, trailing whitespace trimmed ("" on failure).
std::string run(const char* cmd)
{
	std::string out;
	if(FILE* f = popen(cmd, "r"))
	{
		char buf[256];
		while(fgets(buf, sizeof buf, f))
		{
			out += buf;
		}
		pclose(f);
	}
	while(!out.empty() && (out.back() == '\n' || out.back() == ' '))
	{
		out.pop_back();
	}
	return out;
}

std::string env(const char* k)
{
	const char* v = std::getenv(k);
	return v ? v : "";
}

// The kitty graphics query's image id; the answer names it.
constexpr std::string_view QUERY_ID = "1795";

// Where a DA1 answer (ESC [ ? digits/semicolons c) ends in `in`, or npos.
size_t deviceAttributesEnd(std::string_view in)
{
	for(size_t at = in.find("\x1b[?"); at != std::string_view::npos;
	    at = in.find("\x1b[?", at + 1))
	{
		size_t i = at + 3;
		while(i < in.size() &&
		      (std::isdigit(static_cast<unsigned char>(in[i])) || in[i] == ';'))
		{
			++i;
		}
		if(i < in.size() && in[i] == 'c')
		{
			return i + 1;
		}
	}
	return std::string_view::npos;
}
} // namespace

std::string tmuxDisplay(const std::string& format)
{
	return run(("tmux display -p '" + format + "' 2>/dev/null").c_str());
}

std::string tmuxOption(const std::string& name)
{
	return run(("tmux show -Apv " + name + " 2>/dev/null").c_str());
}

bool sshAncestor(int pid, const std::string& psTable)
{
	std::unordered_map<int, std::pair<int, std::string>> procs; // ppid, comm
	std::istringstream in(psTable);
	std::string line;
	while(std::getline(in, line))
	{
		std::istringstream ls(line);
		int p = 0, pp = 0;
		std::string comm;
		if(ls >> p >> pp && std::getline(ls >> std::ws, comm))
		{
			procs[p] = {pp, comm};
		}
	}
	// sshd, sshd-session (OpenSSH 9.8+), /usr/sbin/sshd (macOS: the path).
	for(int hops = 0; hops < 64 && pid > 1; ++hops)
	{
		const auto it = procs.find(pid);
		if(it == procs.end())
		{
			return false;
		}
		if(it->second.second.find("sshd") != std::string::npos)
		{
			return true;
		}
		pid = it->second.first;
	}
	return false;
}

std::string tmuxClientPids()
{
	return run("tmux list-clients -F '#{client_pid}' "
	           "-t \"$(tmux display -p '#{session_id}')\" 2>/dev/null");
}

bool tmuxClientOverSsh()
{
	return tmuxClientOverSsh(tmuxClientPids());
}

bool tmuxClientOverSsh(const std::string& pids)
{
	if(pids.empty())
	{
		return false;
	}
	const std::string table = run("ps -A -o pid=,ppid=,comm= 2>/dev/null");
	std::istringstream in(pids);
	int pid = 0;
	while(in >> pid)
	{
		if(sshAncestor(pid, table))
		{
			return true;
		}
	}
	return false;
}

std::string sshEndpoint(std::string_view address, std::string_view port)
{
	if(address.size() >= 2 && address.front() == '[' && address.back() == ']')
	{
		address = address.substr(1, address.size() - 2);
	}
	if(const size_t scope = address.find('%'); scope != std::string_view::npos)
	{
		address = address.substr(0, scope); // fe80::1%eth0
	}
	if(address.starts_with("::ffff:") && address.find('.') != address.npos)
	{
		address.remove_prefix(7);
	}
	return std::string(address) + ":" + std::string(port);
}

std::optional<SshPeer> parseSshConnection(std::string_view value)
{
	std::istringstream in{std::string(value)};
	std::string clientIp, clientPort, serverIp, serverPort;
	if(!(in >> clientIp >> clientPort >> serverIp >> serverPort))
	{
		return std::nullopt;
	}
	return SshPeer{sshEndpoint(clientIp, clientPort), serverPort};
}

std::vector<SshPeer> tmuxSshPeers()
{
	std::vector<SshPeer> peers;
	std::istringstream pids(tmuxClientPids());
	std::string pid;
	while(pids >> pid)
	{
		std::ifstream environ("/proc/" + pid + "/environ", std::ios::binary);
		std::string var;
		while(std::getline(environ, var, '\0'))
		{
			if(var.starts_with("SSH_CONNECTION="))
			{
				if(auto peer = parseSshConnection(std::string_view(var).substr(
				       sizeof "SSH_CONNECTION=" - 1)))
				{
					peers.push_back(std::move(*peer));
				}
			}
		}
	}
	return peers;
}

std::unordered_map<std::string, TcpCount>
parseBytesAcked(std::string_view ssOutput)
{
	// A line per socket ("Recv-Q Send-Q local:port peer:port", maybe the
	// state first), its tcp_info indented on the next.
	std::unordered_map<std::string, TcpCount> acked;
	std::string peer;
	std::istringstream in{std::string(ssOutput)};
	std::string line;
	while(std::getline(in, line))
	{
		if(line.empty())
		{
			continue;
		}
		std::istringstream words(line);
		std::string word;
		if(line[0] != ' ' && line[0] != '\t')
		{
			std::string last, beforeLast;
			while(words >> word)
			{
				beforeLast = std::move(last);
				last = std::move(word);
			}
			const size_t colon = last.rfind(':');
			peer = colon == std::string::npos || beforeLast.empty()
			           ? std::string()
			           : sshEndpoint(std::string_view(last).substr(0, colon),
			                         std::string_view(last).substr(colon + 1));
			continue;
		}
		TcpCount count;
		bool hasAcked = false;
		while(!peer.empty() && words >> word)
		{
			if(word.starts_with("bytes_acked:"))
			{
				count.m_acked =
				    std::strtoull(word.c_str() + sizeof "bytes_acked:" - 1,
				                  nullptr,
				                  10);
				hasAcked = true;
			}
			else if(word.starts_with("rtt:")) // rtt:<ms>/<deviation>
			{
				count.m_roundTrip =
				    std::strtod(word.c_str() + sizeof "rtt:" - 1, nullptr) /
				    1e3;
			}
		}
		if(hasAcked)
		{
			acked[peer] = count;
		}
	}
	return acked;
}

std::unordered_map<std::string, TcpCount>
sshBytesAcked(const std::vector<SshPeer>& peers)
{
	std::unordered_map<std::string, TcpCount> acked;
	if(peers.empty())
	{
		return acked;
	}
	std::string ports;
	for(const SshPeer& p : peers)
	{
		const std::string term = "sport = :" + p.m_serverPort;
		if(ports.find(term) == std::string::npos)
		{
			ports += (ports.empty() ? "" : " or ") + term;
		}
	}
	const auto all = parseBytesAcked(run(
	    ("ss -tinH state established '( " + ports + " )' 2>/dev/null").c_str()));
	for(const SshPeer& p : peers)
	{
		if(const auto it = all.find(p.m_client); it != all.end())
		{
			acked.insert(*it);
		}
	}
	return acked;
}

std::optional<TcpCount>
slowestDelivery(const std::unordered_map<std::string, TcpCount>& before,
                const std::unordered_map<std::string, TcpCount>& now)
{
	std::optional<TcpCount> least;
	for(const auto& [peer, n] : now)
	{
		const auto b = before.find(peer);
		if(b == before.end() || n.m_acked < b->second.m_acked)
		{
			continue; // attached since (or the port reused)
		}
		const uint64_t carried = n.m_acked - b->second.m_acked;
		if(!least || carried < least->m_acked)
		{
			least = TcpCount{carried, n.m_roundTrip};
		}
	}
	return least;
}

Transfer autoTransfer(bool tmux, bool sshEnv, bool tmuxSshClient)
{
	// Over ssh the terminal is on another machine and can't see our shared
	// memory or temp files. Inside tmux the pane's SSH_* variables only say
	// how the server was started (over ssh once, attached at the desk now:
	// inline, many times slower); the clients attached say who is watching.
	if(tmux)
	{
		return tmuxSshClient ? Transfer::DIRECT : Transfer::TEMP_FILE;
	}
	return sshEnv ? Transfer::DIRECT : Transfer::SHARED_MEMORY;
}

TerminalReply parseTerminalReply(std::string_view in)
{
	TerminalReply r;
	const size_t da = deviceAttributesEnd(in);
	r.m_answered = da != std::string_view::npos;
	in = in.substr(0, da);
	const std::string ok = "\x1b_Gi=" + std::string(QUERY_ID) + ";OK";
	r.m_kittyGraphics = in.find(ok) != std::string_view::npos;
	// XTVERSION: DCS > | name ST.
	if(const size_t at = in.find("\x1bP>|"); at != std::string_view::npos)
	{
		const size_t end = in.find("\x1b\\", at);
		if(end != std::string_view::npos)
		{
			r.m_version = in.substr(at + 4, end - at - 4);
		}
	}
	return r;
}

TerminalReply queryTerminal()
{
	static const TerminalReply REPLY = []
	{
		using Clock = std::chrono::steady_clock;
		using std::chrono::milliseconds;
		// The terminal itself, even with stdout piped (--doctor | less).
		const int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
		termios old{};
		if(tty < 0 || tcgetattr(tty, &old) != 0)
		{
			if(tty >= 0)
			{
				close(tty);
			}
			return TerminalReply{};
		}
		termios raw = old;
		raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(tty, TCSANOW, &raw);
		// A 1x1 image to query (a=q stores nothing), XTVERSION, then DA1:
		// terminals answer in order, so DA1's answer closes the others'.
		const std::string query = "\x1b_Gi=" + std::string(QUERY_ID) +
		                          ",s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"
		                          "\x1b[>q\x1b[c";
		for(size_t off = 0; off < query.size();)
		{
			const ssize_t n =
			    write(tty, query.data() + off, query.size() - off);
			if(n <= 0)
			{
				break;
			}
			off += static_cast<size_t>(n);
		}
		std::string in;
		const auto deadline = Clock::now() + milliseconds(2000);
		while(deviceAttributesEnd(in) == std::string::npos)
		{
			const auto left = std::chrono::duration_cast<milliseconds>(
			    deadline - Clock::now());
			// select, not poll: macOS's poll can't wait on /dev/tty.
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(tty, &fds);
			timeval tv{.tv_sec = static_cast<time_t>(left.count() / 1000),
			           .tv_usec = static_cast<suseconds_t>(left.count() % 1000 *
			                                               1000)};
			if(left.count() <= 0 ||
			   select(tty + 1, &fds, nullptr, nullptr, &tv) <= 0)
			{
				break;
			}
			char buf[256];
			const ssize_t n = read(tty, buf, sizeof buf);
			if(n <= 0)
			{
				break;
			}
			in.append(buf, static_cast<size_t>(n));
		}
		tcsetattr(tty, TCSANOW, &old);
		close(tty);
		return parseTerminalReply(in);
	}();
	return REPLY;
}

bool isOuterTmux(bool tmuxEnv, const std::string& term)
{
	return !tmuxEnv && (term.starts_with("tmux") || term.starts_with("screen"));
}

std::string terminalHint(bool tmux)
{
	if(tmux)
	{
		// "NAME=value"; "-NAME" when an attach found it unset; nothing when
		// the session never had it.
		const std::string line =
		    run("tmux show-environment LC_TERMINAL 2>/dev/null");
		if(line.starts_with("LC_TERMINAL="))
		{
			return line.substr(sizeof "LC_TERMINAL=" - 1);
		}
		if(line == "-LC_TERMINAL")
		{
			return "";
		}
	}
	return env("LC_TERMINAL");
}

bool isKittyTerminal(const std::string& name)
{
	std::string lower = name;
	for(char& c : lower)
	{
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return lower.find("kitty") != std::string::npos ||
	       lower.find("ghostty") != std::string::npos;
}

const char* graphicsModeName(GraphicsMode m)
{
	return m == GraphicsMode::KITTY ? "kitty" : "halfblock";
}

const char* transferName(Transfer t)
{
	switch(t)
	{
		case Transfer::SHARED_MEMORY:
			return "shm";
		case Transfer::TEMP_FILE:
			return "file";
		case Transfer::DIRECT:
			break;
	}
	return "direct";
}

bool refreshCellSize(TermCaps& caps)
{
	struct winsize ws{};
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 ||
	   ws.ws_row == 0)
	{
		return false;
	}
	int w = 0, h = 0;
	if(caps.m_tmux)
	{
		// Inside tmux the pane's pixel size is tmux's estimate, not the outer
		// terminal's; the client's cell size is what Ghostty/kitty actually
		// draw with. Ask again only when the pane is resized (popen is slow).
		static unsigned short lastCols = 0, lastRows = 0;
		static int tmuxW = 0, tmuxH = 0;
		if(ws.ws_col != lastCols || ws.ws_row != lastRows)
		{
			lastCols = ws.ws_col;
			lastRows = ws.ws_row;
			// Quiet when tmux can't answer (its socket in /tmp removed): it
			// would print "error connecting to …" over the terminal.
			std::string out =
			    tmuxDisplay("#{client_cell_width} #{client_cell_height}");
			if(std::sscanf(out.c_str(), "%d %d", &tmuxW, &tmuxH) != 2)
			{
				tmuxW = tmuxH = 0;
			}
		}
		w = tmuxW;
		h = tmuxH;
	}
	if((w <= 0 || h <= 0) && ws.ws_xpixel && ws.ws_ypixel)
	{
		w = ws.ws_xpixel / ws.ws_col;
		h = ws.ws_ypixel / ws.ws_row;
	}
	if(w <= 0 || h <= 0 || (w == caps.m_cellW && h == caps.m_cellH))
	{
		return false;
	}
	caps.m_cellW = w;
	caps.m_cellH = h;
	return true;
}

TermCaps detectCaps(const std::string& forced, const std::string& transfer)
{
	TermCaps caps;
	caps.m_tmux = !env("TMUX").empty();
	std::string term = env("TERM"), prog = env("TERM_PROGRAM");
	caps.m_terminalName = !prog.empty() ? prog : term;
	// In tmux the pane's environment may not say (a server started from
	// elsewhere, e.g. over ssh); the attached client's terminal does. When
	// that client is a tmux too, ssh'd here from one of its panes, there's
	// another tmux to pass through.
	const std::string client =
	    caps.m_tmux ? tmuxDisplay("#{client_termname}") : "";
	caps.m_outerTmux = isOuterTmux(caps.m_tmux, term) ||
	                   (caps.m_tmux && isOuterTmux(false, client));

	// Terminals implementing kitty graphics *with Unicode placeholders*.
	bool kitty = isKittyTerminal(term) || isKittyTerminal(prog) ||
	             !env("KITTY_WINDOW_ID").empty() ||
	             !env("GHOSTTY_RESOURCES_DIR").empty() ||
	             isKittyTerminal(client);
	// Behind a tmux beyond ssh only LC_TERMINAL can say (ssh sends LC_* by
	// default); tmux drops the terminal's answers to queries.
	if(!kitty && caps.m_outerTmux)
	{
		kitty = isKittyTerminal(terminalHint(caps.m_tmux));
	}
	// Anywhere else the terminal is asked, and its answer beats the
	// environment, which multiplexers (zellij) pass on from a terminal that
	// draws images to a pane that doesn't. Kitty graphics alone isn't
	// enough: the images are Unicode placeholders, which WezTerm lacks.
	if(!caps.m_tmux && !caps.m_outerTmux && forced == "auto")
	{
		const TerminalReply r = queryTerminal();
		if(r.m_answered)
		{
			kitty = r.m_kittyGraphics &&
			        (r.m_version.empty() || isKittyTerminal(r.m_version));
			if(!r.m_version.empty())
			{
				caps.m_terminalName = r.m_version;
			}
		}
	}
	if(forced == "kitty")
	{
		kitty = true;
	}
	else if(forced == "halfblock")
	{
		kitty = false;
	}
	// tmux drops kitty graphics escapes unless allow-passthrough is on; the
	// placeholder cells would then render blank, so fall back instead.
	if(kitty && caps.m_tmux && forced != "kitty")
	{
		std::string pt = tmuxOption("allow-passthrough");
		if(pt != "on" && pt != "all")
		{
			kitty = false;
			caps.m_note = "tmux passthrough off → half-blocks (--doctor)";
		}
	}
	caps.m_graphics = kitty ? GraphicsMode::KITTY : GraphicsMode::HALF_BLOCK;
	if(transfer == "shm" || transfer == "file" || transfer == "direct")
	{
		caps.m_transfer = transfer == "shm"    ? Transfer::SHARED_MEMORY
		                  : transfer == "file" ? Transfer::TEMP_FILE
		                                       : Transfer::DIRECT;
	}
	else
	{
		caps.m_transfer =
		    autoTransfer(caps.m_tmux,
		                 !env("SSH_CONNECTION").empty() ||
		                     !env("SSH_TTY").empty(),
		                 kitty && caps.m_tmux && tmuxClientOverSsh());
	}
	refreshCellSize(caps);
	return caps;
}

} // namespace rega
