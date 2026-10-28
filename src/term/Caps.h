#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rega
{

/// How images are drawn.
enum class GraphicsMode
{
	KITTY,     ///< kitty graphics with Unicode placeholders
	HALF_BLOCK ///< ▀ cells, two pixels each
};

/// How kitty image pixels reach the terminal.
enum class Transfer
{
	DIRECT,        ///< in the escapes: zlib + base64 through the pty (ssh)
	SHARED_MEMORY, ///< t=s: raw RGBA in POSIX shared memory, the name in the pipe
	/// t=f: raw RGBA in a temp file. For tmux: every attached client reads
	/// it, while the first to read shared memory unlinks it and leaves the
	/// other clients blank.
	TEMP_FILE
};

/// What the terminal can do, from detectCaps().
struct TermCaps
{
	GraphicsMode m_graphics = GraphicsMode::HALF_BLOCK;
	Transfer m_transfer = Transfer::DIRECT;
	/// Bumped to send every picture again: tmux doesn't replay them to a
	/// terminal that attaches later.
	int m_resend = 0;
	bool m_tmux = false;
	/// A tmux beyond ssh: TERM, or in tmux its client's terminal, names tmux
	/// (see isOuterTmux()). Escapes still go through its passthrough, but
	/// `tmux` commands can't reach it, nor terminal answers get back.
	bool m_outerTmux = false;
	int m_cellW = 10, m_cellH = 20; ///< pixels per cell
	std::string m_terminalName;
	/// A misconfiguration worth a lasting warning (e.g. kitty graphics off
	/// because tmux has no allow-passthrough). Short; `rega --doctor` has the
	/// details. Empty when nothing to report.
	std::string m_note;

	/// The tmuxes escapes pass through, each wrapping them once: ours, one
	/// beyond ssh, or both (tmux on a machine ssh'd to from a tmux pane).
	[[nodiscard]] int tmuxLayers() const noexcept
	{
		return int(m_tmux) + int(m_outerTmux);
	}
};

/// `tmux display -p <format>` for the current client ("" on failure).
std::string tmuxDisplay(const std::string& format);
/// A tmux option as the current pane sees it (`tmux show -Apv <name>`).
std::string tmuxOption(const std::string& name);
/// Whether process `pid` descends from sshd (an ssh login), by `psTable`:
/// `ps -A -o pid=,ppid=,comm=` output.
bool sshAncestor(int pid, const std::string& psTable);
/// Whether a client attached to this pane's tmux session came in over ssh.
/// The pane's own environment can't tell when the server was started on
/// this machine (its desktop) and then also attached to from elsewhere: that
/// client's terminal can't read our temp files or shared memory.
bool tmuxClientOverSsh();
/// The pids of the clients attached to this pane's tmux session, one a line.
std::string tmuxClientPids();
/// tmuxClientOverSsh() for these tmuxClientPids().
bool tmuxClientOverSsh(const std::string& pids);
/// One end of an ssh connection, as "address:port" (IPv6 unbracketed,
/// IPv4-mapped as plain IPv4), so SSH_CONNECTION and `ss` compare.
std::string sshEndpoint(std::string_view address, std::string_view port);
/// An SSH_CONNECTION value ("client_ip client_port server_ip server_port"):
/// the client's sshEndpoint and the server's port.
struct SshPeer
{
	std::string m_client;
	std::string m_serverPort;
};
std::optional<SshPeer> parseSshConnection(std::string_view value);
/// Variable `name` in the environment of each client attached to this pane's
/// tmux session (/proc: Linux only; none elsewhere), one value per client
/// that has it.
std::vector<std::string> tmuxClientEnv(const std::string& name);
/// The ssh connections the clients attached to this pane's tmux session came
/// in over: SSH_CONNECTION from each client's environment (/proc: Linux
/// only; none elsewhere, or for a client attached at the desk).
std::vector<SshPeer> tmuxSshPeers();
/// A TCP connection as `ss -ti` shows it: bytes the far end acknowledged,
/// and the smoothed round trip in seconds (0 not shown).
struct TcpCount
{
	uint64_t m_acked = 0;
	double m_roundTrip = 0;
};
/// From `ss -tinH` output: per connection with bytes_acked, by the peer's
/// sshEndpoint.
std::unordered_map<std::string, TcpCount>
parseBytesAcked(std::string_view ssOutput);
/// What the far end of each of `peers`' connections has acknowledged so far:
/// bytes that crossed the network, unlike what tmux counts as written to a
/// client (#{client_written}: what it buffered, however far behind). By
/// SshPeer::m_client; a connection `ss` doesn't show is left out.
std::unordered_map<std::string, TcpCount>
sshBytesAcked(const std::vector<SshPeer>& peers);
/// What the slowest connection carried between two sshBytesAcked() (in
/// m_acked), with its round trip now; nullopt when none is in both.
std::optional<TcpCount>
slowestDelivery(const std::unordered_map<std::string, TcpCount>& before,
                const std::unordered_map<std::string, TcpCount>& now);
/// What `--transfer auto` picks. `sshEnv`: SSH_CONNECTION / SSH_TTY set;
/// `tmuxSshClient`: tmuxClientOverSsh(). In tmux only the clients count:
/// direct with one over ssh, else temp files (each terminal attached reads
/// them; shared memory goes once read). Outside it: direct over ssh, else
/// shared memory.
Transfer autoTransfer(bool tmux, bool sshEnv, bool tmuxSshClient);
/// A TERM / TERM_PROGRAM / LC_TERMINAL value of a terminal with kitty graphics
/// + Unicode placeholders (ghostty, kitty).
bool isKittyTerminal(const std::string& name);

/// TERM names tmux (or screen, tmux's old default) while $TMUX is unset: ssh
/// from a tmux pane carries TERM across but not TMUX, so the tmux the
/// terminal sits behind is on the other machine.
bool isOuterTmux(bool tmuxEnv, const std::string& term);
/// LC_TERMINAL as it stands: in tmux the session's, which `update-environment`
/// refreshes on every attach (the pane's own is from when it started), else
/// ours. ssh carries it by default, so it names the terminal behind a tmux
/// beyond ssh (an `ssh` wrapper sets it there).
std::string terminalHint(bool tmux);

/// What the terminal said to queryTerminal().
struct TerminalReply
{
	/// It answered DA1, which every terminal does, after the other queries:
	/// whatever it didn't answer by then it never will.
	bool m_answered = false;
	bool m_kittyGraphics = false; ///< it answered the kitty graphics query OK
	std::string m_version; ///< XTVERSION, e.g. "ghostty 1.2.0"; "" if none
};
/// Parse what came back for queryTerminal()'s queries.
TerminalReply parseTerminalReply(std::string_view in);
/// Ask the terminal on /dev/tty for kitty graphics, XTVERSION and DA1, once
/// (later calls reuse the answer), before the UI reads from it. Unanswered
/// (m_answered false) without a terminal, or after 2 s.
TerminalReply queryTerminal();

/// `forced` is "auto" | "kitty" | "halfblock". `transfer` is "auto" | "direct"
/// | "shm" | "file"; auto: see autoTransfer().
TermCaps detectCaps(const std::string& forced = "auto",
                    const std::string& transfer = "auto");

/// Re-read the cell pixel size (call on resize / font change). Returns true if
/// changed.
bool refreshCellSize(TermCaps& caps);

const char* graphicsModeName(GraphicsMode m); ///< "kitty" | "halfblock"
const char* transferName(Transfer t);         ///< "direct" | "shm" | "file"

} // namespace rega
