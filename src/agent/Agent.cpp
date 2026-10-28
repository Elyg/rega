#include "agent/Agent.h"

#include "agent/Protocol.h"
#include "util/Log.h"

#include <spdlog/fmt/ranges.h>
#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <random>
#include <stb_image.h>
#include <unistd.h>

namespace rega::agent
{

namespace
{
// macOS: PSHMNAMLEN.
constexpr size_t MAX_SHM_NAME = 31;
} // namespace

ShmStore::ShmStore(std::string token, uint64_t budget)
    : m_token(std::move(token)), m_budget(budget)
{
}

ShmStore::~ShmStore()
{
	clear();
}

std::string ShmStore::nameFor(uint32_t key, uint32_t gen) const
{
	return "/re" + m_token + "-" + std::to_string(key) + "-" +
	       std::to_string(gen);
}

bool ShmStore::create(Frame& f,
                      const std::string& name,
                      const void* src,
                      size_t n)
{
	if(name.size() > MAX_SHM_NAME || n == 0)
	{
		return false;
	}
	const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
	if(fd < 0)
	{
		return false;
	}
	void* p = MAP_FAILED;
	if(ftruncate(fd, static_cast<off_t>(n)) == 0)
	{
		p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}
	close(fd);
	if(p == MAP_FAILED)
	{
		shm_unlink(name.c_str());
		return false;
	}
	std::memcpy(p, src, n);
	f.m_map = p;
	f.m_size = n;
	f.m_name = name;
	// Locked after the copy: every page is there to lock.
	f.m_locked = mlock(p, n) == 0;
	return true;
}

void ShmStore::release(Frame& f)
{
	if(!f.m_name.empty())
	{
		shm_unlink(f.m_name.c_str()); // ENOENT: the terminal read it
	}
	if(f.m_map)
	{
		if(f.m_locked)
		{
			munlock(f.m_map, f.m_size);
		}
		else
		{
			m_unlocked -= f.m_size;
		}
		munmap(f.m_map, f.m_size);
		m_bytes -= f.m_size;
	}
	f.m_map = nullptr;
	f.m_name.clear();
}

ShmStore::Put ShmStore::put(uint32_t key, const uint8_t* rgba, size_t n)
{
	Put out;
	uint32_t gen = 0;
	if(auto it = m_frames.find(key); it != m_frames.end())
	{
		gen = it->second.m_gen + 1;
		drop(key);
	}
	Frame f;
	// O_EXCL: a name left from before (a crash, SIGKILL) is skipped.
	for(int tries = 0; tries < 8 && !f.m_map; ++tries, ++gen)
	{
		create(f, nameFor(key, gen), rgba, n);
	}
	if(!f.m_map)
	{
		return out;
	}
	f.m_gen = gen - 1;
	m_bytes += f.m_size;
	if(!f.m_locked)
	{
		m_unlocked += f.m_size;
	}
	m_lru.push_back(key);
	f.m_lru = std::prev(m_lru.end());
	out.m_name = f.m_name;
	m_frames.emplace(key, std::move(f));
	while(m_budget > 0 && m_bytes > m_budget && m_lru.front() != key)
	{
		const uint32_t old = m_lru.front();
		drop(old);
		out.m_dropped.push_back(old);
	}
	return out;
}

std::string ShmStore::name(uint32_t key) const
{
	const auto it = m_frames.find(key);
	return it == m_frames.end() ? std::string() : it->second.m_name;
}

void ShmStore::watch(uint32_t key)
{
	if(m_frames.contains(key))
	{
		m_watched.insert_or_assign(key,
		                           Watch{std::chrono::steady_clock::now()});
	}
}

std::vector<std::pair<uint32_t, std::string>> ShmStore::rearm()
{
	std::vector<std::pair<uint32_t, std::string>> out;
	for(auto w = m_watched.begin(); w != m_watched.end();)
	{
		const auto it = m_frames.find(w->first);
		if(it == m_frames.end())
		{
			w = m_watched.erase(w);
			continue;
		}
		Frame& f = it->second;
		const int fd = shm_open(f.m_name.c_str(), O_RDONLY, 0);
		if(fd >= 0 || errno != ENOENT)
		{
			if(fd >= 0)
			{
				close(fd);
			}
			// What the remote calls stale: shown, never read (see
			// AgentClient::STALE_AFTER).
			if(!w->second.m_logged &&
			   std::chrono::steady_clock::now() - w->second.m_since >
			       std::chrono::seconds(1))
			{
				w->second.m_logged = true;
				spdlog::info("agent: {} shown, not read after 1 s", f.m_name);
			}
			++w; // not read yet
			continue;
		}
		Frame next;
		uint32_t gen = f.m_gen + 1;
		for(int tries = 0; tries < 8 && !next.m_map; ++tries, ++gen)
		{
			create(next, nameFor(it->first, gen), f.m_map, f.m_size);
		}
		const uint32_t key = it->first;
		w = m_watched.erase(w);
		if(!next.m_map)
		{
			drop(key);
			out.emplace_back(key, std::string());
			continue;
		}
		f.m_name.clear(); // gone already
		const bool wasLocked = f.m_locked;
		if(wasLocked)
		{
			munlock(f.m_map, f.m_size);
		}
		munmap(f.m_map, f.m_size);
		if(wasLocked != next.m_locked)
		{
			m_unlocked =
			    next.m_locked ? m_unlocked - f.m_size : m_unlocked + f.m_size;
		}
		f.m_map = next.m_map;
		f.m_locked = next.m_locked;
		f.m_name = next.m_name;
		f.m_gen = gen - 1;
		m_lru.splice(m_lru.end(), m_lru, f.m_lru);
		out.emplace_back(key, f.m_name);
	}
	return out;
}

void ShmStore::drop(uint32_t key)
{
	const auto it = m_frames.find(key);
	if(it == m_frames.end())
	{
		return;
	}
	release(it->second);
	m_lru.erase(it->second.m_lru);
	m_watched.erase(key);
	m_frames.erase(it);
}

void ShmStore::clear()
{
	for(auto& [key, f] : m_frames)
	{
		release(f);
	}
	m_frames.clear();
	m_lru.clear();
	m_watched.clear();
}

std::vector<std::string> sshCommand(const std::vector<std::string>& command,
                                    const std::string& remoteSocket,
                                    const std::string& localSocket)
{
	if(command.empty())
	{
		return {};
	}
	// Options before the destination: right after the program.
	std::vector<std::string> out{command.front(),
	                             "-R",
	                             remoteSocket + ":" + localSocket,
	                             "-o",
	                             "SetEnv=LC_REGA_AGENT=" + remoteSocket,
	                             "-o",
	                             "SendEnv=LC_REGA_AGENT"};
	out.insert(out.end(), command.begin() + 1, command.end());
	return out;
}

std::string shellInit(const std::string& shell, const std::string& rega)
{
	if(shell != "bash" && shell != "zsh")
	{
		return {};
	}
	// Single-quoted: any ' in the path closed, escaped, reopened.
	std::string quoted = "'";
	for(const char c : rega)
	{
		quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
	}
	quoted += "'";
	// Only ssh typed at a terminal: scp, rsync, git and scripts start ssh
	// themselves, and a pipe has no terminal to show pictures on. ssh's
	// path comes from PATH again, past any function (exec sees none). An
	// ssh() of the user's own stays: it calls rega_ssh instead.
	return "# rega agent mode: an ssh from this shell runs under rega "
	       "--agent, which\n"
	       "# keeps the pictures rega plays at the far end here, in RAM.\n"
	       "# An ssh() of your own: call rega_ssh in it, not command ssh.\n"
	       "rega_ssh() {\n"
	       "\tif [ -t 0 ] && [ -t 1 ] && [ -z \"${REGA_NO_AGENT:-}\" ] && "
	       "[ -x " +
	       quoted +
	       " ]; then\n"
	       "\t\t" +
	       quoted +
	       " --agent -- ssh \"$@\"\n"
	       "\telse\n"
	       "\t\tcommand ssh \"$@\"\n"
	       "\tfi\n"
	       "}\n"
	       "typeset -f ssh >/dev/null 2>&1 || ssh() { rega_ssh \"$@\"; }\n";
}

namespace
{

int g_signalPipe[2] = {-1, -1};

void onSignal(int sig)
{
	const int saved = errno;
	const auto b = static_cast<unsigned char>(sig);
	(void)!::write(g_signalPipe[1], &b, 1);
	errno = saved;
}

void setFlags(int fd, int fdFlags, int flFlags)
{
	if(fdFlags)
	{
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | fdFlags);
	}
	if(flFlags)
	{
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | flFlags);
	}
}

