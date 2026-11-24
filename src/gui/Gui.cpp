#include "gui/Gui.h"

#include "BuildInfo.h"
#include "app/App.h"
#include "app/Host.h"
#include "app/Recent.h"
#include "gui/Font.h"
#include "gui/Input.h"
#include "gui/Settings.h"
#include "term/Native.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Ui.h"

#include <SDL3/SDL.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color_info.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stb_image.h> // its code is in StbLoader.cpp
#include <string>
#include <unordered_map>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace rega::assets
{
// The built-in font (generated from assets/fonts by cmake/Embed.cmake).
extern const std::uint8_t FONT_REGULAR[];
extern const std::size_t FONT_REGULAR_SIZE;
extern const std::uint8_t FONT_BOLD[];
extern const std::size_t FONT_BOLD_SIZE;
extern const std::uint8_t ICON_PNG[];
extern const std::size_t ICON_PNG_SIZE;
} // namespace rega::assets

namespace fs = std::filesystem;

namespace rega::gui
{

namespace
{

struct Rgb
{
	uint8_t m_r = 0, m_g = 0, m_b = 0;
	bool operator==(const Rgb&) const = default;
};

// The window's colours: a neutral dark grey (images are judged against it),
// and a 16-colour palette in the spirit of a modern terminal theme.
constexpr Rgb DEFAULT_BG{24, 25, 28};
constexpr Rgb DEFAULT_FG{218, 220, 224};
constexpr std::array<Rgb, 16> PALETTE = {{
    {14, 15, 18},    // black: darker than the background (popups)
    {224, 108, 117}, // red
    {152, 195, 121}, // green
    {229, 192, 123}, // yellow
    {97, 175, 239},  // blue
    {198, 120, 221}, // magenta
    {86, 182, 194},  // cyan
    {171, 178, 191}, // light grey
    {98, 104, 117},  // dark grey
    {255, 122, 133}, // light red
    {181, 232, 144}, // light green
    {255, 214, 138}, // light yellow
    {124, 195, 255}, // light blue
    {222, 149, 240}, // light magenta
    {114, 210, 221}, // light cyan
    {255, 255, 255}, // white
}};

// An ftxui::Color as RGB; nullopt for the default (Color::Default). FTXUI
// keeps its value private: read it back from the escape it would print.
std::optional<Rgb> resolve(const ftxui::Color& c)
{
	const std::string s = c.Print(false);
	auto num = [](std::string_view v)
	{
		int x = 0;
		std::from_chars(v.data(), v.data() + v.size(), x);
		return x;
	};
	if(s == "39")
	{
		return std::nullopt;
	}
	if(s.starts_with("38;2;"))
	{
		const std::string_view v(s);
		const size_t a = 5, b = v.find(';', a), d = v.find(';', b + 1);
		return Rgb{static_cast<uint8_t>(num(v.substr(a, b - a))),
		           static_cast<uint8_t>(num(v.substr(b + 1, d - b - 1))),
		           static_cast<uint8_t>(num(v.substr(d + 1)))};
	}
	if(s.starts_with("38;5;"))
	{
		const int i = num(std::string_view(s).substr(5));
		if(i < 16)
		{
			return PALETTE[static_cast<size_t>(i)];
		}
		const auto info =
		    ftxui::GetColorInfo(static_cast<ftxui::Color::Palette256>(i));
		return Rgb{info.red, info.green, info.blue};
	}
	const int code = num(s); // 30-37, 90-97
	const int i = code >= 90 ? code - 90 + 8 : code - 30;
	return PALETTE[static_cast<size_t>(std::clamp(i, 0, 15))];
}

Rgb mix(Rgb a, Rgb b, float t)
{
	auto m = [t](uint8_t x, uint8_t y)
	{ return static_cast<uint8_t>(std::lround(x + (y - x) * t)); };
	return {m(a.m_r, b.m_r), m(a.m_g, b.m_g), m(a.m_b, b.m_b)};
}

SDL_FColor fcolor(Rgb c, float alpha = 1.0f)
{
	return {c.m_r / 255.0f, c.m_g / 255.0f, c.m_b / 255.0f, alpha};
}

// Triangles for SDL_RenderGeometry: quads of one texture (or none).
struct Batch
{
	std::vector<SDL_Vertex> m_vertices;
	std::vector<int> m_indices;

