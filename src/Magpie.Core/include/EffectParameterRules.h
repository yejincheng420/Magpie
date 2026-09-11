#pragma once
#include <string_view>
#include "FramePacingOptions.h"

namespace Magpie {

inline bool IsSuperResolutionEffect(std::string_view effect) noexcept {
	return effect == "DLSS\\DLSS_SR" || effect == "FSR2\\FSR2_SR" ||
		effect == "FSR3\\FSR3_SR" || effect == "FSR4\\FSR4_SR" || effect == "XeSS\\XeSS_SR";
}

inline bool HasOpticalFlowSelection(std::string_view effect) noexcept {
	return IsSuperResolutionEffect(effect) ||
		effect == "DLSSNR\\DLSSNR_AI_Filter" ||
		effect == "DLSSFG\\DLSS_FrameGeneration" ||
		effect == "Diagnostics\\FrameGuidance_Motion" ||
		effect == "Diagnostics\\FrameGuidance_Confidence" ||
		effect == "XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV" ||
		effect == "XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV";
}

// Restrict built-in UI rules to their owning effects. Custom effects may use
// the same parameter names with completely different meanings.
template<class GetValue>
bool IsEffectParameterVisible(std::string_view effect, std::string_view parameter,
	GetValue&& getValue) noexcept {
	if (HasOpticalFlowSelection(effect)) {
		const float method = getValue("opticalFlowMethod", 0.0f);
		if (parameter == "amdOpticalFlowMode") return method == 1.0f;
		if (parameter == "nvidiaOpticalFlowQuality") return method == 2.0f;
	}
	if (effect == "DLSSNR\\DLSSNR_AI_Filter" &&
		(parameter == "samplingQuality" || parameter == "inputResolutionPercent" ||
		 parameter == "residualMultiplier" || parameter == "residualSaturation" ||
		 parameter == "residualLightness" ||
		 parameter == "shadowStructureMultiplier" || parameter == "reflectionGlowMultiplier")) {
		return getValue("enableInputResolutionScaling", 0.0f) != 0.0f;
	}
	return true;
}

template<class GetValue>
bool IsEffectParameterEnabled(std::string_view effect, std::string_view parameter,
	bool frontEdgeSyncEnabled, GetValue&& getValue) noexcept {
	if (IsFrameRateFilterEffect(effect)) {
		if (parameter == "frameRateMode") return !frontEdgeSyncEnabled;
		if (parameter == "targetFrameRate") {
			return !UsesFrontEdgeSyncFrameRate(frontEdgeSyncEnabled, getValue("frameRateMode", 0.0f));
		}
	}
	return true;
}

}
