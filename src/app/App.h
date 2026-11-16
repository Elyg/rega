#pragma once

#include "app/Annotations.h"
#include "app/AppContext.h"
#include "app/Browser.h"
#include "app/FileWatcher.h"
#include "app/Host.h"
#include "app/Recent.h"
#include "app/StartScreen.h"
#include "app/Tour.h"
#include "app/Viewer.h"
#include "image/ImageService.h"
#include "term/Caps.h"
#include "term/Kitty.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rega
{

/// Command-line options.
struct AppOptions
{
	/// Directory, image(s) or sequence glob to open; empty = current dir.
	std::vector<std::string> m_paths;
	/// With `--diff`: everything to open, pairs and paths, in the order
	/// given (m_paths unused).
	std::vector<OpenArg> m_args;
	std::string m_graphics = "auto"; ///< auto | kitty | halfblock
	std::string m_transfer =
	    "auto"; ///< kitty pixels: auto | direct | shm | file
	/// Write chosen paths here on `o` and exit (nvim integration).
	std::string m_chooserFile;
	std::string m_cwdFile; ///< write the last directory here on exit
	/// Decoded-image cache budget.
	std::uint64_t m_cacheBytes = ImageService::DEFAULT_BUDGET;
	bool m_tutorial = false; ///< show the tutorial tour (see Tour)
	/// The window, given nothing to open: the start screen first (the
	/// browser, at m_paths, one key away).
	bool m_start = false;
	/// The window: where what is opened is remembered for the start screen
	/// (empty: nowhere; the terminal never remembers).
	std::filesystem::path m_recentFile;
	/// Inline transfer: bytes a second the link carries (0: unpaced).
	double m_linkRate = kitty::Transmitter::DEFAULT_LINK_RATE;
	/// Terminal image memory played frames may keep (inline only; 0 none).
	std::uint64_t m_terminalCache = DEFAULT_TERMINAL_CACHE;
	static constexpr std::uint64_t DEFAULT_TERMINAL_CACHE = 256ull << 20;
};

/// Run the TUI until quit; returns the exit code.
int runApp(const AppOptions& opts);

/// The shell around the browser and the viewer: which one is shown, the help
/// overlay, the shared services, and watching the disk.
class App
{
public:
	/// `caps`: how to draw, when the host decides (the window); else what
	/// the terminal can do (detectCaps()).
	App(const AppOptions& opts, Host& host, std::optional<TermCaps> caps = {});
	~App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	/// The frame to draw.
	[[nodiscard]] ftxui::Element render();
	/// Handle an event; true when consumed.
	[[nodiscard]] bool onEvent(ftxui::Event e);

	/// Open `paths` as the command line would (a directory in the browser;
	/// images, sequences in the viewer): files dropped on the window, or
	/// picked in its Open dialog.
	void openPaths(const std::vector<std::string>& paths);
	/// The window: the pointer is at (`x`, `y`) cells, fractions kept; the
	/// next mouse event is there.
	void setPointer(double x, double y)
	{
		m_ctx.m_pointer = std::pair(x, y);
	}
	/// The window's Ctrl+V: on the start screen, paths for its list; while
	/// typing, the text; else paths to open, as a drop would.
	void pasteText(std::string_view text);
	/// The window's cell size changed (font size, display scale).
	void setCellSize(int w, int h);
	/// The directory the browser shows.
	[[nodiscard]] std::filesystem::path browserDir() const;
	/// What the window may remember (recent list, last folder): not the
	/// tutorial's images.
	[[nodiscard]] bool rememberable(const std::string& path) const;
	/// What the window's title shows: the image or directory open.
	[[nodiscard]] std::string title() const;

private:
	enum class Mode
	{
		START, ///< the window's start screen
		BROWSER,
		VIEWER
	};

	/// The start screen's Tutorial: render its images unless they are there
	/// (in the background, progress in the status bar), then browse them
	/// with the tour.
	void startTutorial();
	/// The window's Ctrl+C: copy what is selected (see onEvent).
	void copyShortcut();
	/// Show the viewer; closing it comes back to where this was.
	void toViewer();
	/// Show the start screen (re-listing what still exists).
	void toStart();
	/// Remember `entries` opened (Recent): their paths, sequences as
	/// patterns; too many as their folder.
	void remember(const std::vector<Entry>& entries);

	[[nodiscard]] ftxui::Element renderHelp();
	/// `S`: what the link carries, what a picture costs, what is cached.
	[[nodiscard]] ftxui::Element renderStats();
	bool helpEvent(ftxui::Event e);
	/// On the UI thread: files changed on disk (see FileWatcher). Re-list,
	/// re-scan the open sequences, drop stale decodes, redraw.
	void filesChanged(const std::vector<std::filesystem::path>& changed);
	/// On the UI thread: other terminals attached to tmux. Send every
	/// picture again, `t`'s way.
	void clientsChanged(Transfer t);
	/// Point the watcher at the directories on screen.
	void updateWatch();
	/// What the tour watches, as things are now.
	[[nodiscard]] TourView tourView() const;
	/// Open the command line's `args` / `paths` (see AppOptions).
	void open(const std::vector<OpenArg>& args,
	          const std::vector<std::string>& paths);

	AppOptions m_opts;
	Host& m_host;
	TermCaps m_caps;
	kitty::Transmitter m_kitty;
	ImageService m_svc;
	Annotations m_ann;
	std::filesystem::path m_annPath; ///< state file (empty = not saved)
	ColourManager m_colour;
	AppContext m_ctx;
	Mode m_mode = Mode::BROWSER;
	Mode m_back = Mode::BROWSER; ///< where closing the viewer goes
	Recent m_recent;
	bool m_remember = false; ///< opens now go to m_recent
	bool m_help = false;
	bool m_stats = false;    ///< `S`: the link and cache overlay
	int m_helpScroll = 0;    ///< first help line shown (short terminals)
	std::string m_helpQuery; ///< typed while the help is open: shows matches

	Browser m_browser;
	Viewer m_viewer;
	StartScreen m_start;
	std::optional<Tour> m_tour; ///< --tutorial only
	std::vector<std::filesystem::path> m_watched;
	/// --transfer auto in tmux: re-checks the clients attached.
	std::jthread m_clientCheck;
	std::jthread m_linkPoll;    ///< in tmux: what it sent clients over ssh
	std::jthread m_tutorialJob; ///< rendering the tutorial's images
	bool m_tutorialBusy = false;
	FileWatcher m_watcher; ///< last: stops before the rest goes
};

} // namespace rega
