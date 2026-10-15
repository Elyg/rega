#pragma once

#include "app/Annotations.h"
#include "app/AppContext.h"
#include "app/Browser.h"
#include "app/FileWatcher.h"
#include "app/Tour.h"
#include "app/Viewer.h"
#include "image/ImageService.h"
#include "term/Caps.h"
#include "term/Kitty.h"

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rv
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
	/// Inline transfer: bytes a second the link carries (0: unpaced).
	double m_linkRate = kitty::Transmitter::DEFAULT_LINK_RATE;
};

/// Run the TUI until quit; returns the exit code.
int runApp(const AppOptions& opts);

/// The shell around the browser and the viewer: which one is shown, the help
/// overlay, the shared services, and watching the disk.
class App
{
public:
	App(const AppOptions& opts, ftxui::ScreenInteractive& screen);
	~App();
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	/// The frame to draw.
	[[nodiscard]] ftxui::Element render();
	/// Handle an event; true when consumed.
	[[nodiscard]] bool onEvent(ftxui::Event e);

private:
	enum class Mode
	{
		BROWSER,
		VIEWER
	};

	[[nodiscard]] ftxui::Element renderHelp();
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

	AppOptions m_opts;
	ftxui::ScreenInteractive& m_screen;
	TermCaps m_caps;
	kitty::Transmitter m_kitty;
	ImageService m_svc;
	Annotations m_ann;
	std::filesystem::path m_annPath; ///< state file (empty = not saved)
	ColourManager m_colour;
	AppContext m_ctx;
	Mode m_mode = Mode::BROWSER;
	bool m_help = false;
	int m_helpScroll = 0;    ///< percent of the help scrolled (short terminals)
	std::string m_helpQuery; ///< typed while the help is open: shows matches

	Browser m_browser;
	Viewer m_viewer;
	std::optional<Tour> m_tour; ///< --tutorial only
	std::vector<std::filesystem::path> m_watched;
	/// --transfer auto in tmux: re-checks the clients attached.
	std::jthread m_clientCheck;
	FileWatcher m_watcher; ///< last: stops before the rest goes
};

} // namespace rv
