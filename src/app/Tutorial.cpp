#include "app/Tutorial.h"

#include "util/Parallel.h"

#include <Imath/ImathBox.h>
#include <Imath/ImathVec.h>
#include <Imath/half.h>
#include <spdlog/fmt/fmt.h>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfRationalAttribute.h>
#include <ImfStringAttribute.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace rega::tutorial
{

namespace
{

// Bump when what write() produces changes: ensure() then renders again.
constexpr int REVISION = 10;
constexpr const char* STAMP = ".rega-tutorial";
constexpr int FPS = 24;
constexpr int STILLS = 6;
// wip.####.exr, a render still being written: WIP_FRAMES frame numbers from
// the first, these missing, and the last one only partly written.
constexpr int WIP_FRAMES = 24;
constexpr std::array<int, 4> WIP_MISSING = {5, 6, 7, 14};
constexpr int WIP_FILES = WIP_FRAMES - int(WIP_MISSING.size());

// --- writing -----------------------------------------------------------------

struct Channel
{
	std::string m_name;
	Imf::PixelType m_type = Imf::HALF;
	std::vector<float> m_values; ///< over the data window, row by row
};

// One scanline EXR, ZIP compressed. `fps` 0 = no framesPerSecond.
void writeExr(const fs::path& path,
              const Imath::Box2i& display,
              const Imath::Box2i& data,
              const std::vector<Channel>& channels,
              const std::string& comments,
              int fps = 0)
{
	Imf::Header header(display, data);
	header.compression() = Imf::ZIP_COMPRESSION;
	header.insert("software", Imf::StringAttribute("rega --tutorial"));
	header.insert("comments", Imf::StringAttribute(comments));
	if(fps > 0)
	{
		header.insert("framesPerSecond",
		              Imf::RationalAttribute(Imf::Rational(fps, 1)));
	}
	std::vector<std::vector<half>> halves(channels.size());
	Imf::FrameBuffer fb;
	for(size_t i = 0; i < channels.size(); ++i)
	{
		const Channel& c = channels[i];
		header.channels().insert(c.m_name, Imf::Channel(c.m_type));
		char* base = nullptr;
		size_t stride = 0;
		if(c.m_type == Imf::HALF)
		{
			halves[i].assign(c.m_values.begin(), c.m_values.end());
			base = reinterpret_cast<char*>(halves[i].data());
			stride = sizeof(half);
		}
		else
		{
			base =
			    reinterpret_cast<char*>(const_cast<float*>(c.m_values.data()));
			stride = sizeof(float);
		}
		fb.insert(c.m_name, Imf::Slice::Make(c.m_type, base, data, stride));
	}
	Imf::OutputFile out(path.string().c_str(), header);
	out.setFrameBuffer(fb);
	out.writePixels(data.max.y - data.min.y + 1);
}

Imath::Box2i box(int w, int h)
{
	return {Imath::V2i(0, 0), Imath::V2i(w - 1, h - 1)};
}

// Named planes of one size, filled pixel by pixel.
struct Planes
{
	std::vector<Channel> m_channels;
	size_t m_size;

	Planes(size_t size,
	       std::initializer_list<std::pair<const char*, Imf::PixelType>> names)
	    : m_size(size)
	{
		for(const auto& [name, type] : names)
		{
			m_channels.push_back({name, type, std::vector<float>(size, 0.0f)});
		}
	}
	float& at(size_t channel, size_t px)
	{
		return m_channels[channel].m_values[px];
	}
};

float srgbToLinear(int c8)
{
	const float c = c8 / 255.0f;
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// --- the sequence: a tiny ray tracer (scripts/demo-sequence.py in C++) -------

using V3 = Imath::V3f;

float dot(V3 a, V3 b)
{
	return a ^ b;
}
V3 cross(V3 a, V3 b)
{
	return a % b;
}
V3 normalize(V3 v)
{
	return v.normalized();
}

void put3(Planes& p, int first, size_t px, V3 v)
{
	p.at(first, px) = v.x;
	p.at(first + 1, px) = v.y;
	p.at(first + 2, px) = v.z;
}

constexpr float FAR = 1e9f;

struct Sphere
{
	V3 m_centre;
	float m_radius;
	V3 m_colour;
};

float hitSphere(V3 orig, V3 dir, const Sphere& s)
{
	const V3 oc = orig - s.m_centre;
	const float b = dot(dir, oc);
	const float disc = b * b - (dot(oc, oc) - s.m_radius * s.m_radius);
	if(disc <= 0)
	{
		return FAR;
	}
	const float t = -b - std::sqrt(disc);
	return t > 1e-4f ? t : FAR;
}

float hitFloor(V3 orig, V3 dir)
{
	if(dir.y == 0)
	{
		return FAR;
	}
	const float t = -orig.y / dir.y;
	return t > 1e-4f ? t : FAR;
}

// The shot's three spheres at `frame` of `count` (one full, seamless loop),
// orbiting over the floor: red, cyan, yellow.
std::array<Sphere, 3> shotSpheres(int frame, int count)
{
	struct Orbit
	{
		float m_radius, m_phase, m_height, m_size;
		V3 m_colour;
	};
	static constexpr std::array<Orbit, 3> ORBITS = {{
	    {1.5f, 0.0f, 0.70f, 0.70f, {0.85f, 0.20f, 0.15f}},
	    {1.5f, 2.1f, 0.55f, 0.55f, {0.15f, 0.65f, 0.70f}},
	    {1.5f, 4.2f, 0.60f, 0.60f, {0.90f, 0.70f, 0.20f}},
	}};
	const float phase = 2 * std::numbers::pi_v<float> * frame / count;
	std::array<Sphere, 3> spheres;
	for(size_t i = 0; i < ORBITS.size(); ++i)
	{
		const Orbit& o = ORBITS[i];
		spheres[i] = {{o.m_radius * std::cos(o.m_phase + phase),
		               o.m_height,
		               o.m_radius * std::sin(o.m_phase + phase)},
		              o.m_size,
		              o.m_colour};
	}
	return spheres;
}

// The shot's camera: a ray per pixel of a w x h frame.
struct ShotCamera
{
	V3 m_eye{0.0f, 2.2f, -6.0f};
	V3 m_fwd = normalize(V3{0.0f, 0.5f, 0.0f} - m_eye);
	V3 m_right = normalize(cross({0.0f, 1.0f, 0.0f}, m_fwd));
	V3 m_up = cross(m_fwd, m_right);
	float m_halfFov = std::tan(20.0f * std::numbers::pi_v<float> / 180);

	V3 ray(float x, float y, int w, int h) const
	{
		const float sx = ((x + 0.5f) / w * 2 - 1) * m_halfFov * w / h;
		const float sy = (1 - (y + 0.5f) / h * 2) * m_halfFov;
		return normalize(m_fwd + m_right * sx + m_up * sy);
	}
};

// Three spheres orbiting over a checkered floor under a light going round the
// other way; `frame` of `count` is one full, seamless loop.
std::vector<Channel> renderShot(int frame, int count, int w, int h)
{
	const std::array<Sphere, 3> spheres = shotSpheres(frame, count);
	const float phase = 2 * std::numbers::pi_v<float> * frame / count;
	const V3 light{4 * std::cos(-phase), 5.0f, 4 * std::sin(-phase)};
	const V3 skyTop{0.20f, 0.35f, 0.75f}, skyHorizon{0.80f, 0.85f, 0.95f};

	const ShotCamera cam;
	const V3 eye = cam.m_eye;

	enum : uint8_t
	{
		R,
		G,
		B,
		A,
		DIFF_R,
		SPEC_R = DIFF_R + 3,
		ALB_R = SPEC_R + 3,
		N_X = ALB_R + 3,
		Z = N_X + 3,
		MASK_R
	};
	Planes p(size_t(w) * h,
	         {{"R", Imf::HALF},          {"G", Imf::HALF},
	          {"B", Imf::HALF},          {"A", Imf::HALF},
	          {"diffuse.R", Imf::HALF},  {"diffuse.G", Imf::HALF},
	          {"diffuse.B", Imf::HALF},  {"specular.R", Imf::HALF},
	          {"specular.G", Imf::HALF}, {"specular.B", Imf::HALF},
	          {"albedo.R", Imf::HALF},   {"albedo.G", Imf::HALF},
	          {"albedo.B", Imf::HALF},   {"N.X", Imf::HALF},
	          {"N.Y", Imf::HALF},        {"N.Z", Imf::HALF},
	          {"Z", Imf::HALF},          {"mask.R", Imf::HALF},
	          {"mask.G", Imf::HALF},     {"mask.B", Imf::HALF}});

	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const size_t px = size_t(y) * w + x;
			const V3 dir = cam.ray(float(x), float(y), w, h);

			// Nearest hit: 0 = floor, 1.. = spheres, -1 = sky.
			int obj = 0;
			float t = hitFloor(eye, dir);
			for(size_t i = 0; i < spheres.size(); ++i)
			{
				if(float ts = hitSphere(eye, dir, spheres[i]); ts < t)
				{
					t = ts;
					obj = int(i) + 1;
				}
			}
			const V3 sky = skyHorizon + (skyTop - skyHorizon) *
			                                std::clamp(dir.y * 3, 0.0f, 1.0f);
			p.at(A, px) = 1.0f;
			if(t >= FAR)
			{
				put3(p, R, px, sky);
				continue;
			}
			const V3 pos = eye + dir * t;
			V3 normal{0.0f, 1.0f, 0.0f}, albedo(0.0f);
			if(obj == 0)
			{
				const int checker =
				    int(std::floor(pos.x)) + int(std::floor(pos.z));
				albedo = V3(checker & 1 ? 0.55f : 0.35f);
			}
			else
			{
				const Sphere& s = spheres[obj - 1];
				normal = (pos - s.m_centre) * (1 / s.m_radius);
				albedo = s.m_colour;
			}

			// Hard shadows from the spheres.
			const V3 toLight = normalize(light - pos);
			const V3 shadowOrig = pos + normal * 1e-3f;
			bool lit = true;
			for(const Sphere& s : spheres)
			{
				lit = lit && hitSphere(shadowOrig, toLight, s) >= FAR;
			}
			const float ndl =
			    lit ? std::clamp(dot(normal, toLight), 0.0f, 1.0f) : 0.0f;
			const V3 diffuse = albedo * (0.12f + 0.95f * ndl);
			const float ndh =
			    std::clamp(dot(normal, normalize(toLight - dir)), 0.0f, 1.0f);
			const float spec = lit ? (obj > 0 ? 0.6f : 0.05f) *
			                             std::pow(ndh, obj > 0 ? 60.0f : 8.0f)
			                       : 0.0f;
			const V3 specular{spec, spec, spec};
			// The floor fades into the sky with distance.
			const float fog =
			    obj == 0 ? std::clamp((t - 8) / 14, 0.0f, 1.0f) : 0.0f;

			put3(p, R, px, (diffuse + specular) * (1 - fog) + sky * fog);
			put3(p, DIFF_R, px, diffuse);
			put3(p, SPEC_R, px, specular);
			put3(p, ALB_R, px, albedo);
			put3(p, N_X, px, normal);
			p.at(Z, px) = t;
			if(obj > 0)
			{
				p.at(MASK_R + obj - 1, px) = 1.0f;
			}
		}
	}
	return std::move(p.m_channels);
}

// --- the stills --------------------------------------------------------------

float linearToSrgb(float c)
{
	return c <= 0.0031308f ? c * 12.92f
	                       : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
}

// The 24 patches of a ColorChecker (sRGB), then a linear grey step wedge.
// Layers: the linear values, the same sRGB encoded, and a patch number.
// `v2`: a later version to diff against, two patches changed.
void writeChart(const fs::path& path, int w, int h, bool v2 = false)
{
	std::array<std::array<int, 3>, 24> patches = {{
	    {115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},
	    {133, 128, 177}, {103, 189, 170}, {214, 126, 44},  {80, 91, 166},
	    {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
	    {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},
	    {187, 86, 149},  {8, 133, 161},   {243, 243, 242}, {200, 200, 200},
	    {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52},
	}};
	if(v2)
	{
		patches[6] = {236, 150, 40}; // orange, warmer
		patches[14] = {150, 40, 50}; // red, darker
	}
	constexpr int STEPS = 11;
	enum : uint8_t
	{
		R,
		SRGB_R = 3,
		PATCH = 6
	};
	Planes p(size_t(w) * h,
	         {{"R", Imf::HALF},
	          {"G", Imf::HALF},
	          {"B", Imf::HALF},
	          {"srgb.R", Imf::HALF},
	          {"srgb.G", Imf::HALF},
	          {"srgb.B", Imf::HALF},
	          {"patch.id", Imf::HALF}});
	const int margin = std::max(1, h / 20);
	const int gap = std::max(1, margin / 3);
	const float cellW = float(w - 2 * margin) / 6;
	const float cellH = float(h - 2 * margin) / 5;
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const size_t px = size_t(y) * w + x;
			V3 v(0.008f);
			int id = 0;
			const float fx = x - margin, fy = y - margin;
			const int row = int(std::floor(fy / cellH));
			if(fx >= 0 && fy >= 0 && x < w - margin && row < 4)
			{
				const int col = int(fx / cellW);
				const float inX = fx - col * cellW, inY = fy - row * cellH;
				if(inX >= gap && inX < cellW - gap && inY >= gap &&
				   inY < cellH - gap)
				{
					const auto& rgb = patches[row * 6 + col];
					v = {srgbToLinear(rgb[0]),
					     srgbToLinear(rgb[1]),
					     srgbToLinear(rgb[2])};
					id = row * 6 + col + 1;
				}
			}
			else if(fx >= 0 && x < w - margin && row == 4 && y < h - margin)
			{
				const int step =
				    std::min(STEPS - 1,
				             int(fx / ((w - 2 * margin) / float(STEPS))));
				v = V3(step / float(STEPS - 1));
				id = 25 + step;
			}
			put3(p, R, px, v);
			put3(p,
			     SRGB_R,
			     px,
			     {linearToSrgb(v.x), linearToSrgb(v.y), linearToSrgb(v.z)});
			p.at(PATCH, px) = float(id);
		}
	}
	writeExr(
	    path,
	    box(w, h),
	    box(w, h),
	    p.m_channels,
	    v2 ? "chart.exr, a later version: the orange patch (7) warmer, "
	         "the red one (15) darker. In the browser mark chart.exr and "
	         "this one (space), then d: A, B and |A - B| side by side."
	       : "A colour chart over a linear grey wedge (0, 0.1 ... 1). "
	         "Layers: the linear values; srgb, the same encoded for a "
	         "display (what an 8-bit file would hold, so it looks washed "
	         "out through the view transform); patch, each patch's number "
	         "(1-24, wedge 25-35: E to stop down and see it, or the "
	         "inspector). Ctrl+click a patch to read every layer at once.");
}

