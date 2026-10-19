#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rv
{

/// An OCIO display transform (input colour space → display / view, plus a
/// look) baked into a 3D LUT, so a frame costs a lookup per pixel whatever
/// the transform (an ACES 2.0 view per pixel on the CPU is too slow to
/// play). A shaper spreads the input over the LUT's axes. Immutable: safe
/// from any thread.
class ColourTransform
{
public:
	static constexpr int SIZE = 65; ///< LUT points along each axis
	/// The shaper's range for scene-linear input, in stops (log2): from far
	/// below 18% grey to well into the highlights.
	static constexpr float LOG_LO = -12.5f, LOG_HI = 6.5f;

	/// How input values map to LUT coordinates.
	enum class Shaper
	{
		LINEAR, ///< [0, 1] as is: display-referred input
		LOG2,   ///< scene-linear input, LOG_LO to LOG_HI stops
		SRGB    ///< pixels decoded from sRGB on load: back to the stored code
	};

	/// `lut`: SIZE^3 RGB triples, red fastest, sampled at shaper() inverse.
	ColourTransform(Shaper shaper, std::vector<float> lut);

	/// Display-referred RGB for scene (file) RGB, each in [0, 1].
	void apply(float& r, float& g, float& b) const;
	/// Input value → LUT coordinate in [0, 1].
	float shaper(float v) const;
	/// The input value LUT coordinate `t` stands for (shaper()'s inverse).
	float unshaper(float t) const;

private:
	Shaper m_shaper;
	std::vector<float> m_lut;
};
using ColourTransformPtr = std::shared_ptr<const ColourTransform>;

/// OpenColorIO: the config in use ($OCIO, one of OCIO's built-in ones, or a
/// file), the display / view / look picked from it, each image's input
/// colour space (the config's file rules, unless overridden per source) and
/// the baked transforms. Without a config rvtui keeps its plain sRGB view.
/// Choices are remembered per config in a small state file. UI thread only.
class ColourManager
{
public:
	/// A config to pick: what to load, and how to show it.
	struct Choice
	{
		std::string m_id; ///< path or ocio:// URI; empty = none
		std::string m_label;
	};
	static constexpr const char* STUDIO = "ocio://studio-config-latest";
	static constexpr const char* CG = "ocio://cg-config-latest";

	/// @param statePath where choices are kept (empty: not kept).
	explicit ColourManager(std::filesystem::path statePath = {});
	~ColourManager();
	ColourManager(const ColourManager&) = delete;
	ColourManager& operator=(const ColourManager&) = delete;

	/// $XDG_STATE_HOME/rvtui/colour (~/.local/state/rvtui/colour).
	static std::filesystem::path defaultStatePath();

	/// Start-up: $OCIO when set, else the config picked last time, else
	/// none (plain sRGB).
	void loadDefault();
	/// Use config `id` (a path or ocio:// URI; empty = none) with its
	/// remembered (or default) display, view and look. On failure the
	/// current one stays and `error()` says why.
	bool useConfig(const std::string& id);

	bool active() const noexcept;
	const std::string& configId() const noexcept
	{
		return m_configId;
	}
	/// Short name of the config ("studio-config-v2.2.0_aces-v1.3…", the
	/// file name, or "none (sRGB)").
	std::string configLabel() const;
	/// The last problem loading a config or building a transform.
	const std::string& error() const noexcept
	{
		return m_error;
	}
	/// $OCIO (when set), OCIO's built-in configs, and none.
	std::vector<Choice> configChoices() const;

	std::vector<std::string> displays() const;
	std::vector<std::string> views() const; ///< of the current display
	std::vector<std::string> looks() const; ///< "None" first
	std::vector<std::string> colourSpaces() const;
	const std::string& display() const noexcept
	{
		return m_display;
	}
	const std::string& view() const noexcept
	{
		return m_view;
	}
	const std::string& look() const noexcept
	{
		return m_look;
	}
	/// A new display keeps the view if it has it, else takes its default.
	void setDisplay(const std::string& display);
	void setView(const std::string& view);
	void setLook(const std::string& look); ///< "None" or empty: no look

	/// Input colour space of `path` (shown as source `sourceKey`): the
	/// override, else the config's file rules.
	std::string inputFor(const std::filesystem::path& path,
	                     const std::string& sourceKey) const;
	bool overridden(const std::string& sourceKey) const;
	/// What the config's file rules say `path` is ("" without a config).
	std::string fileRuleFor(const std::filesystem::path& path) const;
	/// Override the input colour space of a source (nullopt: back to the
	/// file rules). For this session only.
	void setInput(const std::string& sourceKey,
	              std::optional<std::string> colourSpace);

	/// The transform for `path`; nullptr without a config (or when it
	/// cannot be built: see error()). `srgbDecoded`: its pixels were
	/// linearised from sRGB codes on load (ImageInfo::m_displayReferred);
	/// the input colour space then applies to those codes, not decoded twice.
	ColourTransformPtr transformFor(const std::filesystem::path& path,
	                                const std::string& sourceKey,
	                                bool srgbDecoded);

private:
	struct Impl;
	void loadState();
	void saveState() const;
	/// Display / view / look from the state for the config, else defaults.
	void pickRemembered();

	std::filesystem::path m_statePath;
	std::unique_ptr<Impl> m_impl; ///< the OCIO config (none: null inside)
	std::string m_configId;
	std::string m_display, m_view, m_look;
	std::string m_error;
	std::string m_lastConfig; ///< from the state: picked when $OCIO is unset
	/// Per config id: display, view, look.
	std::map<std::string, std::vector<std::string>> m_remembered;
	std::map<std::string, std::string> m_inputs; ///< source key → override
	/// By input colour space and srgbDecoded.
	std::map<std::pair<std::string, bool>, ColourTransformPtr> m_transforms;
};

} // namespace rv
