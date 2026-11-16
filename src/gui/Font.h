#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rega::gui
{

/// The window's monospace font (JetBrains Mono, built in) at one pixel size,
/// and the atlas its glyphs are rasterized into as they are first drawn.
/// Box-drawing, block and braille characters are drawn as shapes filling the
/// cell exactly, so borders join and blocks tile with no seams.
class Font
{
public:
	/// A glyph's place in the atlas and where it sits in a cell.
	struct Glyph
	{
		int m_x = 0, m_y = 0, m_w = 0, m_h = 0; ///< in the atlas
		int m_offX = 0, m_offY = 0; ///< top-left, from the cell's top-left
	};

	/// `regular` and `bold`: TrueType font files (copied).
	Font(std::span<const uint8_t> regular, std::span<const uint8_t> bold);
	~Font();
	Font(const Font&) = delete;
	Font& operator=(const Font&) = delete;

	/// Rasterize at `pixelHeight` (the em size, in screen pixels): new cell
	/// size, empty atlas.
	void setPixelHeight(float pixelHeight);
	[[nodiscard]] float pixelHeight() const noexcept
	{
		return m_pixelHeight;
	}
	[[nodiscard]] int cellW() const noexcept
	{
		return m_cellW;
	}
	[[nodiscard]] int cellH() const noexcept
	{
		return m_cellH;
	}

	/// The glyph for `cp` (rasterized now if new); nullptr when there is
	/// nothing to draw (a space). A glyph the font lacks draws as "?".
	const Glyph* glyph(char32_t cp, bool bold);

	/// The atlas: one byte of coverage a pixel, atlasSize() square.
	[[nodiscard]] const std::vector<uint8_t>& atlas() const noexcept
	{
		return m_atlas;
	}
	[[nodiscard]] int atlasSize() const noexcept
	{
		return m_atlasSize;
	}
	/// Whether the atlas changed since the last call (it needs uploading).
	bool takeDirty() noexcept
	{
		const bool d = m_dirty;
		m_dirty = false;
		return d;
	}
	/// Whether the atlas was emptied since the last call (glyphs handed out
	/// before then are gone: draw the frame again).
	bool takeReset() noexcept
	{
		const bool r = m_reset;
		m_reset = false;
		return r;
	}

private:
	struct Face;
	/// Room for a w x h glyph; false when the atlas is full.
	bool allocate(int w, int h, int& x, int& y);
	void clearAtlas();
	/// Box drawing, blocks, braille: drawn into a cell-sized bitmap; false
	/// for anything else.
	bool drawShape(char32_t cp, std::vector<uint8_t>& cell) const;
	Glyph* add(uint64_t key,
	           const std::vector<uint8_t>& pixels,
	           int w,
	           int h,
	           int offX,
	           int offY);

	std::vector<uint8_t> m_regularData, m_boldData;
	std::vector<Face> m_faces; ///< regular, bold
	float m_pixelHeight = 0;
	int m_cellW = 1, m_cellH = 1, m_baseline = 1;
	int m_atlasSize = 1024;
	std::vector<uint8_t> m_atlas;
	int m_penX = 0, m_penY = 0, m_rowH = 0; ///< shelf packing
	bool m_dirty = true, m_reset = false;
	std::unordered_map<uint64_t, Glyph> m_glyphs;
	std::unordered_map<uint64_t, bool> m_blank; ///< nothing to draw
};

/// The code points of `utf8` (invalid bytes as U+FFFD).
[[nodiscard]] std::u32string decodeUtf8(std::string_view utf8);

} // namespace rega::gui
