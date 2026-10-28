#pragma once

#include "app/AppContext.h"
#include "app/ViewerState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace rega
{

/// "[x, y]" as Nuke counts them (y up from the frame's bottom), with ≈ when
/// read from a reduced resolution.
std::string sampleCoord(const Sample& s);
/// The sample as name/value rows: what the inspector's cursor moves over and
/// `y` / `Y` copy.
std::vector<std::pair<std::string, std::string>> sampleItems(const Sample& s);
/// "(0.500, 0.250, 0.125, 1.000)", each number in its channel's colour;
/// NaN / ±inf values highlighted.
ftxui::Element rgbaValues(const Sample& s);
/// Which non-finite values the samples (null ones skipped) hold, for the
/// inspector title's badge: "NaN", "inf", "NaN inf", or "" when all finite.
std::string nonFiniteLabel(std::initializer_list<const Sample*> samples);
/// "12 NaN · 3 inf px": the layer's broken pixels, "" when it has none.
std::string nonFiniteSummary(const LayerImage& img);
/// How NaN / ±inf values and warnings stand out: black on amber.
ftxui::Decorator nonFiniteStyle();

/// [4] pixel inspector: the live readout under the mouse, the picked
/// (ctrl+clicked) pixel as rows to move over and copy, and the layer's
/// per-channel stats.
class InspectorPane
{
public:
	InspectorPane(ViewerState& state, AppContext& ctx);

	bool isOpen() const noexcept
	{
		return m_open;
	}
	void setOpen(bool open) noexcept
	{
		m_open = open;
	}
	bool focused() const noexcept
	{
		return m_state.m_focus == Focus::INSPECT;
	}
	bool contains(int x, int y) const noexcept
	{
		return inside(m_box, x, y);
	}
	int cursor() const noexcept
	{
		return m_cursor;
	}

	/// @param hover the sample under the mouse.
	/// @param shown the layer on screen (null while decoding), for its count
	///              of NaN / inf pixels and its per-channel min / max / avg.
	[[nodiscard]] ftxui::Element render(const Sample& hover,
	                                    const LayerImage* shown = nullptr);
	/// Focused: move, y/Y copy, x clears the pick, 4 closes.
	[[nodiscard]] bool event(const ftxui::Event& e);

private:
	ViewerState& m_state;
	AppContext& m_ctx;
	bool m_open = false;
	int m_cursor = 0; ///< row in the picked readout
	bool m_pendingG = false;
	ftxui::Box m_box{};
};

} // namespace rega