	void clear()
	{
		m_vertices.clear();
		m_indices.clear();
	}
	void quad(float x0,
	          float y0,
	          float x1,
	          float y1,
	          SDL_FColor c,
	          float u0 = 0,
	          float v0 = 0,
	          float u1 = 0,
	          float v1 = 0)
	{
		const int base = static_cast<int>(m_vertices.size());
		m_vertices.push_back({{x0, y0}, c, {u0, v0}});
		m_vertices.push_back({{x1, y0}, c, {u1, v0}});
		m_vertices.push_back({{x1, y1}, c, {u1, v1}});
		m_vertices.push_back({{x0, y1}, c, {u0, v1}});
		for(int i : {0, 1, 2, 0, 2, 3})
		{
			m_indices.push_back(base + i);
		}
	}
	void draw(SDL_Renderer* r, SDL_Texture* tex) const
	{
		if(!m_indices.empty())
		{
			SDL_RenderGeometry(r,
			                   tex,
			                   m_vertices.data(),
			                   static_cast<int>(m_vertices.size()),
			                   m_indices.data(),
			                   static_cast<int>(m_indices.size()));
		}
	}
};

// The window's way back into its loop from other threads: an SDL event wakes
// it; posted tasks wait in a queue.
class WindowHost final : public Host
{
public:
	WindowHost() : m_wake(SDL_RegisterEvents(1))
	{
	}

	void redraw() override
	{
		if(!m_redraw.exchange(true))
		{
			wake();
		}
	}
	void post(std::function<void()> task) override
	{
		{
			std::lock_guard lk(m_mu);
			m_tasks.push_back(std::move(task));
		}
		wake();
	}
	void quit() override
	{
		m_quit = true;
		wake();
	}
	[[nodiscard]] bool gui() const noexcept override
	{
		return true;
	}
	bool copyText(std::string_view text) override
	{
		return SDL_SetClipboardText(std::string(text).c_str());
	}
	void openDialog(bool folder) override
	{
		if(m_dialog)
		{
			m_dialog(folder);
		}
	}
	/// What openDialog() does (set once the window and the app are up).
	void setDialog(std::function<void(bool folder)> dialog)
	{
		m_dialog = std::move(dialog);
	}

	[[nodiscard]] bool quitting() const noexcept
	{
		return m_quit;
	}
	/// Whether a redraw was asked for since the last call.
	bool takeRedraw() noexcept
	{
		return m_redraw.exchange(false);
	}
	/// Run the posted tasks; true if there were any.
	bool runTasks()
	{
		std::deque<std::function<void()>> tasks;
		{
			std::lock_guard lk(m_mu);
			tasks.swap(m_tasks);
		}
		for(auto& t : tasks)
		{
			t();
		}
		return !tasks.empty();
	}

private:
	void wake()
	{
		SDL_Event e{};
		e.type = m_wake;
		SDL_PushEvent(&e);
	}

	const uint32_t m_wake;
	std::atomic<bool> m_redraw{false};
	std::atomic<bool> m_quit{false};
	std::mutex m_mu;
	std::deque<std::function<void()>> m_tasks;
	std::function<void(bool)> m_dialog;
};

/// Draws a frame of cells and pictures with SDL's renderer.
class Painter
{
public:
	Painter(SDL_Renderer* r, Font& font) : m_r(r), m_font(font)
	{
	}
	~Painter()
	{
		if(m_atlas)
		{
			SDL_DestroyTexture(m_atlas);
		}
		for(auto& [id, t] : m_pictures)
		{
			SDL_DestroyTexture(t.m_texture);
		}
	}
	Painter(const Painter&) = delete;
	Painter& operator=(const Painter&) = delete;