// A sunset sky far brighter than white, split into light groups the way a
// renderer writes them: sky + sun + water = the beauty.
void writeSky(const fs::path& path, int w, int h)
{
	enum : uint8_t
	{
		R,
		SKY_R = 3,
		SUN_R = 6,
		WATER_R = 9
	};
	Planes p(size_t(w) * h,
	         {{"R", Imf::HALF},
	          {"G", Imf::HALF},
	          {"B", Imf::HALF},
	          {"sky.R", Imf::HALF},
	          {"sky.G", Imf::HALF},
	          {"sky.B", Imf::HALF},
	          {"sun.R", Imf::HALF},
	          {"sun.G", Imf::HALF},
	          {"sun.B", Imf::HALF},
	          {"water.R", Imf::HALF},
	          {"water.G", Imf::HALF},
	          {"water.B", Imf::HALF}});
	const float horizon = 0.62f;
	const float sunX = 0.68f, sunY = 0.42f, sunR = 0.03f;
	const V3 zenith{0.10f, 0.22f, 0.65f}, low{1.6f, 1.1f, 0.7f};
	const V3 glow{6.0f, 3.6f, 1.6f}, disc{50.0f, 46.0f, 38.0f};
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const size_t px = size_t(y) * w + x;
			const float u = (x + 0.5f) / w * w / h, v = (y + 0.5f) / h;
			const float d = std::hypot(u - sunX * w / h, v - sunY);
			V3 sky(0.0f), sun(0.0f), water(0.0f);
			if(v < horizon)
			{
				const float k = std::pow(v / horizon, 2.0f);
				sky = zenith + (low - zenith) * k;
				sun = glow * std::exp(-d / 0.07f) + (d < sunR ? disc : V3(0));
			}
			else
			{
				// Dark water, with the sun's streak on it.
				const float k = (v - horizon) / (1 - horizon);
				const float streak =
				    std::exp(-std::abs(u - sunX * w / h) / (0.01f + 0.05f * k));
				water =
				    V3{0.04f, 0.04f, 0.06f} + glow * (streak * 0.6f * (1 - k));
			}
			put3(p, R, px, sky + sun + water);
			put3(p, SKY_R, px, sky);
			put3(p, SUN_R, px, sun);
			put3(p, WATER_R, px, water);
		}
	}
	writeExr(
	    path,
	    box(w, h),
	    box(w, h),
	    p.m_channels,
	    "Light far brighter than white: the glow is up to 6 and the "
	    "sun's disc over 50. e / E: exposure up / down half a stop "
	    "(0 resets); stop down (E) a few times to see the disc. Layers are "
	    "light groups: sky + sun + water add up to the beauty.");
}

