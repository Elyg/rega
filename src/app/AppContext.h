#pragma once

#include "app/Annotations.h"
#include "image/Colour.h"
#include "image/ImageService.h"
#include "term/Caps.h"
#include "term/Kitty.h"

#include <ftxui/dom/elements.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace rega
{

class ImageSlot;
struct AppContext;

/// A slot drawing through the context's terminal; it asks for a redraw when a
/// picture it prepared in the background is ready.
std::unique_ptr<ImageSlot> newSlot(AppContext& ctx);

/// Pixels sent to the terminal per draw (kitty), shared by the images on
/// screen.
constexpr int TOTAL_PIXEL_BUDGET = 4'000'000;

/// What the browser and the viewer share: the services, the status message and
/// the way back to the event loop. Owned by App; handed down by reference.
struct AppContext
{
	ImageService& m_svc;
	TermCaps& m_caps;
	kitty::Transmitter& m_kitty;
	Annotations& m_ann;
	ColourManager& m_colour; ///< OCIO config, view and transforms
	/// Ask for a redraw. Safe from any thread.
	std::function<void()> m_redraw;
	/// Run `task` on the UI thread, without a redraw of its own. Safe from
	/// any thread.
	std::function<void(std::function<void()> task)> m_post;
	/// Leave the event loop (quit rega).
	std::function<void()> m_quit;
	/// One line in the bottom bar; cleared by the next key.
	std::string m_message;

	/// Clipboard + status message ("copied: …" / "copy failed").
	void copyText(std::string_view text);
	/// Move to the neighbouring tmux pane; false outside tmux.
	bool tmuxSelectPane(char dir) const;
	/// The bottom bar at the terminal's width, with m_message and the setup
	/// warning (see ui::statusLine).
	[[nodiscard]] ftxui::Element statusLine(std::string_view hints,
	                                        ftxui::Elements status = {},
	                                        std::string_view tail = {}) const;
	/// Decode reduction (1 = full resolution) for showing `bounds` in `slot`:
	/// fitted when `zoom` is empty, else at `zoom` terminal px per image px.
	/// With kitty, the slot renders at most `pixelCap` pixels, so less is
	/// needed. `slack` > 1 accepts upscaling the decode up to that much
	/// (playback: a little sharpness for a quarter of the memory).
	[[nodiscard]] int reduceFor(const ImageSlot& slot,
	                            const Box2i& bounds,
	                            std::optional<double> zoom,
	                            int pixelCap,
	                            double slack = 1.0) const;
};

} // namespace rega
