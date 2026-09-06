#pragma once
#include "NativeEffectBackend.h"

namespace Magpie {

class DeviceResources;
class NgxD3D12Core;

struct DLSSNRSettings {
	bool enableInputResolutionScaling = false;
	// 0 = Performance（Lanczos2 降采样 + Catmull-Rom 残差上采样，原始行为）
	// 1 = Quality（Lanczos3 降采样 + Mitchell-Netravali 残差上采样，低输入分辨率下保留更多中频）
	// 2 = UltraPerformance（单趟 4-tap bilinear 降采样 + 硬件 bilinear 直读残差合成，
	//     无水平中间趟，面向 inputResolutionPercent < 35% 的极端低分辨率场景）
	uint32_t samplingQuality = 0;
	uint32_t inputResolutionPercent = 100;
	float residualMultiplier = 1.0f;
	float residualSaturation = 1.0f;
	float residualLightness = 1.0f;
	float shadowStructureMultiplier = 1.0f;
	float reflectionGlowMultiplier = 1.0f;
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float skinStructureStrength = -1.0f;
	bool useAutoMask = false;
	bool uiCorrection = false;
	NvidiaOpticalFlowQuality motionVectorQuality =
		NvidiaOpticalFlowQuality::Balanced;
};

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option) noexcept;

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame, so valid zero-filled motion/depth textures are used
// as explicit temporal guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	bool Drain() noexcept override;
	EffectParameterApplyMode GetParameterApplyMode(
		std::string_view parameterName
	) const noexcept override;
	EffectParameterRestartReason GetParameterRestartReason(
		std::string_view parameterName
	) const noexcept override;
	bool ApplyLiveParameters(
		const EffectOption& option,
		std::span<const std::string> parameterNames
	) noexcept override;

	bool Initialize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSNRSettings& settings
	) noexcept;

	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	std::unique_ptr<Impl> _impl;
	DLSSNRSettings _settings;
	NgxD3D12Core* _ngxCore = nullptr;
};

}