// A smooth linear ramp over a stepped one, one channel (Y) per layer.
void writeRamp(const fs::path& path, int w, int h)
{
	constexpr int STEPS = 11;
	Planes p(size_t(w) * h,
	         {{"Y", Imf::HALF},
	          {"srgb.Y", Imf::HALF},
	          {"inverted.Y", Imf::HALF}});
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const size_t px = size_t(y) * w + x;
			const float u = (x + 0.5f) / w;
			const float l = y < h / 2 ? u
			                          : std::min(STEPS - 1, int(u * STEPS)) /
			                                float(STEPS - 1);
			p.at(0, px) = l;
			p.at(1, px) = linearToSrgb(l);
			p.at(2, px) = 1 - l;
		}
	}
	writeExr(path,
	         box(w, h),
	         box(w, h),
	         p.m_channels,
	         "Single-channel layers (Y), shown as grey: a linear ramp 0 to 1 "
	         "over 11 steps; srgb, the same encoded for a display; inverted, "
	         "1 - Y. y / Y: gamma +/- 0.1; s: view transform on / off (raw "
	         "values).");
}

// The two windows, after the OpenEXR docs' example: a beach ball whose data
// window (where the pixels are) is a box around it, inside the display window
// (the image's frame). The rest of the display window has no pixels.
void writeBeachball(const fs::path& path, int w, int h)
{
	const float r = h * 0.3f, pad = r * 0.18f;
	const float cx = w * 0.62f, cy = h * 0.5f;
	const Imath::Box2i data(Imath::V2i(int(std::floor(cx - r - pad)),
	                                   int(std::floor(cy - r - pad))),
	                        Imath::V2i(int(std::ceil(cx + r + pad)),
	                                   int(std::ceil(cy + r + pad))));
	const int dw = data.max.x - data.min.x + 1;
	const int dh = data.max.y - data.min.y + 1;
	enum : uint8_t
	{
		R,
		A = 3,
		MASK,
		UV_U,
		UV_V
	};
	Planes p(size_t(dw) * dh,
	         {{"R", Imf::HALF},
	          {"G", Imf::HALF},
	          {"B", Imf::HALF},
	          {"A", Imf::HALF},
	          {"mask.Y", Imf::HALF},
	          {"uv.U", Imf::HALF},
	          {"uv.V", Imf::HALF}});
	// Six panels round the ball, white caps at the poles; the ball tipped
	// towards the camera so the top cap shows.
	static constexpr std::array<std::array<float, 3>, 6> PANELS = {{
	    {0.80f, 0.06f, 0.05f},
	    {0.85f, 0.85f, 0.82f},
	    {0.05f, 0.20f, 0.75f},
	    {0.90f, 0.70f, 0.05f},
	    {0.10f, 0.55f, 0.15f},
	    {0.85f, 0.85f, 0.82f},
	}};
	const float tilt = 0.45f, ct = std::cos(tilt), st = std::sin(tilt);
	const V3 light = normalize(V3{-0.5f, 0.7f, 0.6f});
	const V3 half = normalize(light + V3{0.0f, 0.0f, 1.0f});
	auto ball = [&](float x, float y, V3& out) -> bool
	{
		const float nx = (x - cx) / r, ny = (cy - y) / r;
		const float rr = nx * nx + ny * ny;
		if(rr >= 1)
		{
			return false;
		}
		const V3 n{nx, ny, std::sqrt(1 - rr)};
		// In the ball's own frame (tipped about x) for the panels.
		const V3 b{n.x, n.y * ct + n.z * st, -n.y * st + n.z * ct};
		V3 base(0.85f);
		if(std::abs(b.y) < 0.93f)
		{
			const float lon = std::atan2(b.x, b.z) + std::numbers::pi_v<float>;
			const auto& c =
			    PANELS[size_t(lon / (2 * std::numbers::pi_v<float>)*6) % 6];
			base = {c[0], c[1], c[2]};
		}
		const float diff = std::max(0.0f, dot(n, light));
		const float spec = 0.6f * std::pow(std::max(0.0f, dot(n, half)), 60.0f);
		out = base * (0.2f + 0.9f * diff) + V3(spec);
		return true;
	};
	for(int y = data.min.y; y <= data.max.y; ++y)
	{
		for(int x = data.min.x; x <= data.max.x; ++x)
		{
			const size_t px = size_t(y - data.min.y) * dw + (x - data.min.x);
			// The box's own background: dark grey, lighter than the empty
			// display window's black, with the data window's teal dashes and
			// the ball's white panels standing out on it.
			const float v = float(y - data.min.y) / dh;
			const V3 grey(0.075f - 0.02f * v);
			// 2 x 2 samples: a smooth edge.
			V3 sum(0.0f);
			int hits = 0;
			for(float sy : {0.25f, 0.75f})
			{
				for(float sx : {0.25f, 0.75f})
				{
					V3 c;
					if(ball(x + sx, y + sy, c))
					{
						sum = sum + c;
						++hits;
					}
					else
					{
						sum = sum + grey;
					}
				}
			}
			put3(p, R, px, sum * 0.25f);
			p.at(A, px) = 1.0f;
			p.at(MASK, px) = hits / 4.0f;
			p.at(UV_U, px) = (x + 0.5f) / w;
			p.at(UV_V, px) = 1 - (y + 0.5f) / h;
		}
	}
	writeExr(
	    path,
	    box(w, h),
	    data,
	    p.m_channels,
	    "The two windows, as in the OpenEXR docs: the display window is "
	    "the image's frame; the data window, the box around the ball, is "
	    "where the pixels are. The rest of the display window has no pixels "
	    "(black). w: outlines (display + data window dashed / display "
	    "window / off).");
}