	/// Draw `screen` with the pictures `native` holds, then present.
	void paint(const ftxui::Screen& screen, const NativeImages& native)
	{
		const int cw = m_font.cellW(), ch = m_font.cellH();
		SDL_SetRenderDrawColor(m_r,
		                       DEFAULT_BG.m_r,
		                       DEFAULT_BG.m_g,
		                       DEFAULT_BG.m_b,
		                       255);
		SDL_RenderClear(m_r);

		// Pictures first: the cells still marked as theirs stay clear and
		// show them; everything drawn over them (popups, labels) covers them.
		std::unordered_map<uint32_t, bool> used;
		for(const auto& p : native.placements())
		{
			SDL_Texture* tex = picture(p.m_id, p.m_bitmap);
			used[p.m_id] = true;
			if(tex && p.m_at)
			{
				// Moved and scaled into the box, nothing outside it.
				const SDL_Rect clip{p.m_x * cw,
				                    p.m_y * ch,
				                    p.m_cols * cw,
				                    p.m_rows * ch};
				const SDL_FRect dst{static_cast<float>(p.m_x * cw +
				                                       p.m_at->m_x),
				                    static_cast<float>(p.m_y * ch +
				                                       p.m_at->m_y),
				                    static_cast<float>(p.m_at->m_w),
				                    static_cast<float>(p.m_at->m_h)};
				SDL_SetRenderClipRect(m_r, &clip);
				if(p.m_fill)
				{
					SDL_SetRenderDrawColor(m_r,
					                       TILE_FILL[0],
					                       TILE_FILL[1],
					                       TILE_FILL[2],
					                       255);
					const SDL_FRect box{static_cast<float>(clip.x),
					                    static_cast<float>(clip.y),
					                    static_cast<float>(clip.w),
					                    static_cast<float>(clip.h)};
					SDL_RenderFillRect(m_r, &box);
				}
				SDL_RenderTexture(m_r, tex, nullptr, &dst);
				SDL_SetRenderClipRect(m_r, nullptr);
			}
			else if(tex)
			{
				const SDL_FRect dst{static_cast<float>(p.m_x * cw),
				                    static_cast<float>(p.m_y * ch),
				                    static_cast<float>(p.m_cols * cw),
				                    static_cast<float>(p.m_rows * ch)};
				SDL_RenderTexture(m_r, tex, nullptr, &dst);
			}
		}
		for(auto it = m_pictures.begin(); it != m_pictures.end();)
		{
			if(!used.contains(it->first))
			{
				SDL_DestroyTexture(it->second.m_texture);
				it = m_pictures.erase(it);
			}
			else
			{
				++it;
			}
		}

		m_back.clear();
		m_glyphs.clear();
		m_lines.clear();
		for(int y = 0; y < screen.dimy(); ++y)
		{
			for(int x = 0; x < screen.dimx(); ++x)
			{
				cell(screen.PixelAt(x, y), x, y, cw, ch);
			}
		}
		if(m_font.takeReset())
		{
			// Glyphs of this frame went with the atlas: once more, fresh.
			m_glyphs.clear();
			m_lines.clear();
			m_back.clear();
			for(int y = 0; y < screen.dimy(); ++y)
			{
				for(int x = 0; x < screen.dimx(); ++x)
				{
					cell(screen.PixelAt(x, y), x, y, cw, ch);
				}
			}
			m_font.takeReset();
		}
		uploadAtlas();
		m_back.draw(m_r, nullptr);
		m_glyphs.draw(m_r, m_atlas);
		m_lines.draw(m_r, nullptr);
	}

	/// The frame just drawn as RGBA (before it is presented).
	bool read(std::vector<uint8_t>& rgba, int& w, int& h)
	{
		SDL_Surface* s = SDL_RenderReadPixels(m_r, nullptr);
		if(!s)
		{
			return false;
		}
		SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(s);
		if(!c)
		{
			return false;
		}
		w = c->w;
		h = c->h;
		rgba.resize(static_cast<size_t>(w) * h * 4);
		for(int y = 0; y < h; ++y)
		{
			std::memcpy(&rgba[static_cast<size_t>(y) * w * 4],
			            static_cast<const uint8_t*>(c->pixels) +
			                static_cast<size_t>(y) * c->pitch,
			            static_cast<size_t>(w) * 4);
		}
		SDL_DestroySurface(c);
		return true;
	}

private:
	struct Picture
	{
		SDL_Texture* m_texture = nullptr;
		const Rgba8Image* m_source = nullptr; ///< what it holds
		int m_w = 0, m_h = 0;
	};

	SDL_Texture* picture(uint32_t id,
	                     const std::shared_ptr<const Rgba8Image>& bmp)
	{
		Picture& p = m_pictures[id];
		if(p.m_source == bmp.get() && p.m_texture)
		{
			return p.m_texture;
		}
		if(bmp->m_width <= 0 || bmp->m_height <= 0)
		{
			return nullptr;
		}
		if(!p.m_texture || p.m_w != bmp->m_width || p.m_h != bmp->m_height)
		{
			if(p.m_texture)
			{
				SDL_DestroyTexture(p.m_texture);
			}
			p.m_texture = SDL_CreateTexture(m_r,
			                                SDL_PIXELFORMAT_RGBA32,
			                                SDL_TEXTUREACCESS_STREAMING,
			                                bmp->m_width,
			                                bmp->m_height);
			if(!p.m_texture)
			{
				spdlog::warn("window: no texture for a picture: {}",
				             SDL_GetError());
				return nullptr;
			}
			SDL_SetTextureBlendMode(p.m_texture, SDL_BLENDMODE_BLEND);
			SDL_SetTextureScaleMode(p.m_texture, SDL_SCALEMODE_LINEAR);
			p.m_w = bmp->m_width;
			p.m_h = bmp->m_height;
		}
		SDL_UpdateTexture(p.m_texture,
		                  nullptr,
		                  bmp->m_pixels.data(),
		                  bmp->m_width * 4);
		p.m_source = bmp.get();
		return p.m_texture;
	}

