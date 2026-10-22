#include "term/BitmapSlot.h"

#include "term/Kitty.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>

#include <algorithm>
#include <utility>

namespace rv
{

namespace
{

class BitmapNode : public ftxui::Node
{
public:
	BitmapNode(BitmapSlot* slot, BitmapSlot::Paint paint)
	    : m_slot(slot), m_paint(std::move(paint))
	{
	}

	void ComputeRequirement() override
	{
		requirement_ = {};
		requirement_.min_x = 1;
		requirement_.min_y = 1;
		requirement_.flex_grow_x = requirement_.flex_shrink_x = 1;
	}

	void Render(ftxui::Screen& screen) override
	{
		m_slot->draw(screen, box_, m_paint);
	}

private:
	BitmapSlot* m_slot;
	BitmapSlot::Paint m_paint;
};

} // namespace

BitmapSlot::BitmapSlot(const TermCaps& caps, kitty::Transmitter& tx)
    : m_caps(caps), m_tx(tx), m_id(tx.allocId())
{
}

BitmapSlot::~BitmapSlot()
{
	if(m_transmitted)
	{
		m_tx.write(kitty::deleteImage(m_id, m_tx.tmux()), true);
	}
	m_tx.releaseId(m_id);
}

ftxui::Element BitmapSlot::element(Paint paint)
{
	return std::make_shared<BitmapNode>(this, std::move(paint));
}

void BitmapSlot::draw(ftxui::Screen& screen,
                      const ftxui::Box& box,
                      const Paint& paint)
{
	const int cols =
	    std::min(box.x_max - box.x_min + 1, kitty::MAX_PLACEHOLDER_INDEX);
	const int rows =
	    std::min(box.y_max - box.y_min + 1, kitty::MAX_PLACEHOLDER_INDEX);
	if(cols <= 0 || rows <= 0)
	{
		return;
	}
	Rgba8Image bmp = paint(cols * m_caps.m_cellW, rows * m_caps.m_cellH);
	if(!m_transmitted || cols != m_cols || rows != m_rows ||
	   bmp.m_pixels != m_sent.m_pixels)
	{
		// Replaces the picture under m_id; a new size in cells drops the old
		// placement first (as ImageSlot::apply does).
		if(m_transmitted && (cols != m_cols || rows != m_rows))
		{
			m_tx.write(kitty::deletePlacements(m_id, m_tx.tmux()));
		}
		kitty::TransmitOptions opt;
		opt.m_id = m_id;
		opt.m_cols = cols;
		opt.m_rows = rows;
		opt.m_measureCost = false; // drawn shapes cost nothing like a picture
		m_tx.write(m_tx.encode(bmp, opt));
		m_transmitted = true;
		m_cols = cols;
		m_rows = rows;
		m_sent = std::move(bmp);
	}
	const ftxui::Color fg =
	    ftxui::Color::Palette256(static_cast<uint8_t>(m_id));
	for(int r = 0; r < rows; ++r)
	{
		for(int c = 0; c < cols; ++c)
		{
			auto& px = screen.PixelAt(box.x_min + c, box.y_min + r);
			px.character = kitty::placeholderCell(r, c);
			px.foreground_color = fg;
			px.background_color = ftxui::Color::Default;
		}
	}
}

} // namespace rv