// A broken render: beauty = diffuse + specular, and each of those broke in
// its own way, so the beauty has every problem. albedo is clean.
void writeNanInf(const fs::path& path, int w, int h)
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	enum : uint8_t
	{
		R,
		A = 3,
		DIFF_R,
		SPEC_R = DIFF_R + 3,
		ALB_R = SPEC_R + 3
	};
	Planes p(size_t(w) * h,
	         {{"R", Imf::FLOAT},
	          {"G", Imf::FLOAT},
	          {"B", Imf::FLOAT},
	          {"A", Imf::FLOAT},
	          {"diffuse.R", Imf::FLOAT},
	          {"diffuse.G", Imf::FLOAT},
	          {"diffuse.B", Imf::FLOAT},
	          {"specular.R", Imf::FLOAT},
	          {"specular.G", Imf::FLOAT},
	          {"specular.B", Imf::FLOAT},
	          {"albedo.R", Imf::FLOAT},
	          {"albedo.G", Imf::FLOAT},
	          {"albedo.B", Imf::FLOAT}});
	uint32_t seed = 12345; // fixed: the same pixels on every run
	auto random = [&seed]
	{
		seed = seed * 1664525u + 1013904223u;
		return seed >> 8;
	};
	for(int y = 0; y < h; ++y)
	{
		for(int x = 0; x < w; ++x)
		{
			const size_t px = size_t(y) * w + x;
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			const V3 albedo{0.2f + 0.6f * u,
			                0.3f + 0.4f * (1 - v),
			                0.5f + 0.4f * v};
			const float hl = std::exp(
			    -std::pow(std::hypot((u - 0.3f) * w / h, v - 0.35f) / 0.12f,
			              2.0f));
			V3 diffuse = albedo * 0.5f;
			if(random() % 400 == 0) // ~0.25% stray NaNs
			{
				diffuse = V3(nan);
			}
			put3(p, DIFF_R, px, diffuse);
			put3(p, SPEC_R, px, V3(0.8f * hl));
			put3(p, ALB_R, px, albedo);
		}
	}
	auto fill = [&](int channel, float value, int x0, int y0, int x1, int y1)
	{
		for(int y = std::max(0, y0); y < std::min(h, y1); ++y)
		{
			for(int x = std::max(0, x0); x < std::min(w, x1); ++x)
			{
				p.at(channel, size_t(y) * w + x) = value;
			}
		}
	};
	fill(DIFF_R + 1, nan, w / 8, h / 6, w / 8 + w / 10, h / 6 + h / 6);
	fill(SPEC_R, inf, w / 3, h / 2, w - w / 8, h / 2 + std::max(1, h / 100));
	fill(SPEC_R + 2,
	     -inf,
	     w * 3 / 5,
	     h * 3 / 4,
	     w * 3 / 5 + w / 12,
	     h * 3 / 4 + h / 8);
	for(size_t px = 0; px < size_t(w) * h; ++px)
	{
		for(int c = 0; c < 3; ++c)
		{
			p.at(R + c, px) = p.at(DIFF_R + c, px) + p.at(SPEC_R + c, px);
		}
		p.at(A, px) = 1.0f;
	}
	writeExr(path,
	         box(w, h),
	         box(w, h),
	         p.m_channels,
	         "A broken render: beauty = diffuse + specular. diffuse has stray "
	         "NaN pixels and a NaN block in G; specular a streak of +inf in R "
	         "and a patch of -inf in B; albedo is clean. The inspector (4) "
	         "warns about them; ! paints NaN magenta and inf cyan; [ / ] "
	         "step through the layers to find which one broke.");
}