	void uploadAtlas()
	{
		const int n = m_font.atlasSize();
		if(!m_atlas || m_atlasSize != n)
		{
			if(m_atlas)
			{
				SDL_DestroyTexture(m_atlas);
			}
			m_atlas = SDL_CreateTexture(m_r,
			                            SDL_PIXELFORMAT_RGBA32,
			                            SDL_TEXTUREACCESS_STREAMING,
			                            n,
			                            n);
			SDL_SetTextureBlendMode(m_atlas, SDL_BLENDMODE_BLEND);
			SDL_SetTextureScaleMode(m_atlas, SDL_SCALEMODE_NEAREST);
			m_atlasSize = n;
			m_font.takeDirty();
			m_dirty = true;
		}
		if(!m_font.takeDirty() && !m_dirty)
		{
			return;
		}
		m_dirty = false;
		// White, the coverage in alpha: the vertex colour tints it.
		const auto& a = m_font.atlas();
		m_rgba.resize(a.size() * 4);
		for(size_t i = 0; i < a.size(); ++i)
		{
			m_rgba[4 * i + 0] = m_rgba[4 * i + 1] = m_rgba[4 * i + 2] = 255;
			m_rgba[4 * i + 3] = a[i];
		}
		SDL_UpdateTexture(m_atlas, nullptr, m_rgba.data(), n * 4);
	}

	void cell(const ScreenCell& px, int x, int y, int cw, int ch)
	{
		if(px.character == NATIVE_CELL)
		{
			return; // a picture shows here
		}
		Rgb fg = resolve(px.foreground_color).value_or(DEFAULT_FG);
		Rgb bg = resolve(px.background_color).value_or(DEFAULT_BG);
		if(px.inverted)
		{
			std::swap(fg, bg);
		}
		if(px.dim)
		{
			fg = mix(fg, bg, 0.45f);
		}
		const float x0 = static_cast<float>(x * cw),
		            y0 = static_cast<float>(y * ch);
		const float x1 = x0 + cw, y1 = y0 + ch;
		if(!(bg == DEFAULT_BG))
		{
			m_back.quad(x0, y0, x1, y1, fcolor(bg));
		}
		else
		{
			// Over a picture's leftover area too: always opaque.
			m_back.quad(x0, y0, x1, y1, fcolor(DEFAULT_BG));
		}
		if(px.underlined || px.underlined_double)
		{
			const float t = std::max(1.0f, ch / 16.0f);
			m_lines.quad(x0, y1 - 2 * t, x1, y1 - t, fcolor(fg));
		}
		if(px.strikethrough)
		{
			const float t = std::max(1.0f, ch / 16.0f);
			const float my = y0 + ch / 2.0f;
			m_lines.quad(x0, my - t / 2, x1, my + t / 2, fcolor(fg));
		}
		if(px.character.empty() || px.character == " ")
		{
			return;
		}
		const float inv = 1.0f / static_cast<float>(m_font.atlasSize());
		float pen = x0;
		for(char32_t cp : decodeUtf8(px.character))
		{
			// Combining marks (and variation selectors) ride on the base.
			if(cp >= 0x0300 && cp <= 0x036F)
			{
				continue;
			}
			if(cp >= 0xFE00 && cp <= 0xFE0F)
			{
				continue;
			}
			const Font::Glyph* g = m_font.glyph(cp, px.bold);
			if(g)
			{
				const float gx = pen + g->m_offX, gy = y0 + g->m_offY;
				m_glyphs.quad(gx,
				              gy,
				              gx + g->m_w,
				              gy + g->m_h,
				              fcolor(fg),
				              g->m_x * inv,
				              g->m_y * inv,
				              (g->m_x + g->m_w) * inv,
				              (g->m_y + g->m_h) * inv);
			}
			pen += cw;
		}
	}

