#pragma once
#include "NativeEffectBackend.h"
#include "GroupBEffectProtocol.h"

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
	float residualHueProtection = 0;
	float residualDarkProtection = 0;
	float residualHighlightProtection = 0;
	float residualLocalCompression = 0;
	float residualLowFrequencyGain = 1;
	float residualDetailGain = 1;
	float residualChromaTemporalStrength = 0;
	int residualDebugView = 0;
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float skinStructureStrength = 0.0f;
	bool useAutoMask = false;
	bool uiCorrection = false;
	// 残差转移：奇数帧跳过 NGX，把偶数帧的（运动补偿后的）修正贴到奇数帧的
	// 新捕获上。奇数帧是真实的新画面（可与帧生成叠加），NGX 开销减半。
	// 残差与噪点独立随机，转移后奇帧噪点幅度约 √2 倍（比无 DLSSNR 干净一半）。
	// residualTransferMode: 0 = Copy(不挪) 1 = Global MV(全帧单向量挪)
	//   2 = Reproject(逐像素 MV 反向重投影 + Catmull-Rom + 2x2 footprint clamp)
	// residualTransferDomain: 0 = Residual(P0-1 残差域，转移 evenDenoised−
	//   evenInput 的有符号残差场，底图恒为当帧原图) 1 = Legacy Denoised
	//   (旧「整张降噪图重放」实现，仅作 A/B 对照；残影 ∝ 帧间整幅变化)。
	bool enableFrameReuse = false;
	uint32_t residualTransferMode = 0;
	uint32_t residualTransferDomain = 0;
	// P3 Fill：模式 2 下，被信任判据拒掉的像素从同表面邻居借已信任的残差补上
	// （0 = 关闭，画面与 P2 逐位一致；1 = 两个可信邻居就补满）。
	// 默认 0：无 depth 时「同表面」只能用色彩近似，软阴影/渐变上会把邻区的色调
	// 搬进来（阴影移位、发白）——实测 fill>0 反而更差，故默认关闭、保留旋钮。
	float residualFillStrength = 0.0f;
	// P4 保护阀（各自独立，0 = 关闭该项）。
	// 渲染帧率低于此值时暂停复用：帧间位移随帧率下降变大，挪过去的残差偏差随之
	// 变大（拖尾）。0 = 不设最低帧率。
	float residualMinFps = 0.0f;
	// 「本帧没有 detail 可搬」的像素占比（百分数）超过此值时暂停复用：那是屏外
	// 新涌入与被遮挡后露出的内容，只能拿当帧旧残差，与旁边的完整帧不一致（半帧率
	// 边缘闪烁）。0 = 从不暂停，且不做 GPU 回读。
	float residualMaxDropped = 0.0f;
	// 上游 0.6.7 统一的光流请求（取代旧 motionVectorQuality NVIDIA 单选）：
	// 支持 AMD OF / NVIDIA OF 双通道，含旧键迁移（useMotionVectors/motionVectorQuality）。
	MotionVectorRequest motionRequest{};
	// Experimental FP16 path. SDR RGBA8 remains the default.
	DlssnrExperimentProtocol experimentalHdr{};
};

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option, bool hdrEnabled = false) noexcept;

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame. Motion uses shared optical flow when selected;
// depth and unavailable motion use explicit zero guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	bool Drain() noexcept override;
	int32_t LastDrawReuseParity() const noexcept override;
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
	// One scheduling/resource owner, with independent features and cached outputs.
	bool InitializeChain(DeviceResources& resources, NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input, ID3D11Texture2D* output,
		std::span<const DLSSNRSettings> passes) noexcept;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;
	// A failed evaluation may retain legacy pass-through behavior for one pass.
	// Serial chains must detect it and reject the entire frame.
	bool IsHealthy() const noexcept;

private:
	// 仅在 MP_ENABLE_DLSSNR 构建中使用；无 SDK 的 CI 构建里 ClangCL -Werror 会报未使用
	[[maybe_unused]] std::unique_ptr<Impl> _impl;
	[[maybe_unused]] DLSSNRSettings _settings;
	[[maybe_unused]] std::vector<DLSSNRSettings> _passSettings;
	[[maybe_unused]] NgxD3D12Core* _ngxCore = nullptr;
};

}
