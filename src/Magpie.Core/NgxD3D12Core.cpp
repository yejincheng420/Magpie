#include "pch.h"
#include "NgxRuntimeGuard.h"
#include "NgxD3D12Core.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"

#if defined(MP_ENABLE_DLSSNR) || defined(MP_ENABLE_DLSS_FRAME_GENERATION)
#include <nvsdk_ngx.h>
#endif

namespace Magpie {

#if defined(MP_ENABLE_DLSSNR) || defined(MP_ENABLE_DLSS_FRAME_GENERATION)

namespace {

// dlssg 代理包（dlssg_sm75 / dlssg_for_sm86）附带的 310.1 运行库只接受它同
// 时代（NGX API 0x13，Streamline 同款）的核心声明：0x15 核心下该运行库在
// 自身 init 阶段即返回 UnableToInitializeFeature（backend kernel_create 从
// 未发生、无任何 NvAPI 调用——版本门禁在两者之前）。较新的运行库（DLSS-SR
// 310.7、DLSSNR 310.8）对旧声明向后兼容（NGX OTA 的正常方向），因此链上
// 有 DLSSFG 时对外声明 0x13，其余会话保持 SDK 版本不变，行为零变化。
constexpr NVSDK_NGX_Version NGX_VERSION_DLSSG_PROXY =
	(NVSDK_NGX_Version)0x0000013;

bool EffectChainContainsDLSSFG() noexcept {
	if (!ScalingWindow::Get()) {
		return false;
	}
	for (const EffectOption& effect : ScalingWindow::Get().Options().effects) {
		if (ClassifyFrameGenerationEffect(effect.name) ==
			FrameGenerationEffectKind::DLSS) {
			return true;
		}
	}
	return false;
}

void NVSDK_CONV NgxLogCallback(
	const char* message,
	NVSDK_NGX_Logging_Level loggingLevel,
	NVSDK_NGX_Feature sourceComponent
) noexcept {
	if (!message || !*message) {
		return;
	}
	// 可从任意线程进入；spdlog 的每次调用自持锁，线程安全。
	const std::string_view text(message);
	switch (loggingLevel) {
	case NVSDK_NGX_LOGGING_LEVEL_OFF:
		break;
	case NVSDK_NGX_LOGGING_LEVEL_ON:
		Logger::Get().Info(fmt::format("NGX(feature={}) {}",
			(uint32_t)sourceComponent, text));
		break;
	default:
		Logger::Get().Info(fmt::format("NGX(feature={}) [verbose] {}",
			(uint32_t)sourceComponent, text));
		break;
	}
}

NVSDK_NGX_Result InitCoreSafely(
	const wchar_t* applicationDirectory,
	ID3D12Device* device,
	const NVSDK_NGX_FeatureCommonInfo* featureInfo,
	NVSDK_NGX_Version sdkVersion,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_Init_with_ProjectID(
			"7c134ab9-9677-4af5-a2b2-bca943350861",
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			"Magpie-Experimental-0.5.7",
			applicationDirectory,
			device,
			featureInfo,
			sdkVersion);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result AllocateParametersSafely(
	NVSDK_NGX_Parameter** parameters,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_AllocateParameters(parameters);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result GetCapabilityParametersSafely(
	NVSDK_NGX_Parameter** parameters,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_GetCapabilityParameters(parameters);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result DestroyParametersSafely(
	NVSDK_NGX_Parameter* parameters,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_DestroyParameters(parameters);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

NVSDK_NGX_Result ShutdownCoreSafely(
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_Shutdown1(device);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

bool LogNgxResult(
	std::string_view operation,
	std::string_view consumer,
	NVSDK_NGX_Result result,
	DWORD sehCode
) noexcept {
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"NGX D3D12 Core {} for {} raised SEH {:#x} at {:#x}, thread={}; "
			"NGX disabled until Magpie is restarted",
			operation, consumer, sehCode, NgxRuntimeGuard::FaultAddress(),
			NgxRuntimeGuard::FaultThread()));
		return false;
	}
	if (!NVSDK_NGX_SUCCEED(result)) {
		Logger::Get().Error(fmt::format(
			"NGX D3D12 Core {} for {} failed ({:#x})",
			operation, consumer, static_cast<uint32_t>(result)));
		return false;
	}
	return true;
}

}

NgxD3D12Core::~NgxD3D12Core() {
	_Shutdown();
}

bool NgxD3D12Core::Acquire(
	DeviceResources& resources,
	std::string_view consumer
) noexcept {
	if (NgxRuntimeGuard::IsFaulted()) {
		Logger::Get().Error("NGX initialization blocked after an earlier fault; restart Magpie");
		return false;
	}
	if (!_device) {
		const HRESULT hr = D3D12CreateDevice(
			resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(_device.put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Create shared NGX D3D12 device failed", hr);
			return false;
		}
	}
	if (!_initialized) {
		const std::filesystem::path applicationDirectory =
			Win32Helper::GetExePath().parent_path();
		const std::wstring featurePath = applicationDirectory.wstring();
		const wchar_t* featurePaths[]{ featurePath.c_str() };
		NVSDK_NGX_FeatureCommonInfo featureInfo{};
		featureInfo.PathListInfo.Path = featurePaths;
		featureInfo.PathListInfo.Length = 1;
		featureInfo.LoggingInfo.LoggingCallback = &NgxLogCallback;
		featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
		const NVSDK_NGX_Version sdkVersion = EffectChainContainsDLSSFG() ?
			NGX_VERSION_DLSSG_PROXY : NVSDK_NGX_Version_API;
		if (sdkVersion != NVSDK_NGX_Version_API) {
			Logger::Get().Info(fmt::format(
				"NGX core 将以兼容版本 {:#x} 初始化（链上含 DLSSFG，"
				"适配 dlssg 代理的 310.1 运行库；SDK 版本为 {:#x}）",
				(uint32_t)sdkVersion, (uint32_t)NVSDK_NGX_Version_API));
		}
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result = InitCoreSafely(
			applicationDirectory.c_str(), _device.get(), &featureInfo,
			sdkVersion, &sehCode);
		if (!LogNgxResult("Init", consumer, result, sehCode)) return false;
		_initialized = true;
		Logger::Get().Info("NGX D3D12 Core initialized once for Renderer session");
	}
	++_activeConsumers;
	Logger::Get().Info(fmt::format(
		"NGX D3D12 Core consumer acquired: name={} active={}",
		consumer, _activeConsumers));
	return true;
}

void NgxD3D12Core::Release(std::string_view consumer) noexcept {
	if (!_activeConsumers) {
		Logger::Get().Warn(fmt::format(
			"NGX D3D12 Core consumer release underflow: name={}", consumer));
		return;
	}
	--_activeConsumers;
	Logger::Get().Info(fmt::format(
		"NGX D3D12 Core consumer released: name={} active={}",
		consumer, _activeConsumers));
}

bool NgxD3D12Core::AllocateParameters(
	NVSDK_NGX_Parameter** parameters,
	std::string_view consumer
) noexcept {
	if (!_initialized || !parameters) return false;
	DWORD sehCode = 0;
	const NVSDK_NGX_Result result =
		AllocateParametersSafely(parameters, &sehCode);
	if (!LogNgxResult("AllocateParameters", consumer, result, sehCode) ||
		!*parameters) return false;
	++_activeParameterBlocks;
	return true;
}

bool NgxD3D12Core::GetCapabilityParameters(
	NVSDK_NGX_Parameter** parameters,
	std::string_view consumer
) noexcept {
	if (!_initialized || !parameters) return false;
	DWORD sehCode = 0;
	const NVSDK_NGX_Result result =
		GetCapabilityParametersSafely(parameters, &sehCode);
	if (!LogNgxResult("GetCapabilityParameters", consumer, result, sehCode) ||
		!*parameters) return false;
	++_activeParameterBlocks;
	return true;
}

bool NgxD3D12Core::DestroyParameters(
	NVSDK_NGX_Parameter* parameters,
	std::string_view consumer
) noexcept {
	if (!parameters) return true;
	DWORD sehCode = 0;
	const NVSDK_NGX_Result result =
		DestroyParametersSafely(parameters, &sehCode);
	const bool succeeded =
		LogNgxResult("DestroyParameters", consumer, result, sehCode);
	if (succeeded && _activeParameterBlocks) --_activeParameterBlocks;
	return succeeded;
}

void NgxD3D12Core::_Shutdown() noexcept {
	if (NgxRuntimeGuard::IsFaulted()) {
		// Keep the device alive for SDK state whose teardown was interrupted.
		(void)_device.detach();
		_initialized = false;
		return;
	}
	if (!_initialized) return;
	if (_activeConsumers || _activeParameterBlocks) {
		Logger::Get().Warn(fmt::format(
			"NGX D3D12 Core final shutdown with live state: consumers={} parameters={}",
			_activeConsumers, _activeParameterBlocks));
	}
	DWORD sehCode = 0;
	const NVSDK_NGX_Result result = ShutdownCoreSafely(_device.get(), &sehCode);
	if (LogNgxResult("final Shutdown1", "Renderer", result, sehCode)) {
		Logger::Get().Info("NGX D3D12 Core final Shutdown1 completed");
	} else {
		NgxRuntimeGuard::MarkShutdownFailed();
		(void)_device.detach();
	}
	_initialized = false;
}

#else

NgxD3D12Core::~NgxD3D12Core() = default;
bool NgxD3D12Core::Acquire(DeviceResources&, std::string_view) noexcept { return false; }
void NgxD3D12Core::Release(std::string_view) noexcept {}
bool NgxD3D12Core::AllocateParameters(
	NVSDK_NGX_Parameter**, std::string_view) noexcept { return false; }
bool NgxD3D12Core::GetCapabilityParameters(
	NVSDK_NGX_Parameter**, std::string_view) noexcept { return false; }
bool NgxD3D12Core::DestroyParameters(
	NVSDK_NGX_Parameter*, std::string_view) noexcept { return false; }
void NgxD3D12Core::_Shutdown() noexcept {}

#endif

}