std::string randomToken(size_t n)
{
	static constexpr char ALPHABET[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	std::random_device rd;
	std::string s;
	for(size_t i = 0; i < n; ++i)
	{
		s += ALPHABET[rd() % (sizeof ALPHABET - 1)];
	}
	return s;
}

std::string localSocketPath()
{
	const char* tmp = std::getenv("TMPDIR");
	std::string dir = tmp && *tmp ? tmp : "/tmp";
	while(dir.size() > 1 && dir.back() == '/')
	{
		dir.pop_back();
	}
	std::string path =
	    dir + "/rega-agent-" + std::to_string(getpid()) + ".sock";
	if(path.size() >= sizeof(sockaddr_un{}.sun_path))
	{
		path = "/tmp/rega-agent-" + std::to_string(getpid()) + ".sock";
	}
	return path;
}

// Unique: sshd's StreamLocalBindUnlink is unknown, and the ControlMaster
// keeps old forwards bound.
std::string remoteSocketPath()
{
	const char* user = std::getenv("USER");
	return "/tmp/rega-agent-" + std::string(user && *user ? user : "user") +
	       "-" + randomToken(10) + ".sock";
}

int listenOn(const std::string& path)
{
	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd < 0)
	{
		return -1;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
	unlink(path.c_str());
	if(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
	   listen(fd, 8) != 0)
	{
		close(fd);
		return -1;
	}
	chmod(path.c_str(), 0600);
	setFlags(fd, FD_CLOEXEC, O_NONBLOCK);
	return fd;
}

struct Stats
{
	size_t m_connections = 0, m_frames = 0, m_rearms = 0, m_drops = 0,
	       m_failed = 0;
	uint64_t m_bytesIn = 0, m_peakHeld = 0, m_peakUnlocked = 0;
	double m_decodeMs = 0;
};

// One remote rega: its frames live as long as it stays connected.
struct Connection
{
	int m_fd = -1;
	Reader m_in;
	std::string m_out; ///< not written yet
	std::unique_ptr<ShmStore> m_store;
	bool m_hello = false, m_closing = false;
	std::vector<std::string> m_created; ///< every name made, for the check

	void send(const Message& m)
	{
		m_out += encode(m);
	}
	bool flush()
	{
		while(!m_out.empty())
		{
			const ssize_t n = ::write(m_fd, m_out.data(), m_out.size());
			if(n < 0 && errno == EINTR)
			{
				continue;
			}
			if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				return true;
			}
			if(n <= 0)
			{
				return false;
			}
			m_out.erase(0, static_cast<size_t>(n));
		}
		return true;
	}
};

