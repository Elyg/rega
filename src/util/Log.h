#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <string>
#include <thread>

namespace rv::log
{

/// Plain stdout logger (pattern "%v") for user-facing CLI output such as --dump.
spdlog::logger& out();

/// Default logger → stderr.
void initConsole();
/// Call before running the TUI: from then on everything goes to `file` (the
/// terminal belongs to FTXUI). Empty `file` → $RVTUI_LOG or
/// ~/.cache/rvtui/rvtui.log.
void redirectToFile(std::string file = {});

/// While alive, whatever is written to stderr (fd 2) is logged line by line
/// instead: libraries such as OpenEXR print their errors there, which would
/// land on top of the TUI and never reach the log. Restores stderr when
/// destroyed. Only after redirectToFile, or the lines come back to stderr.
class StderrToLog
{
public:
	StderrToLog();
	~StderrToLog();
	StderrToLog(const StderrToLog&) = delete;
	StderrToLog& operator=(const StderrToLog&) = delete;

private:
	int m_saved = -1; ///< the real stderr, put back on destruction
	int m_read = -1;  ///< the pipe's end the thread reads
	std::atomic<bool> m_stop{false};
	std::thread m_thread;
};

} // namespace rv::log
