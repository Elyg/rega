#include "util/Log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <unistd.h>

namespace rega::log
{

spdlog::logger& out()
{
	static auto logger = []
	{
		auto l = spdlog::stdout_logger_mt("out");
		l->set_pattern("%v");
		return l;
	}();
	return *logger;
}

void initConsole()
{
	auto l = spdlog::stderr_logger_mt("rega");
	l->set_pattern("rega: %^%l%$: %v");
	spdlog::set_default_logger(l);
}

void redirectToFile(std::string file)
{
	if(file.empty())
	{
		if(const char* env = std::getenv("REGA_LOG"))
		{
			file = env;
		}
		else if(const char* home = std::getenv("HOME"))
		{
			file = std::string(home) + "/.cache/rega/rega.log";
		}
		else
		{
			file = "/tmp/rega.log";
		}
	}
	std::error_code ec;
	std::filesystem::create_directories(
	    std::filesystem::path(file).parent_path(), ec);
	try
	{
		// Kept across runs (each line carries the pid), capped at 2 x 2 MB.
		auto l =
		    spdlog::rotating_logger_mt("rega-file", file, 2 * 1024 * 1024, 1);
		l->set_pattern("[%m-%d %H:%M:%S.%e] [%l] [%P] %v"); // %P: pid per run
		l->flush_on(spdlog::level::info);
		// REGA_LOG_LEVEL=debug for per-frame detail.
		if(const char* lvl = std::getenv("REGA_LOG_LEVEL"))
		{
			l->set_level(spdlog::level::from_str(lvl));
			l->flush_on(l->level());
		}
		spdlog::set_default_logger(l);
	}
	catch(const spdlog::spdlog_ex&)
	{
		spdlog::set_level(spdlog::level::off); // never write onto the TUI
	}
}

namespace
{

void logLine(std::string line)
{
	if(!line.empty() && line.back() == '\r')
	{
		line.pop_back();
	}
	if(!line.empty())
	{
		spdlog::warn("stderr: {}", line);
	}
}

} // namespace

StderrToLog::StderrToLog()
{
	int fds[2];
	if(::pipe(fds) != 0)
	{
		return;
	}
	std::fflush(stderr);
	m_saved = ::dup(STDERR_FILENO);
	if(m_saved < 0 || ::dup2(fds[1], STDERR_FILENO) < 0)
	{
		::close(fds[0]);
		::close(fds[1]);
		if(m_saved >= 0)
		{
			::close(m_saved);
			m_saved = -1;
		}
		return;
	}
	::close(fds[1]); // fd 2 is now the only write end
	m_read = fds[0];
	m_thread = std::thread(
	    [this]
	    {
		    std::string pending;
		    // Complete lines; with `all`, the unfinished last one too.
		    auto emit = [&pending](bool all)
		    {
			    size_t start = 0;
			    for(size_t nl;
			        (nl = pending.find('\n', start)) != std::string::npos;
			        start = nl + 1)
			    {
				    logLine(pending.substr(start, nl - start));
			    }
			    pending.erase(0, start);
			    if(all && !pending.empty())
			    {
				    logLine(pending);
				    pending.clear();
			    }
		    };
		    char buf[4096];
		    while(true)
		    {
			    pollfd p{m_read, POLLIN, 0};
			    const int r = ::poll(&p, 1, 100);
			    if(r > 0)
			    {
				    const ssize_t n = ::read(m_read, buf, sizeof buf);
				    if(n <= 0)
				    {
					    break; // every writer gone
				    }
				    pending.append(buf, static_cast<size_t>(n));
				    emit(false);
			    }
			    // Stopped and drained; a child still holding the pipe (a
			    // popen) must not keep the exit waiting.
			    else if((r == 0 && m_stop) || (r < 0 && errno != EINTR))
			    {
				    break;
			    }
		    }
		    emit(true);
	    });
}

StderrToLog::~StderrToLog()
{
	if(m_saved < 0)
	{
		return;
	}
	std::fflush(stderr);
	::dup2(m_saved, STDERR_FILENO); // drops the pipe's write end
	::close(m_saved);
	m_stop = true;
	m_thread.join();
	::close(m_read);
}

} // namespace rega::log
