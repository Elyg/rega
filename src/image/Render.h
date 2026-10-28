#pragma once

#include "image/ImageBuffer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace rega
{

class ColourTransform;

/// Colour, one channel as grey, or luma.
enum class ChannelMode
{
	COLOR,
	RED,
	GREEN,
	BLUE,
	ALPHA,
	LUMA
};
/// HUD name: "RGB", "R", "G", "B", "A", "Luma".
const char* channelModeName(ChannelMode m);

/// Which window outlines renderLayer draws (`w` cycles them).
enum class Outlines
{
	FRAME_AND_DATA, ///< display window, plus the data window when it differs
	FRAME,
	NONE
};

/// Display transform (see applyDisplay) and outlines.
struct DisplayParams
{
	ChannelMode m_mode = ChannelMode::COLOR;
	float m_exposure = 0.0f; ///< stops
	float m_gamma = 1.0f;
	bool m_srgb = true; ///< linear → sRGB OETF (else plain clamp)
	/// OCIO display transform in place of the sRGB OETF (when m_srgb):
	/// exposure, then this, then gamma.
	std::shared_ptr<const ColourTransform> m_ocio;
	Outlines m_outlines = Outlines::NONE; ///< `w` turns them on
	bool m_selected = false; ///< tiles: dashed highlight just inside the frame
	/// `!`: paint pixels holding NaN (NAN_RGB) / ±inf (INF_RGB) in the shown
	/// channels over the image.
	bool m_showNonFinite = false;
	/// Image pixel (x, y) ringed in red: the inspector's picked one.
	std::optional<std::pair<int, int>> m_marker;
	bool operator==(const DisplayParams&) const = default;
};

/// Where the image sits in the output: `zoom` = output pixels per image pixel,
/// `center` = image-space point shown at the middle of the output. `fit`
/// overrides both.
struct ViewParams
{
	bool m_fit = true;
	/// Fit the frame (display window) only, ignoring overscan: tiles, so
	/// every tile of a contact sheet shares one scale.
	bool m_fitFrame = false;
	double m_zoom = 1.0;
	double m_centerX = 0, m_centerY = 0;
	bool operator==(const ViewParams&) const = default;
};

/// 8-bit RGBA bitmap, as sent to the terminal.
struct Rgba8Image
{
	int m_width = 0, m_height = 0;
	std::vector<uint8_t> m_pixels; ///< RGBA, row-major
};

/// Resolved mapping of output pixels to image coordinates (display-window space).
struct ViewMapping
{
	double m_scale = 1; ///< output px per image px
	double m_originX = 0,
	       m_originY = 0; ///< image coordinate at output pixel (0,0)
	/// Image coordinate under output pixel (ox, oy).
	double imageX(double ox) const
	{
		return m_originX + ox / m_scale;
	}
	double imageY(double oy) const
	{
		return m_originY + oy / m_scale;
	}
};

/// Zoom (output px per image px) from which renderLayer outlines each image
/// pixel.
constexpr double PIXEL_GRID_MIN_SCALE = 8.0;
/// Grey of the display-window outline renderLayer draws around the image.
constexpr uint8_t OUTLINE_GREY = 96;
/// Dim cyan of the dashed data-window outline (drawn when it differs).
constexpr uint8_t DATA_OUTLINE_RGB[3] = {0, 150, 170};
/// Yellow dashed highlight on the selected tile (the one the panes describe).
constexpr uint8_t SELECTED_RGB[3] = {230, 200, 60};

/// What `!` paints over NaN / ±inf pixels (NaN wins where both).
constexpr uint8_t NAN_RGB[3] = {255, 0, 255};
constexpr uint8_t INF_RGB[3] = {0, 255, 255};
/// Red of the ring around the marked (picked) pixel, as Nuke's.
constexpr uint8_t MARKER_RGB[3] = {255, 40, 40};

/// `fitBox` is what fit mode frames: the frame plus overscan (fitBounds()).
ViewMapping resolveView(const Box2i& fitBox,
                        int outW,
                        int outH,
                        const ViewParams& view,
                        double pixelAspect = 1.0);

/// Exposure, sRGB OETF, gamma; result in [0, 1] (NaN/inf and negatives: 0).
float applyDisplay(float v, const DisplayParams& p);
/// One colour as renderLayer shows it, 8-bit: through the OCIO transform
/// when there is one, else applyDisplay() per channel.
std::array<int, 3>
displayRgb8(float r, float g, float b, const DisplayParams& p);

/// Resample + display-transform a layer into an RGBA8 image of outW x outH.
/// Pixels outside the data window are transparent (alpha 0) so the terminal
/// background shows through.
Rgba8Image renderLayer(const LayerImage& img,
                       int outW,
                       int outH,
                       const ViewParams& view,
                       const DisplayParams& disp);

} // namespace rega
