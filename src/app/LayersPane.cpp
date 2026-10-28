#include "app/LayersPane.h"

#include "util/Ui.h"

#include <algorithm>

using namespace ftxui;

namespace rega
{

LayersPane::LayersPane(ViewerState& state) : m_state(state)
{
}

void LayersPane::show(const ImageInfoPtr& info)
{
	m_open = true;
	m_state.m_focus = Focus::LAYERS;
	// Start the cursor on the layer being shown.
	m_cursor = info ? std::max(0, info->findLayer(m_state.m_layerLabel)) : 0;
}

void LayersPane::click(int y, const ImageInfoPtr& info)
{
	// A layer row (below the title) shows that layer.
	m_state.m_focus = Focus::LAYERS;
	const int row = y - m_box.y_min - 1;
	if(info && row >= 0 && row < static_cast<int>(info->m_layers.size()))
	{
		m_cursor = row;
		m_state.m_layerLabel = info->m_layers[row].label();
	}
}

Element LayersPane::render(const ImageInfoPtr& info)
{
	const bool focus = focused();
	const int n = info ? static_cast<int>(info->m_layers.size()) : 0;
	m_cursor = std::clamp(m_cursor, 0, std::max(0, n - 1));
	// The layer on screen, resolved the way the HUD does it.
	const int current =
	    info ? std::max(0, info->findLayer(m_state.m_layerLabel)) : -1;
	// Names are cut to fit the column less the scroll indicator, as in the
	// files pane: a row too wide makes ftxui squeeze all of its cells (the
	// marker column shifted). The middle goes: they share long prefixes.
	const int width = m_state.leftPanelWidth() - 1;
	Elements rows;
	for(int i = 0; i < n; ++i)
	{
		const LayerInfo& l = info->m_layers[i];
		const bool shown = i == current;
		std::string chans;
		for(const auto& c : l.m_channels)
		{
			chans += (chans.empty() ? "" : " ") + LayerInfo::shortName(c);
		}
		chans = " " + chans + " ";
		const int room =
		    std::max(1, width - 3 - static_cast<int>(chans.size()));
		Element row = hbox({
		    text(shown ? " ▶ " : "   ") | color(Color::Cyan),
		    text(ui::ellipsizeMiddle(l.label(), room)) |
		        (shown ? color(Color::Cyan) | bold : nothing),
		    filler(),
		    text(chans) | dim,
		});
		if(focus && i == m_cursor)
		{
			row = row | inverted | ftxui::focus;
		}
		rows.push_back(row);
	}
	constexpr int MAX_ROWS = 12;
	return vbox({
	           text(ui::paneTitle("─[5]─Layers", m_state.leftPanelWidth())) |
	               (focus ? color(Color::Green) | bold : dim),
	           vbox(std::move(rows)) | vscroll_indicator | yframe |
	               size(HEIGHT, LESS_THAN, MAX_ROWS),
	           focus ? ui::paneHints("", {{"Enter", "show"}, {"5", "close"}})
	                 : ui::paneHints("", {{"5", "focus"}}),
	       }) |
	       reflect(m_box);
}

bool LayersPane::event(const Event& e, const ImageInfoPtr& info)
{
	const int n = info ? static_cast<int>(info->m_layers.size()) : 0;
	auto ch = [&](const char* c) { return e == Event::Character(c); };
	// Moving the cursor shows that layer straight away.
	auto moveTo = [&](int i)
	{
		m_cursor = std::clamp(i, 0, std::max(0, n - 1));
		if(info && m_cursor < n)
		{
			m_state.m_layerLabel = info->m_layers[m_cursor].label();
		}
	};
	if(ch("j") || e == Event::ArrowDown)
	{
		moveTo(m_cursor + 1);
	}
	else if(ch("k") || e == Event::ArrowUp)
	{
		moveTo(m_cursor - 1);
	}
	else if(ch("G"))
	{
		moveTo(n - 1);
	}
	else if(ch("g"))
	{
		moveTo(0);
	}
	else if(e == Event::Return || ch("l"))
	{
		moveTo(m_cursor);
	}
	else if(ch("1") || ch("h"))
	{
		m_state.m_focus = Focus::IMAGE;
	}
	else if(ch("5") || ch("q") || e == Event::Escape)
	{
		m_open = false;
		m_state.m_focus = Focus::IMAGE;
	}
	else
	{
		return false;
	}
	return true;
}

} // namespace rega
