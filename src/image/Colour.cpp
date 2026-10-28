#include "image/Colour.h"

#include "image/Loader.h"

#include <OpenColorIO/OpenColorIO.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace OCIO = OCIO_NAMESPACE;

namespace rega
{

// --- ColourTransform ---

namespace
{

// Linear [0, 1] → sRGB code, per pixel: a lookup (64K entries keep the steep
// toe well under one 8-bit step, as Render's display table does).
float srgbCode(float v)
{
	constexpr int N = 65536;
	static const std::vector<float> TABLE = []
	{
		std::vector<float> t(N);
		for(int i = 0; i < N; ++i)
		{
			const float x = static_cast<float>(i) / (N - 1);
			t[i] = x <= 0.0031308f ? x * 12.92f
			                       : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
		}
		return t;
	}();
	if(!(v > 0.0f)) // also NaN
	{
		return 0.0f;
	}
	return v >= 1.0f ? 1.0f : TABLE[static_cast<int>(v * (N - 1) + 0.5f)];
}

} // namespace

ColourTransform::ColourTransform(Shaper shaper, std::vector<float> lut)
    : m_shaper(shaper), m_lut(std::move(lut))
{
}

float ColourTransform::shaper(float v) const
{
	switch(m_shaper)
	{
		case Shaper::LOG2:
		{
			const float stops = v > 0.0f ? std::log2(v) : LOG_LO;
			return std::clamp((stops - LOG_LO) / (LOG_HI - LOG_LO), 0.0f, 1.0f);
		}
		case Shaper::SRGB:
			return srgbCode(v);
		case Shaper::LINEAR:
			break;
	}
	return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

float ColourTransform::unshaper(float t) const
{
	switch(m_shaper)
	{
		case Shaper::LOG2:
			return std::exp2(LOG_LO + t * (LOG_HI - LOG_LO));
		case Shaper::SRGB:
			return srgbToLinear(t);
		case Shaper::LINEAR:
			break;
	}
	return t;
}

void ColourTransform::apply(float& r, float& g, float& b) const
{
	constexpr int N = SIZE;
	// Trilinear between the 8 points around the shaped colour.
	auto locate = [](float t, int& i, float& f)
	{
		const float x = t * (N - 1);
		i = std::min(static_cast<int>(x), N - 2);
		f = x - static_cast<float>(i);
	};
	int ri, gi, bi;
	float rf, gf, bf;
	locate(shaper(r), ri, rf);
	locate(shaper(g), gi, gf);
	locate(shaper(b), bi, bf);
	const float* base =
	    &m_lut[((static_cast<size_t>(bi) * N + gi) * N + ri) * 3];
	const size_t dr = 3, dg = 3 * N, db = 3 * N * N;
	float out[3];
	for(int c = 0; c < 3; ++c)
	{
		const float* p = base + c;
		const float c00 = p[0] + (p[dr] - p[0]) * rf;
		const float c10 = p[dg] + (p[dg + dr] - p[dg]) * rf;
		const float c01 = p[db] + (p[db + dr] - p[db]) * rf;
		const float c11 = p[db + dg] + (p[db + dg + dr] - p[db + dg]) * rf;
		const float c0 = c00 + (c10 - c00) * gf;
		const float c1 = c01 + (c11 - c01) * gf;
		out[c] = std::clamp(c0 + (c1 - c0) * bf, 0.0f, 1.0f);
	}
	r = out[0];
	g = out[1];
	b = out[2];
}

// --- ColourManager ---

struct ColourManager::Impl
{
	OCIO::ConstConfigRcPtr m_config;
};

namespace
{

constexpr const char* NO_LOOK = "None";

// Tab-separated fields of a state line.
std::vector<std::string> fields(const std::string& line)
{
	std::vector<std::string> out;
	std::stringstream ss(line);
	std::string f;
	while(std::getline(ss, f, '\t'))
	{
		out.push_back(f);
	}
	return out;
}

} // namespace

ColourManager::ColourManager(fs::path statePath)
    : m_statePath(std::move(statePath)), m_impl(std::make_unique<Impl>())
{
	loadState();
}

ColourManager::~ColourManager() = default;

fs::path ColourManager::defaultStatePath()
{
	fs::path base;
	if(const char* x = std::getenv("XDG_STATE_HOME"); x && *x)
	{
		base = x;
	}
	else if(const char* home = std::getenv("HOME"); home && *home)
	{
		base = fs::path(home) / ".local" / "state";
	}
	else
	{
		return {};
	}
	return base / "rega" / "colour";
}

void ColourManager::loadState()
{
	if(m_statePath.empty())
	{
		return;
	}
	std::ifstream in(m_statePath);
	std::string line;
	while(std::getline(in, line))
	{
		const auto f = fields(line);
		if(f.size() == 2 && f[0] == "config")
		{
			m_lastConfig = f[1];
		}
		else if(f.size() == 5 && f[0] == "view")
		{
			m_remembered[f[1]] = {f[2], f[3], f[4]};
		}
	}
}

void ColourManager::saveState() const
{
	if(m_statePath.empty())
	{
		return;
	}
	std::error_code ec;
	fs::create_directories(m_statePath.parent_path(), ec);
	std::ofstream out(m_statePath);
	// $OCIO is picked by the environment, not remembered as the choice.
	const char* env = std::getenv("OCIO");
	if(!(env && *env && m_configId == env))
	{
		out << "config\t" << m_configId << "\n";
	}
	else if(!m_lastConfig.empty())
	{
		out << "config\t" << m_lastConfig << "\n";
	}
	for(const auto& [id, v] : m_remembered)
	{
		if(v.size() == 3)
		{
			out << "view\t" << id << "\t" << v[0] << "\t" << v[1] << "\t"
			    << v[2] << "\n";
		}
	}
	if(!out)
	{
		spdlog::warn("colour: cannot save {}", m_statePath.string());
	}
}

void ColourManager::loadDefault()
{
	std::string envError;
	if(const char* env = std::getenv("OCIO"); env && *env)
	{
		if(useConfig(env))
		{
			return;
		}
		envError = fmt::format("$OCIO {}: {}", env, m_error);
		spdlog::warn("colour: {}", envError);
	}
	if(!m_lastConfig.empty() && !useConfig(m_lastConfig))
	{
		spdlog::warn("colour: {}: {}", m_lastConfig, m_error);
	}
	if(!envError.empty())
	{
		m_error = envError; // still worth saying, whatever loaded instead
	}
}

bool ColourManager::useConfig(const std::string& id)
{
	if(id.empty())
	{
		m_impl->m_config.reset();
		m_configId.clear();
		m_display = m_view = m_look = "";
		m_transforms.clear();
		m_error.clear();
		m_lastConfig.clear();
		saveState();
		return true;
	}
	try
	{
		OCIO::ConstConfigRcPtr config =
		    OCIO::Config::CreateFromFile(id.c_str());
		m_impl->m_config = config;
		m_configId = id;
		m_transforms.clear();
		m_error.clear();
		pickRemembered();
		const char* env = std::getenv("OCIO");
		if(!(env && *env && id == env))
		{
			m_lastConfig = id;
		}
		saveState();
		spdlog::info("colour: {} ({} / {})", id, m_display, m_view);
		return true;
	}
	catch(const OCIO::Exception& e)
	{
		m_error = e.what();
		return false;
	}
}

void ColourManager::pickRemembered()
{
	const auto& cfg = m_impl->m_config;
	m_display = cfg->getDefaultDisplay();
	m_view = cfg->getDefaultView(m_display.c_str());
	m_look = NO_LOOK;
	auto it = m_remembered.find(m_configId);
	if(it == m_remembered.end())
	{
		return;
	}
	const auto ds = displays();
	if(std::ranges::find(ds, it->second[0]) != ds.end())
	{
		m_display = it->second[0];
		m_view = cfg->getDefaultView(m_display.c_str());
	}
	const auto vs = views();
	if(std::ranges::find(vs, it->second[1]) != vs.end())
	{
		m_view = it->second[1];
	}
	const auto ls = looks();
	if(std::ranges::find(ls, it->second[2]) != ls.end())
	{
		m_look = it->second[2];
	}
}

bool ColourManager::active() const noexcept
{
	return m_impl->m_config != nullptr;
}

std::string ColourManager::configLabel() const
{
	if(!active())
	{
		return "none (sRGB)";
	}
	if(m_configId.starts_with("ocio://"))
	{
		const char* name = m_impl->m_config->getName();
		return name && *name ? name : m_configId;
	}
	return fs::path(m_configId).filename().string();
}

std::vector<ColourManager::Choice> ColourManager::configChoices() const
{
	std::vector<Choice> out;
	if(const char* env = std::getenv("OCIO"); env && *env)
	{
		out.push_back({env, std::string("$OCIO ") + env});
	}
	out.push_back({STUDIO, "OCIO studio config (ACES, built in)"});
	out.push_back({CG, "OCIO CG config (ACES, built in)"});
	if(!m_configId.empty() &&
	   std::ranges::none_of(out,
	                        [&](const Choice& c)
	                        { return c.m_id == m_configId; }))
	{
		out.push_back({m_configId, m_configId});
	}
	out.push_back({"", "none (sRGB)"});
	return out;
}

std::vector<std::string> ColourManager::displays() const
{
	std::vector<std::string> out;
	if(const auto& cfg = m_impl->m_config)
	{
		for(int i = 0; i < cfg->getNumDisplays(); ++i)
		{
			out.emplace_back(cfg->getDisplay(i));
		}
	}
	return out;
}

std::vector<std::string> ColourManager::views() const
{
	std::vector<std::string> out;
	if(const auto& cfg = m_impl->m_config)
	{
		const int n = cfg->getNumViews(m_display.c_str());
		for(int i = 0; i < n; ++i)
		{
			out.emplace_back(cfg->getView(m_display.c_str(), i));
		}
	}
	return out;
}

std::vector<std::string> ColourManager::looks() const
{
	std::vector<std::string> out{NO_LOOK};
	if(const auto& cfg = m_impl->m_config)
	{
		for(int i = 0; i < cfg->getNumLooks(); ++i)
		{
			out.emplace_back(cfg->getLookNameByIndex(i));
		}
	}
	return out;
}

std::vector<std::string> ColourManager::colourSpaces() const
{
	std::vector<std::string> out;
	if(const auto& cfg = m_impl->m_config)
	{
		for(int i = 0; i < cfg->getNumColorSpaces(); ++i)
		{
			out.emplace_back(cfg->getColorSpaceNameByIndex(i));
		}
	}
	return out;
}

void ColourManager::setDisplay(const std::string& display)
{
	if(!active() || display == m_display)
	{
		return;
	}
	m_display = display;
	const auto vs = views();
	if(std::ranges::find(vs, m_view) == vs.end())
	{
		m_view = m_impl->m_config->getDefaultView(m_display.c_str());
	}
	m_transforms.clear();
	m_remembered[m_configId] = {m_display, m_view, m_look};
	saveState();
}

void ColourManager::setView(const std::string& view)
{
	if(!active() || view == m_view)
	{
		return;
	}
	m_view = view;
	m_transforms.clear();
	m_remembered[m_configId] = {m_display, m_view, m_look};
	saveState();
}

void ColourManager::setLook(const std::string& look)
{
	const std::string l = look.empty() ? NO_LOOK : look;
	if(!active() || l == m_look)
	{
		return;
	}
	m_look = l;
	m_transforms.clear();
	m_remembered[m_configId] = {m_display, m_view, m_look};
	saveState();
}

std::string ColourManager::inputFor(const fs::path& path,
                                    const std::string& sourceKey) const
{
	if(auto it = m_inputs.find(sourceKey); it != m_inputs.end())
	{
		return it->second;
	}
	return fileRuleFor(path);
}

std::string ColourManager::fileRuleFor(const fs::path& path) const
{
	const auto& cfg = m_impl->m_config;
	if(!cfg)
	{
		return "";
	}
	try
	{
		return cfg->getColorSpaceFromFilepath(path.string().c_str());
	}
	catch(const OCIO::Exception&)
	{
		return "";
	}
}

bool ColourManager::overridden(const std::string& sourceKey) const
{
	return m_inputs.contains(sourceKey);
}

void ColourManager::setInput(const std::string& sourceKey,
                             std::optional<std::string> colourSpace)
{
	if(colourSpace)
	{
		m_inputs[sourceKey] = *colourSpace;
	}
	else
	{
		m_inputs.erase(sourceKey);
	}
}

ColourTransformPtr ColourManager::transformFor(const fs::path& path,
                                               const std::string& sourceKey,
                                               bool srgbDecoded)
{
	const auto& cfg = m_impl->m_config;
	if(!cfg)
	{
		return nullptr;
	}
	const std::string input = inputFor(path, sourceKey);
	const std::pair key{input, srgbDecoded};
	if(auto it = m_transforms.find(key); it != m_transforms.end())
	{
		return it->second;
	}
	ColourTransformPtr t;
	try
	{
		auto dvt = OCIO::DisplayViewTransform::Create();
		dvt->setSrc(input.c_str());
		dvt->setDisplay(m_display.c_str());
		dvt->setView(m_view.c_str());
		auto pipeline = OCIO::LegacyViewingPipeline::Create();
		pipeline->setDisplayViewTransform(dvt);
		if(m_look != NO_LOOK)
		{
			pipeline->setLooksOverrideEnabled(true);
			pipeline->setLooksOverride(m_look.c_str());
		}
		auto cpu = pipeline->getProcessor(cfg, cfg->getCurrentContext())
		               ->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
		// Scene-linear input spans many stops: sample it in log2. Pixels
		// linearised from sRGB on load are sampled at their stored codes,
		// which are what OCIO is given: the input space describes the file.
		using Shaper = ColourTransform::Shaper;
		const Shaper shaper =
		    srgbDecoded ? Shaper::SRGB
		    : cfg->isColorSpaceLinear(input.c_str(),
		                              OCIO::REFERENCE_SPACE_SCENE)
		        ? Shaper::LOG2
		        : Shaper::LINEAR;
		ColourTransform shape(shaper, {});
		constexpr int N = ColourTransform::SIZE;
		std::vector<float> axis(N);
		for(int i = 0; i < N; ++i)
		{
			const float t = static_cast<float>(i) / (N - 1);
			axis[i] = srgbDecoded ? t : shape.unshaper(t);
		}
		std::vector<float> lut(static_cast<size_t>(N) * N * N * 3);
		size_t k = 0;
		for(int b = 0; b < N; ++b)
		{
			for(int g = 0; g < N; ++g)
			{
				for(int r = 0; r < N; ++r)
				{
					lut[k++] = axis[r];
					lut[k++] = axis[g];
					lut[k++] = axis[b];
				}
			}
		}
		OCIO::PackedImageDesc img(lut.data(), N * N, N, 3);
		cpu->apply(img);
		t = std::make_shared<const ColourTransform>(shaper, std::move(lut));
	}
	catch(const OCIO::Exception& e)
	{
		m_error = e.what();
		spdlog::warn("colour: {} → {} / {}: {}",
		             input,
		             m_display,
		             m_view,
		             e.what());
	}
	m_transforms[key] = t;
	return t;
}

} // namespace rega