constexpr const char* README = R"(rega tutorial
==============

Images made by `rega --tutorial` to try the viewer on.
A card in the corner walks you through them: F1 hides
it, F2 skips a step, F3 goes back one. Each image says
what it shows in its `comments` (metadata pane: 2 or
m). ? lists every key.

shot.####.exr  frames {first}-{last}, multi-layer, {fps} fps:
               beauty (RGBA), diffuse, specular,
               albedo, N, Z, mask
wip.####.exr   a render still being written: frames
               missing (G), the last one half done
chart.exr      one RGB layer: colour chart, grey wedge
chart-v2.exr   chart.exr changed: diff them (d)
hdr-sky.exr    values far above 1: stop down (E)
luma-ramp.exr  a single channel (Y)
beachball.exr  a data window inside the display
               window: the two windows (w)
nan-inf.exr    NaN and inf pixels (!: paint them)

A tour
------
Browser: j / k move with a live preview, Enter
opens, space marks.

1. Enter on shot.####.exr. space plays, , and .
   step, < and > jump to the ends, : goes to a
   frame, Shift+i / Shift+o set in and out.
2. [ / ] step through the layers; 5 lists them.
   T shows the layer's name; t tiles every layer
   side by side, each named.
3. c r g b a u: colour, one channel, alpha, luma.
   e / E exposure, y / Y gamma, 0 resets.
