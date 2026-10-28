#pragma once

#include "image/Render.h"
#include "term/Caps.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <functional>

namespace rega
{

namespace kitty
{
class Transmitter;
}

/// A picture the app paints itself (the playbar), shown through kitty
/// placeholder cells like an ImageSlot shows an image, under an image id of
/// its own. Painted at the terminal's pixel size on every draw; sent only
/// when its pixels change. Kitty only.
class BitmapSlot
{
public:
	/// Paints the picture, `width` x `height` terminal pixels.
	using Paint = std::function<Rgba8Image(int width, int height)>;

	BitmapSlot(const TermCaps& caps, kitty::Transmitter& tx);
	~BitmapSlot();
	BitmapSlot(const BitmapSlot&) = delete;
	BitmapSlot& operator=(const BitmapSlot&) = delete;

	/// An element filling what it is given (at most the placeholder limit)
	/// with what `paint` draws.
	[[nodiscard]] ftxui::Element element(Paint paint);
	/// Paint into `box` and put its placeholder cells on `screen`.
	void draw(ftxui::Screen& screen, const ftxui::Box& box, const Paint& paint);

private:
	const TermCaps& m_caps;
	kitty::Transmitter& m_tx;
	const uint32_t m_id;
	bool m_transmitted = false;
	int m_cols = 0, m_rows = 0;
	Rgba8Image m_sent; ///< the picture on screen
};

} // namespace rega