void handleFrame(Connection& c, Message& m, Stats& stats)
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::now();
	const size_t want = size_t(m.m_width) * m.m_height * 4;
	stats.m_bytesIn += m.m_bytes.size();
	ShmStore::Put put;
	if(m.m_codec == Codec::RAW)
	{
		if(m.m_bytes.size() == want && want > 0)
		{
			put = c.m_store->put(m.m_key,
			                     reinterpret_cast<const uint8_t*>(
			                         m.m_bytes.data()),
			                     want);
		}
	}
	else
	{
		int w = 0, h = 0, comp = 0;
		stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(
		                                        m.m_bytes.data()),
		                                    static_cast<int>(m.m_bytes.size()),
		                                    &w,
		                                    &h,
		                                    &comp,
		                                    4);
		if(px && uint32_t(w) == m.m_width && uint32_t(h) == m.m_height)
		{
			put = c.m_store->put(m.m_key, px, want);
		}
		stbi_image_free(px);
	}
	stats.m_decodeMs +=
	    std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
	for(const uint32_t key : put.m_dropped)
	{
		Message drop;
		drop.m_type = Type::DROP;
		drop.m_key = key;
		c.send(drop);
		++stats.m_drops;
	}
	if(put.m_name.empty())
	{
		spdlog::warn("agent: frame {} ({}x{} {}) not stored",
		             m.m_key,
		             m.m_width,
		             m.m_height,
		             codecName(m.m_codec));
		++stats.m_failed;
		Message drop;
		drop.m_type = Type::DROP;
		drop.m_key = m.m_key;
		c.send(drop);
		return;
	}
	++stats.m_frames;
	c.m_created.push_back(put.m_name);
	Message ready;
	ready.m_type = Type::READY;
	ready.m_key = m.m_key;
	ready.m_name = put.m_name;
	ready.m_size = want;
	c.send(ready);
}