4. Ctrl+click a pixel: the inspector (4) shows it.
5. q: back to the browser (Shift+q quits rega
   from anywhere). Mark a few stills with space,
   then Enter: n / p step, t tiles them.
6. nan-inf.exr: the inspector warns about the NaNs;
   ! paints them.
7. In the files pane (3), l goes into a file's
   annotations and a adds a line over the image,
   such as [#@frame].
)";

std::string replaceAll(std::string s,
                       const std::string& from,
                       const std::string& to)
{
	for(size_t at = s.find(from); at != std::string::npos;
	    at = s.find(from, at + to.size()))
	{
		s.replace(at, from.size(), to);
	}
	return s;
}

std::string stampText(const Options& opts)
{
	return fmt::format("{} {} {} {}x{}\n",
	                   REVISION,
	                   opts.m_first,
	                   opts.m_frames,
	                   opts.m_width,
	                   opts.m_height);
}

} // namespace

fs::path defaultDir()
{
	fs::path base;
	if(const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
	{
		base = x;
	}
	else if(const char* home = std::getenv("HOME"); home && *home)
	{
		base = fs::path(home) / ".cache";
	}
	else
	{
		base = fs::temp_directory_path();
	}
	return base / "rega" / "tutorial";
}

void write(const fs::path& dir, const Options& opts, const Progress& progress)
{
	fs::create_directories(dir);
	fs::remove(dir / STAMP);
	// A shorter sequence than last time leaves no stale frames behind.
	for(const auto& e : fs::directory_iterator(dir))
	{
		const std::string name = e.path().filename().string();
		if(((name.starts_with("shot.") || name.starts_with("wip.")) &&
		    name.ends_with(".exr")) ||
		   name == "overscan.exr") // beachball.exr's older name
		{
			fs::remove(e.path());
		}
	}
	const int w = opts.m_width, h = opts.m_height;
	// The wip frames written, by index into the WIP_FRAMES.
	std::vector<int> wip;
	for(int i = 0; i < WIP_FRAMES; ++i)
	{
		if(std::ranges::find(WIP_MISSING, i) == WIP_MISSING.end())
		{
			wip.push_back(i);
		}
	}
	const int total = opts.m_frames + STILLS + WIP_FILES;
	std::atomic<int> done{0};
	std::mutex progressMutex;
	auto wrote = [&]
	{
		const int n = ++done;
		if(progress)
		{
			std::lock_guard lock(progressMutex);
			progress(n, total);
		}
	};
	// Every file is its own task: frames first (the bulk), then the stills.
	parallelFor(
	    total,
	    1,
	    [&](int begin, int end)
	    {
		    for(int i = begin; i < end; ++i)
		    {
			    if(i < opts.m_frames)
			    {
				    writeExr(dir / fmt::format("shot.{:04d}.exr",
				                               opts.m_first + i),
				             box(w, h),
				             box(w, h),
				             renderShot(i, opts.m_frames, w, h),
				             "A multi-layer render: beauty (RGBA), "
				             "diffuse, specular, albedo, N (world "
				             "normals), Z (depth) and mask (one "
				             "channel per sphere). 5: layers, t: "
				             "tile them all.",
				             FPS);
			    }
			    else if(i >= opts.m_frames + STILLS)
			    {
				    const int f = wip[i - opts.m_frames - STILLS];
				    const fs::path path =
				        dir / fmt::format("wip.{:04d}.exr", opts.m_first + f);
				    writeExr(
				        path,
				        box(w, h),
				        box(w, h),
				        renderShot(f, WIP_FRAMES, w, h),
				        fmt::format("A render still being written: frames "
				                    "{}-{} and {} aren't there yet (G: hold "
				                    "the nearest or skip them) and the last "
				                    "frame is cut short, its bottom still "
				                    "missing.",
				                    opts.m_first + WIP_MISSING[0],
				                    opts.m_first + WIP_MISSING[2],
				                    opts.m_first + WIP_MISSING[3]),
				        FPS);
				    if(f == WIP_FRAMES - 1)
				    {
					    // The renderer's still writing it: the header
					    // and the top strips only.
					    fs::resize_file(path, fs::file_size(path) * 3 / 5);
				    }
			    }
			    else
			    {
				    switch(i - opts.m_frames)
				    {
					    case 0:
						    writeChart(dir / "chart.exr", w, h);
						    break;
					    case 5:
						    writeChart(dir / "chart-v2.exr", w, h, true);
						    break;
					    case 1:
						    writeSky(dir / "hdr-sky.exr", w, h);
						    break;
					    case 2:
						    writeRamp(dir / "luma-ramp.exr", w, h);
						    break;
					    case 3:
						    writeBeachball(dir / "beachball.exr", w, h);
						    break;
					    case 4:
						    writeNanInf(dir / "nan-inf.exr", w, h);
						    break;
				    }
			    }
			    wrote();
		    }
	    });
	std::string readme = README;
	readme = replaceAll(readme, "{first}", std::to_string(opts.m_first));
	readme = replaceAll(readme,
	                    "{last}",
	                    std::to_string(opts.m_first + opts.m_frames - 1));
	readme = replaceAll(readme, "{fps}", std::to_string(FPS));
	std::ofstream(dir / "README.txt") << readme;
	// Last: only a complete set is stamped.
	std::ofstream(dir / STAMP) << stampText(opts);
}

bool ensure(const fs::path& dir, const Options& opts, const Progress& progress)
{
	std::ifstream in(dir / STAMP);
	std::stringstream have;
	have << in.rdbuf();
	if(in && have.str() == stampText(opts))
	{
		return false;
	}
	write(dir, opts, progress);
	return true;
}

} // namespace rega::tutorial