	SDL_Renderer* m_r;
	Font& m_font;
	SDL_Texture* m_atlas = nullptr;
	int m_atlasSize = 0;
	bool m_dirty = true;
	std::vector<uint8_t> m_rgba;
	std::unordered_map<uint32_t, Picture> m_pictures;
	Batch m_back, m_glyphs, m_lines;
};

// What the Open dialogs hand back (on any thread): the paths, to the loop.
struct DialogTarget
{
	WindowHost* m_host = nullptr;
	App* m_app = nullptr;
};

void dialogDone(void* userdata, const char* const* files, int /*filter*/)
{
	auto* t = static_cast<DialogTarget*>(userdata);
	if(!files)
	{
		spdlog::warn("open dialog: {}", SDL_GetError());
		return;
	}
	std::vector<std::string> paths;
	for(const char* const* f = files; *f; ++f)
	{
		paths.emplace_back(*f);
	}
	if(!paths.empty())
	{
		t->m_host->post([t, paths = std::move(paths)]
		                { t->m_app->openPaths(paths); });
	}
}

} // namespace

namespace
{

/// One `--gui-script` line: its command and words.
struct ScriptLine
{
	std::string m_op;
	std::vector<std::string> m_args;
};

std::vector<ScriptLine> loadScript(const std::string& file)
{
	std::vector<ScriptLine> out;
	std::ifstream in(file);
	std::string line;
	while(std::getline(in, line))
	{
		std::istringstream words(line);
		ScriptLine l;
		if(!(words >> l.m_op) || l.m_op.starts_with('#'))
		{
			continue;
		}
		for(std::string w; words >> w;)
		{
			l.m_args.push_back(w);
		}
		out.push_back(std::move(l));
	}
	return out;
}

float scriptNumber(const ScriptLine& l, size_t i)
{
	return i < l.m_args.size() ? std::strtof(l.m_args[i].c_str(), nullptr)
	                           : 0.0f;
}

} // namespace

int run(const AppOptions& given, const GuiOptions& gui)
{
	log::redirectToFile();
	SDL_SetAppMetadata("rega", REGA_VERSION, "io.github.elyg.rega");
	if(!SDL_Init(SDL_INIT_VIDEO))
	{
		spdlog::error("window: SDL_Init: {}", SDL_GetError());
		std::fprintf(stderr,
		             "rega: cannot open a window: %s\n",
		             SDL_GetError());
		return 1;
	}
	// Colours as given: FTXUI would round them to what a terminal shows,
	// and the cells under a picture carry its id in theirs.
	ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);

