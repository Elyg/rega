#include "app/AppContext.h"

#include "term/ImageView.h"
#include "util/Clipboard.h"
#include "util/Ui.h"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cmath>

using namespace ftxui;

namespace rega
{

std::unique_ptr<ImageSlot> newSlot(AppContext& ctx)
{
	return std::make_unique<ImageSlot>(ctx.m_caps, ctx.m_kitty, ctx.m_redraw);
}

void AppContext::copyText(std::string_view text)
{
	constexpr size_t MAX_SHOWN = 60;
	m_message =
	    !copyToClipboard(text, m_caps.m_tmux) ? "copy failed"
	    : text.size() > MAX_SHOWN
	        ? "copied: " + std::string(text.substr(0, MAX_SHOWN - 1)) + "…"
	        : "copied: " + std::string(text);
}

bool AppContext::tmuxSelectPane(char dir) const
{
	return rega::tmuxSelectPane(dir, m_caps.m_tmux);
}

Element AppContext::statusLine(std::string_view hints,
                               Elements status,
                               std::string_view tail) const
{
	return ui::statusLine({.m_message = m_message,
	                       .m_hints = hints,
	                       .m_status = std::move(status),
	                       .m_tail = tail,
	                       .m_warning = m_caps.m_note},
	                      Terminal::Size().dimx);
}

int AppContext::reduceFor(const ImageSlot& slot,
                          const Box2i& bounds,
                          std::optional<double> zoom,
                          int pixelCap,
                          double slack) const
{
	auto [aw, ah] = slot.areaPixels();
	if(!slot.drawn() || aw <= 0)
	{
		auto d = Terminal::Size();
		aw = d.dimx * slot.pxPerCellX();
		ah = d.dimy * slot.pxPerCellY();
	}
	// Output px per image px.
	double scale = zoom ? *zoom
	                    : std::min(aw / static_cast<double>(bounds.width()),
	                               ah / static_cast<double>(bounds.height()));
	// Account for the kitty transmit cap (the slot renders below terminal
	// resolution).
	const double area = static_cast<double>(aw) * ah;
	if(m_caps.m_graphics == GraphicsMode::KITTY && area > pixelCap)
	{
		scale *= std::sqrt(pixelCap / area);
	}
	if(scale * slack >= 1.0)
	{
		return 1;
	}
	return std::clamp(static_cast<int>(std::floor(slack / scale)), 1, 32);
}

} // namespace rega
