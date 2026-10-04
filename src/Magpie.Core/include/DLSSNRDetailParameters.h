#pragma once
#include <array>
#include <cmath>
#include <string_view>

namespace Magpie {
inline constexpr std::array<std::string_view, 12> DLSSNR_RESIDUAL_PARAMETERS{
	"residualMultiplier", "residualSaturation", "residualLightness",
	"shadowStructureMultiplier", "reflectionGlowMultiplier",
	"residualHueProtection", "residualDarkProtection", "residualHighlightProtection",
	"residualLocalCompression", "residualLowFrequencyGain", "residualDetailGain",
	"residualDebugView"
};
inline bool IsDLSSNRResidualParameter(std::string_view name) noexcept {
	for (auto value : DLSSNR_RESIDUAL_PARAMETERS) if (value == name) return true;
	return false;
}
inline bool IsDLSSNRAdvancedParameter(std::string_view name) noexcept {
	return name == "residualHueProtection" || name == "residualDarkProtection" ||
		name == "residualHighlightProtection" || name == "residualLocalCompression" ||
		name == "residualLowFrequencyGain" || name == "residualDetailGain" ||
		name == "residualDebugView";
}
// All configurations use one residual algorithm. Only obsolete display state
// and the former algorithm selector are removed; numeric controls are retained.
template<class Map>
bool NormalizeDLSSNRDetailParameters(Map& values) {
	bool changed = values.erase(L"residualColorMode") != 0;
	changed |= values.erase(L"residualChromaTemporalStrength") != 0;
	const auto protection = values.find(L"residualShowProtection");
	if (protection != values.end()) {
		if (std::isfinite(protection->second) && protection->second != 0) {
			values[L"residualShowAdvanced"] = 1.f;
		}
		values.erase(L"residualShowProtection");
		changed = true;
	}
	const auto advanced = values.find(L"residualShowAdvanced");
	if (advanced != values.end()) {
		const float normalized = std::isfinite(advanced->second) && advanced->second != 0 ? 1.f : 0.f;
		changed |= advanced->second != normalized;
		advanced->second = normalized;
	}
	return changed;
}
}