	const fs::path settingsPath = Settings::defaultPath();
	Settings settings = Settings::load(settingsPath);
	const bool shot = !gui.m_screenshot.empty();
	// --gui-script: input and frames from a file, offscreen as a shot is.
	const std::vector<ScriptLine> script = gui.m_script.empty()
	                                           ? std::vector<ScriptLine>{}
	                                           : loadScript(gui.m_script);
	const bool scripted = !gui.m_script.empty();
	SDL_WindowFlags flags =
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if(settings.m_maximized && !shot && !scripted)
	{
		flags |= SDL_WINDOW_MAXIMIZED;
	}
	if(shot || scripted)
	{
		flags |= SDL_WINDOW_HIDDEN;
	}
	SDL_Window* window =
	    SDL_CreateWindow("rega", settings.m_width, settings.m_height, flags);
	if(!window)
	{
		spdlog::error("window: {}", SDL_GetError());
		std::fprintf(stderr,
		             "rega: cannot open a window: %s\n",
		             SDL_GetError());
		SDL_Quit();
		return 1;
	}
	SDL_SetWindowMinimumSize(window, 480, 320);
	// The taskbar's / dock's icon where no launcher gives one (X11, Wayland).
	int iw = 0, ih = 0, n = 0;
	if(stbi_uc* px =
	       stbi_load_from_memory(assets::ICON_PNG,
	                             static_cast<int>(assets::ICON_PNG_SIZE),
	                             &iw,
	                             &ih,
	                             &n,
	                             4))
	{
		if(SDL_Surface* icon =
		       SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, px, iw * 4))
		{
			SDL_SetWindowIcon(window, icon);
			SDL_DestroySurface(icon);
		}
		stbi_image_free(px);
	}
	SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
	if(!renderer)
	{
		spdlog::error("window: renderer: {}", SDL_GetError());
		std::fprintf(stderr,
		             "rega: cannot draw in a window: %s\n",
		             SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}
	SDL_SetRenderVSync(renderer, 1);
	spdlog::info("window: SDL {} ({}), renderer {}",
	             SDL_GetVersion(),
	             SDL_GetCurrentVideoDriver(),
	             SDL_GetRendererName(renderer));

	Font font({assets::FONT_REGULAR, assets::FONT_REGULAR_SIZE},
	          {assets::FONT_BOLD, assets::FONT_BOLD_SIZE});
	MouseCells mouse;
	auto applyFont = [&]
	{
		const float scale = std::max(0.5f, SDL_GetWindowDisplayScale(window));
		font.setPixelHeight(settings.m_fontSize * scale);
		mouse.setGeometry(font.cellW(),
		                  font.cellH(),
		                  SDL_GetWindowPixelDensity(window));
	};
	applyFont();

	NativeImages native;
	TermCaps caps;
	caps.m_graphics = GraphicsMode::NATIVE;
	caps.m_cellW = font.cellW();
	caps.m_cellH = font.cellH();
	caps.m_native = &native;
	caps.m_terminalName = "window";

	auto cellsNow = [&]
	{
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(window, &pw, &ph);
		return ftxui::Dimensions{std::max(1, pw / font.cellW()),
		                         std::max(1, ph / font.cellH())};
	};
	ui::setScreenSize(cellsNow());

	// Nothing to open: the start screen, the browser behind it where it was
	// last time (from a launcher the current directory is the install
	// folder, or /), else home.
	AppOptions opts = given;
	if(!shot && !scripted && !opts.m_tutorial)
	{
		opts.m_recentFile = Recent::defaultPath();
	}
	if(opts.m_paths.empty() && opts.m_args.empty())
	{
		opts.m_start = true;
		std::error_code ec;
		if(!settings.m_lastDir.empty() &&
		   fs::is_directory(settings.m_lastDir, ec))
		{
			opts.m_paths.push_back(settings.m_lastDir.string());
		}
		else if(const fs::path home = paths::home(); !home.empty())
		{
			opts.m_paths.push_back(home.string());
		}
	}

	int exitCode = 0;
	{
		WindowHost host;
		Painter painter(renderer, font);
		App app(opts, host, caps);
		DialogTarget dialog{&host, &app};
		std::string dialogDir; // outlives the dialog
		host.setDialog(
		    [&](bool folder)
		    {
			    static const SDL_DialogFileFilter FILTERS[] = {
			        {"Images and movies", "exr;tif;tiff;png;jpg;jpeg;svg;mov;mp4"},
			        {"All files", "*"},
			    };
			    dialogDir = app.browserDir().string();
			    const char* at =
			        dialogDir.empty() ? nullptr : dialogDir.c_str();
			    if(folder)
			    {
				    SDL_ShowOpenFolderDialog(dialogDone,
				                             &dialog,
				                             window,
				                             at,
				                             false);
			    }
			    else
			    {
				    SDL_ShowOpenFileDialog(
				        dialogDone, &dialog, window, FILTERS, 2, at, true);
			    }
		    });
		SDL_StartTextInput(window);

		std::vector<std::string> dropped;
		size_t scriptPc = 0;
		bool scriptDone = false;
		auto scriptAt = std::chrono::steady_clock::now();
		std::string scriptShot, scriptFramesDir;
		int scriptFramesLeft = 0, scriptFrame = 0;
		std::string title;
		bool dirty = true;
		auto lastActivity = std::chrono::steady_clock::now();
		const auto started = lastActivity;
		int frames = 0;

		auto send = [&](const ftxui::Event& e)
		{
			(void)app.onEvent(e);
			dirty = true;
		};
		auto pointer = [&](float x, float y)
		{
			const auto [cx, cy] = mouse.cells(x, y);
			app.setPointer(cx, cy);
		};
		auto refont = [&]
		{
			applyFont();
			app.setCellSize(font.cellW(), font.cellH());
			dirty = true;
		};

		while(!host.quitting())
		{
			SDL_Event ev;
			bool got = dirty      ? SDL_PollEvent(&ev)
			           : scripted ? SDL_WaitEventTimeout(&ev, 4)
			           : shot     ? SDL_WaitEventTimeout(&ev, 50)
			                      : SDL_WaitEvent(&ev);
			bool closing = false;
			while(got)
			{
				const SDL_Keymod mod = SDL_GetModState();
				switch(ev.type)
				{
					case SDL_EVENT_QUIT:
					case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
						closing = true;
						break;
					case SDL_EVENT_KEY_DOWN:
					{
						switch(command(ev.key.key, ev.key.mod))
						{
							case Command::FONT_BIGGER:
								settings.m_fontSize =
								    std::min(Settings::MAX_FONT_SIZE,
								             settings.m_fontSize + 1);
								refont();
								break;
							case Command::FONT_SMALLER:
								settings.m_fontSize =
								    std::max(Settings::MIN_FONT_SIZE,
								             settings.m_fontSize - 1);
								refont();
								break;
							case Command::FONT_RESET:
								settings.m_fontSize =
								    Settings::DEFAULT_FONT_SIZE;
								refont();
								break;
							case Command::OPEN_FILES:
								host.openDialog(false);
								break;
							case Command::OPEN_FOLDER:
								host.openDialog(true);
								break;
							case Command::PASTE:
								if(char* t = SDL_GetClipboardText())
								{
									app.pasteText(t);
									SDL_free(t);
									dirty = true;
								}
								break;
							case Command::FULLSCREEN:
								SDL_SetWindowFullscreen(
								    window,
								    (SDL_GetWindowFlags(window) &
								     SDL_WINDOW_FULLSCREEN) == 0);
								break;
							case Command::NONE:
								if(auto e = keyEvent(ev.key.key, ev.key.mod))
								{
									send(*e);
								}
								break;
						}
						break;
					}
					case SDL_EVENT_TEXT_INPUT:
						for(const auto& e : textEvents(ev.text.text))
						{
							send(e);
						}
						break;
					case SDL_EVENT_MOUSE_MOTION:
						pointer(ev.motion.x, ev.motion.y);
						if(auto m = mouse.moved(ev.motion.x, ev.motion.y, mod))
						{
							send(ftxui::Event::Mouse("", *m));
						}
						break;
					case SDL_EVENT_MOUSE_BUTTON_DOWN:
					case SDL_EVENT_MOUSE_BUTTON_UP:
						pointer(ev.button.x, ev.button.y);
						if(auto m =
						       mouse.button(ev.button.button,
						                    ev.type ==
						                        SDL_EVENT_MOUSE_BUTTON_DOWN,
						                    ev.button.x,
						                    ev.button.y,
						                    mod))
						{
							send(ftxui::Event::Mouse("", *m));
						}
						break;
					case SDL_EVENT_MOUSE_WHEEL:
					{
						pointer(ev.wheel.mouse_x, ev.wheel.mouse_y);
						// Over the image: zoom by what was turned (a touchpad's
						// fractions too), smoothly; else notches, as a
						// terminal sends them.
						const auto [cx, cy] =
						    mouse.cells(ev.wheel.mouse_x, ev.wheel.mouse_y);
						if(app.wheelZoom(ev.wheel.y,
						                 static_cast<int>(std::floor(cx)),
						                 static_cast<int>(std::floor(cy))))
						{
							dirty = true;
							break;
						}
						for(const auto& m : mouse.wheel(ev.wheel.y,
						                                ev.wheel.mouse_x,
						                                ev.wheel.mouse_y,
						                                mod))
						{
							send(ftxui::Event::Mouse("", m));
						}
						break;
					}
					case SDL_EVENT_DROP_BEGIN:
						dropped.clear();
						break;
					case SDL_EVENT_DROP_FILE:
						if(ev.drop.data)
						{
							dropped.emplace_back(ev.drop.data);
						}
						break;
					case SDL_EVENT_DROP_COMPLETE:
						app.openPaths(dropped);
						dropped.clear();
						dirty = true;
						break;
					case SDL_EVENT_DROP_TEXT: // text dragged in: as pasted
						if(ev.drop.data)
						{
							app.pasteText(ev.drop.data);
							dirty = true;
						}
						break;
					case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
						refont();
						break;
					case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					case SDL_EVENT_WINDOW_EXPOSED:
					case SDL_EVENT_WINDOW_RESTORED:
					case SDL_EVENT_WINDOW_MAXIMIZED:
						dirty = true;
						break;
					default:
						break;
				}
				got = SDL_PollEvent(&ev);
			}
			if(closing)
			{
				break;
			}
			// As in FTXUI, a task brings no redraw of its own: one that
			// changes what shows asks for it (Host::redraw()).
			host.runTasks();
			if(host.takeRedraw())
			{
				send(
				    ftxui::Event::Custom); // the playback tick, a picture ready
				lastActivity = std::chrono::steady_clock::now();
			}
			if(frames == 1 && !gui.m_keys.empty())
			{
				// --gui-keys: typed once the first frame is up.
				for(char32_t cp : decodeUtf8(gui.m_keys))
				{
					if(cp == U'\n')
					{
						send(ftxui::Event::Return);
					}
					else if(cp == U'\x1b')
					{
						send(ftxui::Event::Escape);
					}
					else
					{
						for(const auto& e :
						    textEvents(std::string(1, static_cast<char>(cp))))
						{
							send(e);
						}
					}
				}
				lastActivity = std::chrono::steady_clock::now();
			}
			// --gui-script: the next line once the last wait is over.
			if(scripted && frames > 0 && !scriptDone &&
			   std::chrono::steady_clock::now() >= scriptAt)
			{
				if(scriptPc >= script.size())
				{
					scriptDone = true;
					host.quit();
				}
				else
				{
					const ScriptLine& l = script[scriptPc++];
					const SDL_WindowID id = SDL_GetWindowID(window);
					auto push = [&](SDL_Event e) { SDL_PushEvent(&e); };
					if(l.m_op == "wait")
					{
						scriptAt = std::chrono::steady_clock::now() +
						           std::chrono::milliseconds(
						               static_cast<int>(scriptNumber(l, 0)));
					}
					else if(l.m_op == "wheel")
					{
						SDL_Event e{};
						e.type = SDL_EVENT_MOUSE_WHEEL;
						e.wheel.windowID = id;
						e.wheel.mouse_x = scriptNumber(l, 0);
						e.wheel.mouse_y = scriptNumber(l, 1);
						e.wheel.y = scriptNumber(l, 2);
						push(e);
					}
					else if(l.m_op == "drag" || l.m_op == "rdrag")
					{
						const float x0 = scriptNumber(l, 0),
						            y0 = scriptNumber(l, 1),
						            x1 = scriptNumber(l, 2),
						            y1 = scriptNumber(l, 3);
						const int steps =
						    std::max(1, static_cast<int>(scriptNumber(l, 4)));
						SDL_Event e{};
						e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
						e.button.windowID = id;
						e.button.button = l.m_op == "rdrag" ? SDL_BUTTON_RIGHT
						                                    : SDL_BUTTON_LEFT;
						e.button.down = true;
						e.button.x = x0;
						e.button.y = y0;
						push(e);
						for(int k = 1; k <= steps; ++k)
						{
							SDL_Event m{};
							m.type = SDL_EVENT_MOUSE_MOTION;
							m.motion.windowID = id;
							m.motion.x = x0 + (x1 - x0) * k / steps;
							m.motion.y = y0 + (y1 - y0) * k / steps;
							push(m);
						}
						e.type = SDL_EVENT_MOUSE_BUTTON_UP;
						e.button.down = false;
						e.button.x = x1;
						e.button.y = y1;
						push(e);
					}
					else if(l.m_op == "shot" && !l.m_args.empty())
					{
						scriptShot = l.m_args[0];
						dirty = true;
					}
					else if(l.m_op == "frames" && l.m_args.size() >= 2)
					{
						scriptFramesDir = l.m_args[0];
						scriptFramesLeft = static_cast<int>(scriptNumber(l, 1));
						std::error_code ec;
						fs::create_directories(scriptFramesDir, ec);
					}
				}
			}
			// --gui-screenshot: once nothing has happened for a moment (the
			// pictures are in), or at the latest after 20 s.
			const auto now = std::chrono::steady_clock::now();
			const bool takeShot =
			    shot && frames > 0 &&
			    (now - lastActivity > std::chrono::milliseconds(800) ||
			     now - started > std::chrono::seconds(20));
			if(!dirty && !takeShot)
			{
				continue;
			}
			dirty = false;

			const ftxui::Dimensions cells = cellsNow();
			ui::setScreenSize(cells);
			ftxui::Element root = app.render();
			ftxui::Screen screen(cells.dimx, cells.dimy);
			native.begin();
			ftxui::Render(screen, root);
			painter.paint(screen, native);
			++frames;
			if(const std::string t = app.title(); t != title)
			{
				title = t;
				SDL_SetWindowTitle(window, title.c_str());
			}

			// --gui-script: what it asked to see.
			if(!scriptShot.empty() || scriptFramesLeft > 0)
			{
				std::vector<uint8_t> rgba;
				int w = 0, h = 0;
				if(painter.read(rgba, w, h))
				{
					std::string out = scriptShot;
					if(out.empty())
					{
						char name[16];
						std::snprintf(name,
						              sizeof name,
						              "%04d.png",
						              scriptFrame++);
						out = (fs::path(scriptFramesDir) / name).string();
						--scriptFramesLeft;
					}
					stbi_write_png(out.c_str(), w, h, 4, rgba.data(), w * 4);
				}
				scriptShot.clear();
			}
			if(takeShot)
			{
				std::vector<uint8_t> rgba;
				int w = 0, h = 0;
				if(painter.read(rgba, w, h) &&
				   stbi_write_png(
				       gui.m_screenshot.c_str(), w, h, 4, rgba.data(), w * 4))
				{
					spdlog::info("window: screenshot {}x{} to {}",
					             w,
					             h,
					             gui.m_screenshot);
				}
				else
				{
					std::fprintf(stderr,
					             "rega: could not save the screenshot to %s\n",
					             gui.m_screenshot.c_str());
					exitCode = 1;
				}
				SDL_RenderPresent(renderer);
				break;
			}
			SDL_RenderPresent(renderer);
		}

		SDL_StopTextInput(window);
		// Remember the window as it was left.
		if(!shot && !scripted)
		{
			const SDL_WindowFlags f = SDL_GetWindowFlags(window);
			settings.m_maximized = (f & SDL_WINDOW_MAXIMIZED) != 0;
			if(!settings.m_maximized && (f & SDL_WINDOW_FULLSCREEN) == 0)
			{
				SDL_GetWindowSize(window,
				                  &settings.m_width,
				                  &settings.m_height);
			}
			// The tutorial's folder is no place to come back to.
			if(!app.browserDir().empty() &&
			   app.rememberable(app.browserDir().string()))
			{
				settings.m_lastDir = app.browserDir();
			}
			settings.save(settingsPath);
		}
	}
	ui::setScreenSize(std::nullopt);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	spdlog::info("exit");
	return exitCode;
}

} // namespace rega::gui