void handleMessage(Connection& c, Message& m, Stats& stats)
{
	if(!c.m_hello)
	{
		if(m.m_type != Type::HELLO || m.m_version != PROTOCOL_VERSION)
		{
			spdlog::warn("agent: no HELLO v{} from the remote; closing",
			             PROTOCOL_VERSION);
			Message bye;
			bye.m_type = Type::BYE;
			c.send(bye);
			c.m_closing = true;
			return;
		}
		c.m_hello = true;
		Message hello;
		hello.m_type = Type::HELLO;
		hello.m_version = PROTOCOL_VERSION;
		hello.m_token = c.m_store->token();
		c.send(hello);
		return;
	}
	switch(m.m_type)
	{
		case Type::FRAME:
			handleFrame(c, m, stats);
			break;
		case Type::SHOWN:
			c.m_store->watch(m.m_key);
			break;
		case Type::DROP: // the remote is done with it
			c.m_store->drop(m.m_key);
			break;
		case Type::BYE:
			c.m_closing = true;
			break;
		default:
			break;
	}
}

// Names made that still exist: the terminal unlinks what it reads and
// clear() the rest, so anything here leaked (E6).
size_t leftover(const std::vector<std::string>& names)
{
	size_t n = 0;
	for(const auto& name : names)
	{
		const int fd = shm_open(name.c_str(), O_RDONLY, 0);
		if(fd >= 0)
		{
			close(fd);
			++n;
		}
	}
	return n;
}

} // namespace

