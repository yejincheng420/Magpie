#pragma once
#include "NativeEffectBackend.h"
#include "ScalingOptions.h"

namespace Magpie {

class DeviceResources;

// NVIDIA VideoSuperRes modes 8-11 perform same-resolution denoising. The
// native backend uses D3D11/CUDA interop, so no frame is copied through CPU.
class RTXVideoDenoiser final : public NativeEffectBackend {
public:
	RTXVideoDenoiser();
	RTXVideoDenoiser(const RTXVideoDenoiser&) = delete;
	RTXVideoDenoiser& operator=(const RTXVideoDenoiser&) = delete;
	~RTXVideoDenoiser() override;

	bool Initialize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		uint32_t qualityLevel
	) noexcept;

	bool Resize(
		DeviceResources& deviceResources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	ScalingError InitializationError() const noexcept { return _initializationError; }

private:
	struct Impl;
	// 仅在 MP_ENABLE_RTX_VIDEO_DENOISE 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] uint32_t _qualityLevel = 8;
	ScalingError _initializationError = ScalingError::NoError;
};

}