int runAgent(const AgentOptions& opt)
{
	if(opt.m_command.empty())
	{
		spdlog::error("--agent: give the ssh command after --");
		return 2;
	}
	if(pipe(g_signalPipe) != 0)
	{
		spdlog::error("agent: pipe: {}", std::strerror(errno));
		return 1;
	}
	for(const int fd : g_signalPipe)
	{
		setFlags(fd, FD_CLOEXEC, O_NONBLOCK);
	}
	const std::string local = localSocketPath();
	const int listenFd = listenOn(local);
	if(listenFd < 0)
	{
		spdlog::error("agent: cannot listen on {}: {}",
		              local,
		              std::strerror(errno));
		return 1;
	}
	const std::string remote = remoteSocketPath();
	const std::vector<std::string> command =
	    sshCommand(opt.m_command, remote, local);

	struct sigaction sa{};
	sa.sa_handler = onSignal;
	sigemptyset(&sa.sa_mask);
	for(const int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP})
	{
		sigaction(sig, &sa, nullptr);
	}
	// A remote gone mid-reply: write() fails instead.
	signal(SIGPIPE, SIG_IGN);
	// From here the terminal is ssh's: log to the file only.
	rega::log::redirectToFile(opt.m_log);
	spdlog::info("agent: {} ← {}; running {}",
	             local,
	             remote,
	             fmt::join(command, " "));

	const pid_t child = fork();
	if(child == 0)
	{
		struct sigaction dfl{};
		dfl.sa_handler = SIG_DFL;
		for(const int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE})
		{
			sigaction(sig, &dfl, nullptr);
		}
		// For SendEnv too: a ControlMaster mux client may not pass SetEnv on.
		setenv("LC_REGA_AGENT", remote.c_str(), 1);
		std::vector<char*> argv;
		for(const auto& a : command)
		{
			argv.push_back(const_cast<char*>(a.c_str()));
		}
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	if(child < 0)
	{
		close(listenFd);
		unlink(local.c_str());
		fmt::print(stderr, "agent: fork: {}\n", std::strerror(errno));
		return 1;
	}

	Stats stats;
	std::vector<std::unique_ptr<Connection>> conns;
	std::vector<std::string> created;
	int status = 0, signalled = 0;
	bool childDone = false;
	auto closeConn = [&](Connection& c)
	{
		c.m_store->clear();
		created.insert(created.end(), c.m_created.begin(), c.m_created.end());
		close(c.m_fd);
		c.m_fd = -1;
		spdlog::info("agent: remote disconnected");
	};
	while(!childDone && !signalled)
	{
		std::vector<pollfd> fds{{g_signalPipe[0], POLLIN, 0},
		                        {listenFd, POLLIN, 0}};
		bool watching = false;
		for(const auto& c : conns)
		{
			fds.push_back(
			    {c->m_fd, short(POLLIN | (c->m_out.empty() ? 0 : POLLOUT)), 0});
			watching = watching || c->m_store->watching();
		}
		// Watched names are checked every 2 ms: re-arm latency.
		if(poll(fds.data(), fds.size(), watching ? 2 : -1) < 0 &&
		   errno != EINTR)
		{
			spdlog::error("agent: poll: {}", std::strerror(errno));
			break;
		}
		if(fds[0].revents & POLLIN)
		{
			unsigned char sig = 0;
			while(::read(g_signalPipe[0], &sig, 1) == 1)
			{
				if(sig != SIGCHLD)
				{
					signalled = sig;
				}
			}
			if(waitpid(child, &status, WNOHANG) == child)
			{
				childDone = true;
			}
		}
		const size_t polled = conns.size(); // accepted ones have no revents
		if(fds[1].revents & POLLIN)
		{
			for(int fd; (fd = accept(listenFd, nullptr, nullptr)) >= 0;)
			{
				setFlags(fd, FD_CLOEXEC, O_NONBLOCK);
				auto c = std::make_unique<Connection>();
				c->m_fd = fd;
				c->m_store =
				    std::make_unique<ShmStore>(randomToken(6), opt.m_budget);
				conns.push_back(std::move(c));
				++stats.m_connections;
				spdlog::info("agent: remote connected");
			}
		}
		for(size_t i = 0; i < conns.size(); ++i)
		{
			Connection& c = *conns[i];
			const short ev = i < polled ? fds[2 + i].revents : 0;
			bool open = true;
			if(ev & (POLLIN | POLLHUP | POLLERR))
			{
				open = c.m_in.fill(c.m_fd);
				while(auto m = c.m_in.next())
				{
					handleMessage(c, *m, stats);
				}
				open = open && !c.m_in.failed();
			}
			for(const auto& [key, name] : c.m_store->rearm())
			{
				if(name.empty())
				{
					Message drop;
					drop.m_type = Type::DROP;
					drop.m_key = key;
					c.send(drop);
					++stats.m_drops;
					continue;
				}
				Message ready;
				ready.m_type = Type::READY;
				ready.m_key = key;
				ready.m_name = name;
				ready.m_size = 0; // unchanged
				c.send(ready);
				c.m_created.push_back(name);
				++stats.m_rearms;
			}
			stats.m_peakHeld = std::max(stats.m_peakHeld, c.m_store->bytes());
			stats.m_peakUnlocked =
			    std::max(stats.m_peakUnlocked, c.m_store->unlockedBytes());
			open = c.flush() && open;
			if(!open || (c.m_closing && c.m_out.empty()))
			{
				closeConn(c);
			}
		}
		std::erase_if(conns, [](const auto& c) { return c->m_fd < 0; });
	}
	for(auto& c : conns)
	{
		closeConn(*c);
	}
	close(listenFd);
	unlink(local.c_str());
	if(signalled && !childDone)
	{
		kill(child, SIGTERM);
		waitpid(child, &status, 0);
	}
	const size_t left = leftover(created);
	spdlog::info("agent: done; {} shm names left", left);
	auto mb = [](uint64_t b) { return double(b) / (1024.0 * 1024.0); };
	// Not fmt::print: it throws once the terminal is gone (SIGHUP).
	const std::string summary = fmt::format(
	    "rega agent: {} connection(s), {} frames ({:.1f} MB in, "
	    "decode {:.1f} ms each), {} re-armed, {} dropped, {} failed; "
	    "peak {:.1f} MB held ({:.1f} MB not mlock'd); {} shm left\n",
	    stats.m_connections,
	    stats.m_frames,
	    mb(stats.m_bytesIn),
	    stats.m_frames ? stats.m_decodeMs / double(stats.m_frames) : 0.0,
	    stats.m_rearms,
	    stats.m_drops,
	    stats.m_failed,
	    mb(stats.m_peakHeld),
	    mb(stats.m_peakUnlocked),
	    left);
	// Every ssh runs under the agent (--shell-init): quiet unless it served.
	if(stats.m_frames > 0 || left > 0)
	{
		std::fputs(summary.c_str(), stderr);
	}
	if(signalled)
	{
		return 128 + signalled;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace rega::agent
