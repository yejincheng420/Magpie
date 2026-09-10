#include "pch.h"
#include "NgxRuntimeGuard.h"
#include "DLSSFrameGenerator.h"
#include "DeviceResources.h"
#include "FrameGuidanceD3D12Interop.h"
#include "Logger.h"
#include "NgxD3D12Core.h"
#include "Win32Helper.h"

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>

namespace Magpie {

// ---- 直接 snippet 模式（dlssg 代理场景） -------------------------------
// 现行 SDK（API 0x15）编译进 exe 的 app 侧核心无法初始化 dlssg 代理 Pinned 的
// 310.1 运行库：失败发生在核心与运行库的内部握手上（0xBAD0000B，早于任何
// NvAPI 调用与内核创建），应用层无法干预。310.1 模块自身导出完整的 snippet
// API（Init/CreateFeature/EvaluateFeature/ReleaseFeature/Shutdown1）——游戏
// 时代的 SDK/Streamline 正是直接调它们——因此绕过 app 侧核心，按原签名调用
// 模块导出。参数块是 vtable 对象（接口早于 0x13 且跨版本稳定），由我们核心
// 分配的块可直接交给 310.1 消费。无代理系统上同一流程会得到运行库的真实
// 拒绝（如架构不支持），干净失败。
using PFN_NgxInitD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
	unsigned long long, const wchar_t*, ID3D12Device*,
	NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_NgxCreateFeatureD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
	ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
	const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NgxEvaluateFeatureD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
	ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
	const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFN_NgxReleaseFeatureD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(
	NVSDK_NGX_Handle*);
using PFN_NgxShutdown1D3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

struct DLSSFrameGenerator::Impl {
	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	NgxD3D12Core* coreOwner = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedGenerated11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedGenerated12;
	winrt::com_ptr<ID3D12Resource> zeroMotion12;
	winrt::com_ptr<ID3D12Resource> zeroDepth12;
	std::array<winrt::com_ptr<ID3D12Resource>, 4> interpolationDisable12;
	std::array<winrt::com_ptr<ID3D12Resource>, 4> interpolationDisableReadback12;
	std::unique_ptr<FrameGuidanceD3D12Interop> guidanceInterop;
	winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap12;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	NVSDK_NGX_Handle* feature = nullptr;
	NVSDK_NGX_Parameter* parameters = nullptr;
	uint64_t fenceValue = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t multiplier = 2;
	uint32_t maxSupportedMultiplier = 2;
	uint32_t diagnosticRealFrames = 0;
	std::array<uint32_t, 4> diagnosticEvaluateSuccess{};
	std::array<uint32_t, 4> diagnosticEvaluateFailure{};
	std::array<uint32_t, 4> diagnosticInterpolationEnabled{};
	std::array<uint32_t, 4> diagnosticInterpolationDisabled{};
	std::array<uint32_t, 4> diagnosticInterpolationReadbackFailure{};
	uint32_t diagnosticGeneratedPublishSuccess = 0;
	uint32_t diagnosticGeneratedPublishFailure = 0;
	DLSSFrameGenerationSettings settings{};
	FrameGuidanceFrameId lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	uint8_t lastGuidanceBinding = UINT8_MAX;
	bool coreRegistered = false;
	bool resetHistory = true;

	// 直接 snippet 模式（dlssg 代理场景），见文件顶部说明。
	HMODULE directModule = nullptr;
	bool directInitialized = false;
	PFN_NgxInitD3D12 directInit = nullptr;
	PFN_NgxCreateFeatureD3D12 directCreateFeature = nullptr;
	PFN_NgxEvaluateFeatureD3D12 directEvaluateFeature = nullptr;
	PFN_NgxReleaseFeatureD3D12 directReleaseFeature = nullptr;
	PFN_NgxShutdown1D3D12 directShutdown1 = nullptr;
};

static bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

static NVSDK_NGX_Result ReleaseFeatureSafely(
	NVSDK_NGX_Handle* feature,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_D3D12_ReleaseFeature(feature);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

static NVSDK_NGX_Result GetParameterISafely(
	NVSDK_NGX_Parameter* parameters,
	const char* name,
	int* value,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_Parameter_GetI(parameters, name, value);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

static NVSDK_NGX_Result GetParameterUISafely(
	NVSDK_NGX_Parameter* parameters,
	const char* name,
	uint32_t* value,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NVSDK_NGX_Parameter_GetUI(parameters, name, value);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

static bool SetParameterUISafely(
	NVSDK_NGX_Parameter* parameters,
	const char* name,
	uint32_t value,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		NVSDK_NGX_Parameter_SetUI(parameters, name, value);
		return true;
	}, false, sehCode);
}

static bool SetParameterULLSafely(
	NVSDK_NGX_Parameter* parameters,
	const char* name,
	uint64_t value,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		NVSDK_NGX_Parameter_SetULL(parameters, name, value);
		return true;
	}, false, sehCode);
}

static NVSDK_NGX_Result CreateDlssgSafely(
	ID3D12GraphicsCommandList* commandList,
	NVSDK_NGX_Handle** feature,
	NVSDK_NGX_Parameter* parameters,
	NVSDK_NGX_DLSSG_Create_Params* createParams,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NGX_D3D12_CREATE_DLSSG(
			commandList, 1, 1, feature, parameters, createParams);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

static NVSDK_NGX_Result EvaluateDlssgSafely(
	ID3D12GraphicsCommandList* commandList,
	NVSDK_NGX_Handle* feature,
	NVSDK_NGX_Parameter* parameters,
	NVSDK_NGX_D3D12_DLSSG_Eval_Params* evalParams,
	NVSDK_NGX_DLSSG_Opt_Eval_Params* optionalParams,
	DWORD* sehCode
) noexcept {
	return NgxRuntimeGuard::Invoke([&]() {
		return NGX_D3D12_EVALUATE_DLSSG(
			commandList, feature, parameters, evalParams, optionalParams);
	}, NVSDK_NGX_Result_FAIL_PlatformError, sehCode);
}

// 在驱动仓库中查找 nvngx.dll，仅用作 310.1 Init 身份检查的地址提供者：该模
// 块的任何导出都不会被调用，因此驱动包版本新旧无关紧要。
static HMODULE FindDriverStoreNgxModule() noexcept {
	constexpr const wchar_t* repositoryRoot =
		L"C:\\Windows\\System32\\DriverStore\\FileRepository";
	WIN32_FIND_DATAW findData{};
	wil::unique_hfind find(FindFirstFileW(
		(std::wstring(repositoryRoot) + L"\\*").c_str(), &findData));
	if (!find) {
		return nullptr;
	}
	do {
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			wcscmp(findData.cFileName, L".") == 0 ||
			wcscmp(findData.cFileName, L"..") == 0) {
			continue;
		}
		const std::wstring candidate = std::wstring(repositoryRoot) +
			L"\\" + findData.cFileName + L"\\nvngx.dll";
		if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
			if (HMODULE module = LoadLibraryW(candidate.c_str())) {
				return module;
			}
		}
	} while (FindNextFileW(find.get(), &findData));
	return nullptr;
}

// ---- nvngx.dll 宿主跳板工厂（进程级缓存） -------------------------------
// 310.1 的多个导出（Init/CreateFeature/EvaluateFeature/ReleaseFeature/
// Shutdown1，反汇编确认）内置同款调用方身份检查：以入口返回地址定位调用方
// 模块并要求其路径包含 "nvngx.dll"——即只接受驱动核心 nvngx.dll 调用。在
// nvngx.dll 可执行节 VirtualSize 之后的零填充尾区（映射页内的死空间）写
// 15 字节跳板（mov rax,imm64; call rax; pop rcx; ret，保持 x64 栈对齐），
// 经跳板调用使返回地址落在 nvngx.dll 映像内。写入的是进程私有副本（写时
// 复制），不影响其他进程。同一进程内按目标缓存复用（重建缩放会话时不再
// 消耗新的洞）。仅后端线程调用，无需加锁。
struct NvngxCave {
	HMODULE host = nullptr;
	uint8_t* cursor = nullptr;
	uint8_t* end = nullptr;
	struct Entry { const void* target; void* cave; };
	Entry entries[16]{};
	int entryCount = 0;
};

static NvngxCave g_nvngxCave;

static bool FindCaveRegionInHost() noexcept {
	uint8_t* const base = reinterpret_cast<uint8_t*>(g_nvngxCave.host);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return false;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
		base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return false;
	}
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
			continue;
		}
		const size_t virtualSize = section->Misc.VirtualSize;
		const size_t alignedSize = (virtualSize + 0xFFF) & ~size_t(0xFFF);
		if (alignedSize <= virtualSize) {
			continue;
		}
		// VirtualSize 到节对齐边界之间必为零（映射页零初始化，无代码写入）。
		uint8_t* const tail = base + section->VirtualAddress + virtualSize;
		const size_t available = alignedSize - virtualSize;
		const size_t want = std::min<size_t>(available, 0x100);
		bool zeros = true;
		for (size_t b = 0; b < want; ++b) {
			if (tail[b]) {
				zeros = false;
				break;
			}
		}
		if (zeros) {
			g_nvngxCave.cursor = tail;
			g_nvngxCave.end = tail + available;
			return true;
		}
	}
	return false;
}

static void* GetOrCreateTrampoline(const void* target) noexcept {
	if (!g_nvngxCave.host) {
		HMODULE host = GetModuleHandleW(L"nvngx.dll");
		if (!host) {
			host = FindDriverStoreNgxModule();
		}
		if (!host) {
			return nullptr;
		}
		g_nvngxCave.host = host;
	}
	for (int i = 0; i < g_nvngxCave.entryCount; ++i) {
		if (g_nvngxCave.entries[i].target == target) {
			return g_nvngxCave.entries[i].cave;
		}
	}
	if (!g_nvngxCave.cursor && !FindCaveRegionInHost()) {
		return nullptr;
	}
	if (g_nvngxCave.cursor + 16 > g_nvngxCave.end) {
		return nullptr;
	}

	uint8_t* const cave = g_nvngxCave.cursor;
	DWORD oldProtect = 0;
	if (!VirtualProtect(cave, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		return nullptr;
	}
	// 15 字节跳板，保持 x64 栈对齐：入口 rsp≡8 (mod 16)，push 后 ≡0，
	// call 后被调方入口 ≡8（标准）。缺 push 的版本会让被调函数内部 SSE
	// 指令因栈失对齐崩溃（实测 0xC0000005）。
	uint8_t code[15];
	code[0] = 0x50;	// push rax（对齐 + 占位）
	code[1] = 0x48;
	code[2] = 0xB8;	// mov rax, imm64
	const uint64_t address = reinterpret_cast<uint64_t>(target);
	std::memcpy(code + 3, &address, 8);
	code[11] = 0xFF;
	code[12] = 0xD0;	// call rax（压入的返回地址=cave+13，落在模块内）
	code[13] = 0x59;	// pop rcx（平衡 push，不影响 eax）
	code[14] = 0xC3;	// ret
	std::memcpy(cave, code, sizeof(code));
	FlushInstructionCache(GetCurrentProcess(), cave, sizeof(code));
	VirtualProtect(cave, 16, oldProtect, &oldProtect);

	g_nvngxCave.cursor += 16;
	if (g_nvngxCave.entryCount <
		static_cast<int>(std::size(g_nvngxCave.entries))) {
		g_nvngxCave.entries[g_nvngxCave.entryCount++] = { target, cave };
	}
	return cave;
}

// NGX_D3D12_EVALUATE_DLSSG 宏的逐字复刻（参数写入部分），最终调用换成调用
// 方传入的模块导出，见 Draw。参数 Set* 经 vtable 作用于任意核心分配的块。
static void DirectSetDlssgEvalParams(
	NVSDK_NGX_Parameter* p,
	NVSDK_NGX_D3D12_DLSSG_Eval_Params* e,
	NVSDK_NGX_DLSSG_Opt_Eval_Params* o
) noexcept {
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, e->pBackbuffer);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_MVecs, e->pMVecs);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_Depth, e->pDepth);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, e->pHudless);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_UI, e->pUI);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_UIAlpha, e->pUIAlpha);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField, e->pBidirectionalDistortionField);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, e->pOutputInterpFrame);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_OutputReal, e->pOutputRealFrame);
	NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, e->pOutputDisableInterpolation);

	if (o) {
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, o->multiFrameCount);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, o->multiFrameIndex);

		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, o->cameraViewToClip);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, o->clipToCameraView);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, o->clipToLensClip);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, o->clipToPrevClip);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, o->prevClipToClip);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, o->jitterOffset[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, o->jitterOffset[1]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, o->mvecScale[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, o->mvecScale[1]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, o->cameraPinholeOffset[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, o->cameraPinholeOffset[1]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosX, o->cameraPos[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosY, o->cameraPos[1]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraPosZ, o->cameraPos[2]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpX, o->cameraUp[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpY, o->cameraUp[1]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraUpZ, o->cameraUp[2]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightX, o->cameraRight[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightY, o->cameraRight[1]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraRightZ, o->cameraRight[2]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdX, o->cameraFwd[0]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdY, o->cameraFwd[1]);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, o->cameraFwd[2]);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, o->cameraNear);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, o->cameraFar);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, o->cameraFOV);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, o->cameraAspectRatio);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, o->colorBuffersHDR);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, o->depthInverted);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, o->cameraMotionIncluded);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_Reset, o->reset);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, o->automodeOverrideReset);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, o->notRenderingGameFrames);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OrthoProjection, o->orthoProjection);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, o->motionVectorsInvalidValue);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MvecDilated, o->motionVectorsDilated);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, o->menuDetectionEnabled);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, o->mvecsSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY, o->mvecsSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, o->mvecsSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, o->mvecsSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, o->depthSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY, o->depthSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, o->depthSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, o->depthSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, o->hudLessSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY, o->hudLessSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, o->hudLessSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, o->hudLessSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UISubrectBaseX, o->uiSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UISubrectBaseY, o->uiSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UISubrectWidth, o->uiSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UISubrectHeight, o->uiSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseX, o->uiAlphaSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseY, o->uiAlphaSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectWidth, o->uiAlphaSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectHeight, o->uiAlphaSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectBaseX, o->bidirectionalDistFieldSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectBaseY, o->bidirectionalDistFieldSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectWidth, o->bidirectionalDistFieldSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionFieldSubrectHeight, o->bidirectionalDistFieldSubrectSize.Height);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_IsLowPrecision, o->bidirectionalDistFieldPrecisionInfo.IsLowPrecision);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_Bias, o->bidirectionalDistFieldPrecisionInfo.Bias);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField_LowPrecision_Scale, o->bidirectionalDistFieldPrecisionInfo.Scale);

		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation, o->minRelativeLinearDepthObjectSeparation);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseX, o->backbufferSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseY, o->backbufferSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, o->backbufferSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, o->backbufferSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, o->outputInterpSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY, o->outputInterpSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, o->outputInterpSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, o->outputInterpSubrectSize.Height);

		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseX, o->outputRealSubrectBase.X);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseY, o->outputRealSubrectBase.Y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectWidth, o->outputRealSubrectSize.Width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectHeight, o->outputRealSubrectSize.Height);
	}
}

static bool WaitForFence(DLSSFrameGenerator::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) {
		return true;
	}

	wil::unique_event_nothrow event;
	if (FAILED(event.create())) {
		return false;
	}
	if (FAILED(impl.fence12->SetEventOnCompletion(value, event.get()))) {
		return false;
	}
	return WaitForSingleObject(event.get(), 3000) == WAIT_OBJECT_0;
}

static bool WaitForQueue(DLSSFrameGenerator::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	if (FAILED(impl.queue12->Signal(impl.fence12.get(), value))) {
		return false;
	}
	return WaitForFence(impl, value);
}

DLSSFrameGenerator::Impl::~Impl() {
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}
	if (feature) {
		DWORD sehCode = 0;
		if (directInitialized && directReleaseFeature) {
			// 直接模式：feature 属于 310.1 模块，用它的导出释放。
			const NVSDK_NGX_Result result = NgxRuntimeGuard::Invoke(
				[&]() { return directReleaseFeature(feature); },
				NVSDK_NGX_Result_FAIL_PlatformError, &sehCode);
			if (sehCode) {
				Logger::Get().Warn(fmt::format(
					"DLSSFG direct ReleaseFeature raised SEH {:#x}", sehCode));
			} else if (!NGXSucceeded(result)) {
				Logger::Get().Warn(fmt::format(
					"DLSSFG direct ReleaseFeature failed ({:#x})",
					static_cast<uint32_t>(result)));
			}
		} else {
			const NVSDK_NGX_Result result = ReleaseFeatureSafely(feature, &sehCode);
			if (sehCode) {
				Logger::Get().Warn(fmt::format(
					"DLSSFG ReleaseFeature raised SEH {:#x}", sehCode));
			} else if (!NGXSucceeded(result)) {
				Logger::Get().Warn(fmt::format(
					"DLSSFG ReleaseFeature failed ({:#x})",
					static_cast<uint32_t>(result)));
			}
		}
		feature = nullptr;
	}
	if (parameters) {
		if (!coreOwner || !coreOwner->DestroyParameters(parameters, "DLSSFG")) {
			Logger::Get().Warn("DLSSFG shared Core parameter destruction failed");
		}
	}
	if (coreRegistered && coreOwner) {
		coreOwner->Release("DLSSFG");
	}
	if (directInitialized && directShutdown1 && device12) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result = NgxRuntimeGuard::Invoke(
			[&]() { return directShutdown1(device12.get()); },
			NVSDK_NGX_Result_FAIL_PlatformError, &sehCode);
		if (!NGXSucceeded(result) || sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSFG direct Shutdown1 failed ({:#x}, seh={:#x})",
				static_cast<uint32_t>(result), sehCode));
		}
	}
}

static bool CreateSharedTexture(
	DLSSFrameGenerator::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	bool allowUav,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		(allowUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		return false;
	}

	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSFG shared texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool CreateZeroTexture(
	DLSSFrameGenerator::Impl& impl,
	DXGI_FORMAT format,
	winrt::com_ptr<ID3D12Resource>& resource,
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
	D3D12_RESOURCE_BARRIER& barrier
) noexcept {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = impl.renderWidth;
	desc.Height = impl.renderHeight;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = impl.device12->CreateCommittedResource(
		&heap,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(resource.put())
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG virtual input texture failed", hr);
		return false;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl.device12->CreateUnorderedAccessView(resource.get(), nullptr, &uav, cpuHandle);
	static constexpr float ZERO[4]{};
	impl.commandList12->ClearUnorderedAccessViewFloat(
		gpuHandle, cpuHandle, resource.get(), ZERO, 0, nullptr);

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = {
		resource.get(),
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	return true;
}

static bool CreateInterpolationDisableResources(
	DLSSFrameGenerator::Impl& impl,
	uint32_t frameIndex
) noexcept {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = 4;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = impl.device12->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(impl.interpolationDisable12[frameIndex].put()));
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSFG interpolation-disable output failed", hr);
		return false;
	}

	heap.Type = D3D12_HEAP_TYPE_READBACK;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	hr = impl.device12->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(impl.interpolationDisableReadback12[frameIndex].put()));
	if (FAILED(hr)) {
		Logger::Get().ComError(
			"Create DLSSFG interpolation-disable readback failed", hr);
		return false;
	}
	return true;
}

static std::optional<bool> ReadInterpolationDisabled(
	DLSSFrameGenerator::Impl& impl,
	uint32_t frameIndex
) noexcept {
	D3D12_RANGE readRange{ 0, 1 };
	void* mapped = nullptr;
	const HRESULT hr = impl.interpolationDisableReadback12[frameIndex]->Map(
		0, &readRange, &mapped);
	if (FAILED(hr) || !mapped) {
		++impl.diagnosticInterpolationReadbackFailure[frameIndex];
		return std::nullopt;
	}
	const bool disabled = *static_cast<const uint8_t*>(mapped) != 0;
	D3D12_RANGE writtenRange{};
	impl.interpolationDisableReadback12[frameIndex]->Unmap(0, &writtenRange);
	if (disabled) {
		++impl.diagnosticInterpolationDisabled[frameIndex];
	} else {
		++impl.diagnosticInterpolationEnabled[frameIndex];
	}
	return disabled;
}

static void SetIdentity(float matrix[4][4]) noexcept {
	for (uint32_t row = 0; row < 4; ++row) {
		for (uint32_t column = 0; column < 4; ++column) {
			matrix[row][column] = row == column ? 1.0f : 0.0f;
		}
	}
}

DLSSFrameGenerator::DLSSFrameGenerator() = default;
DLSSFrameGenerator::~DLSSFrameGenerator() = default;

bool DLSSFrameGenerator::Initialize(
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	FrameGuidanceExtent guidanceExtent,
	const DLSSFrameGenerationSettings& settings
) noexcept {
	_requestedSettings = settings;
	_requestedSettings.multiplier = std::clamp(settings.multiplier, 2u, 4u);
	_impl.reset();
	auto impl = std::make_unique<Impl>();
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();
	impl->coreOwner = &ngxCore;
	impl->settings = _requestedSettings;

	D3D11_TEXTURE2D_DESC inputDesc{};
	input->GetDesc(&inputDesc);
	impl->width = inputDesc.Width;
	impl->height = inputDesc.Height;
	const bool guidanceRequested = impl->settings.motionVectorQuality !=
		NvidiaOpticalFlowQuality::None;
	const bool compatibleGuidanceExtent = guidanceRequested &&
		guidanceExtent.IsValid() &&
		guidanceExtent.width <= impl->width &&
		guidanceExtent.height <= impl->height;
	impl->renderWidth = compatibleGuidanceExtent ?
		guidanceExtent.width : impl->width;
	impl->renderHeight = compatibleGuidanceExtent ?
		guidanceExtent.height : impl->height;
	if (guidanceRequested && !compatibleGuidanceExtent) {
		Logger::Get().Warn(fmt::format(
			"DLSS FG guidance extent {}x{} is incompatible with backbuffer {}x{}; "
			"using Zero guidance at backbuffer size",
			guidanceExtent.width, guidanceExtent.height,
			impl->width, impl->height));
	}

	if (!ngxCore.Acquire(resources, "DLSSFG")) {
		return false;
	}
	impl->coreRegistered = true;
	impl->device12.copy_from(ngxCore.Device());
	HRESULT hr = S_OK;

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocator12.get(), nullptr,
			IID_PPV_ARGS(impl->commandList12.put()));
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSFG D3D12 command objects failed", hr);
		return false;
	}

	if (!CreateSharedTexture(*impl, inputDesc, false,
		impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, inputDesc, true,
			impl->sharedGenerated11, impl->sharedGenerated12)) {
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = impl->device12->CreateDescriptorHeap(
		&heapDesc, IID_PPV_ARGS(impl->descriptorHeap12.put()));
	if (FAILED(hr)) {
		return false;
	}
	ID3D12DescriptorHeap* heaps[]{ impl->descriptorHeap12.get() };
	impl->commandList12->SetDescriptorHeaps(1, heaps);
	const UINT stride = impl->device12->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		impl->descriptorHeap12->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu =
		impl->descriptorHeap12->GetGPUDescriptorHandleForHeapStart();
	D3D12_RESOURCE_BARRIER auxBarriers[2]{};
	if (!CreateZeroTexture(*impl, DXGI_FORMAT_R16G16_FLOAT,
		impl->zeroMotion12, cpu, gpu, auxBarriers[0])) {
		return false;
	}
	cpu.ptr += stride;
	gpu.ptr += stride;
	if (!CreateZeroTexture(*impl, DXGI_FORMAT_R32_FLOAT,
		impl->zeroDepth12, cpu, gpu, auxBarriers[1])) {
		return false;
	}
	impl->commandList12->ResourceBarrier(2, auxBarriers);

	NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
	if (!ngxCore.GetCapabilityParameters(&impl->parameters, "DLSSFG")) {
		return false;
	}

	int available = 0;
	DWORD sehCode = 0;
	result = GetParameterISafely(
		impl->parameters, NVSDK_NGX_Parameter_FrameGeneration_Available,
		&available, &sehCode);
	if (!NGXSucceeded(result) || !available) {
		int initResult = 0;
		DWORD initSehCode = 0;
		GetParameterISafely(
			impl->parameters,
			NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,
			&initResult, &initSehCode);

		// capability 参数是驱动/核心侧的结论；dlssg 代理不伪造它，代理伪装的
		// 是特征 DLL 导出的 NVSDK_NGX_D3D12_GetFeatureRequirements（EAT 钩子，
		// 游戏侧 Streamline 走的就是这条查询路）。Magpie 静态库的同名实现不经
		// 过该导出（实测返回 OutOfDate 且钩子未触发），因此这里直接解析特征
		// DLL 的导出并调用。无论查询结果如何都不提前退出：CreateFeature 才是
		// 最终裁决，失败走既有错误分支并记录真实错误码；无代理系统的失败路
		// 径不变（错误来自 CreateFeature 而非 capability 门）。
		const std::filesystem::path applicationDirectory =
			Win32Helper::GetExePath().parent_path();
		const std::wstring featurePath = applicationDirectory.wstring();
		const wchar_t* featurePaths[]{ featurePath.c_str() };
		NVSDK_NGX_FeatureCommonInfo featureInfo{};
		featureInfo.PathListInfo.Path = featurePaths;
		featureInfo.PathListInfo.Length = 1;
		NVSDK_NGX_FeatureDiscoveryInfo discovery{};
		discovery.SDKVersion = NVSDK_NGX_Version_API;
		discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
		discovery.Identifier.IdentifierType =
			NVSDK_NGX_Application_Identifier_Type_Project_Id;
		discovery.Identifier.v.ProjectDesc.ProjectId =
			"7c134ab9-9677-4af5-a2b2-bca943350861";
		discovery.Identifier.v.ProjectDesc.EngineType =
			NVSDK_NGX_ENGINE_TYPE_CUSTOM;
		discovery.Identifier.v.ProjectDesc.EngineVersion =
			"Magpie-Experimental-0.5.7";
		discovery.ApplicationDataPath = featurePath.c_str();
		discovery.FeatureInfo = &featureInfo;

		bool requirementsSupported = false;
		// 模块解析（按优先级）：
		// 1) dlssg Native 0.2.3（dlssg_for_sm86-main）：代理 version.dll 自身
		//    导出完整 snippet API、无调用方身份检查（反汇编确认），直接调用。
		//    TryLoadDlssgProxy 已在进程启动时按完整路径加载它，这里取句柄即可。
		// 2) 旧式包（dlssg_sm75）：LoadLibraryW(nvngx_dlssg.dll) 被代理重定向
		//    到已挂钩的 310.1 运行库，导出有 nvngx.dll 身份检查，经跳板调用。
		// 3) 无代理：加载 EXE 目录的 SDK 运行库，得到真实结论（干净失败）。
		HMODULE dlssgModule = nullptr;
		bool nativeProxy = false;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(applicationDirectory / L"version.dll").c_str(), &dlssgModule);
		if (dlssgModule &&
			!GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_CreateFeature")) {
			// exe 目录的 version.dll 不是 Native 代理（其他来源的代理）
			dlssgModule = nullptr;
		} else if (dlssgModule) {
			nativeProxy = true;
		}
		if (!dlssgModule) {
			dlssgModule = LoadLibraryW(
				(applicationDirectory / L"nvngx_dlssg.dll").c_str());
		}
		if (dlssgModule) {
			using PFN_GetFeatureRequirements = NVSDK_NGX_Result(NVSDK_CONV*)(
				IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
				NVSDK_NGX_FeatureRequirement*);
			const auto getRequirements = reinterpret_cast<PFN_GetFeatureRequirements>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
			if (getRequirements) {
				NVSDK_NGX_FeatureRequirement requirement{};
				DWORD requirementSehCode = 0;
				const NVSDK_NGX_Result requirementResult = NgxRuntimeGuard::Invoke(
					[&]() {
						return getRequirements(
							resources.GetGraphicsAdapter(), &discovery, &requirement);
					}, NVSDK_NGX_Result_FAIL_PlatformError, &requirementSehCode);
				requirementsSupported = NGXSucceeded(requirementResult) &&
					!requirementSehCode &&
					requirement.FeatureSupported ==
						NVSDK_NGX_FeatureSupportResult_Supported;
				Logger::Get().Warn(fmt::format(
					"DLSS FG capability 门禁旁路：initResult={:#x}, "
					"requirements={:#x} (flags={:#x}, seh={:#x}), supported={}",
					(uint32_t)initResult, (uint32_t)requirementResult,
					(uint32_t)requirement.FeatureSupported, requirementSehCode,
					requirementsSupported));
			} else {
				Logger::Get().Warn(
					"DLSS FG capability 门禁旁路：特征 DLL 缺少 GetFeatureRequirements 导出");
			}

			// 直接 snippet 模式：绕过 app 侧核心，在 310.1 模块自身上完成
			// Init/Create/Evaluate（见文件顶部说明）。requirements 只是诊断，
			// 无论结果如何都尝试 Init——CreateFeature 才是裁决。
			// 注意：310.1 只导出 EvaluateFeature，没有 _C 变体（后续 SDK 才有）。
			// 五个导出（Init/Create/Evaluate/Release/Shutdown1）都内置同款
			// 调用方身份检查——以入口返回地址定位调用方模块并要求路径包含
			// "nvngx.dll"（只接受驱动核心；游戏链路调用点在 nvngx.dll 内）。
			// Init 签名为 (appId, path, device, version)，version 在第 4 参。
			// 统一经 nvngx.dll 代码洞跳板（GetOrCreateTrampoline，进程级缓存）
			// 包装后调用，返回地址即落在 nvngx.dll 映像内。
			const auto initExport = reinterpret_cast<PFN_NgxInitD3D12>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_Init"));
			const auto createExport = reinterpret_cast<PFN_NgxCreateFeatureD3D12>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_CreateFeature"));
			const auto evaluateExport = reinterpret_cast<PFN_NgxEvaluateFeatureD3D12>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_EvaluateFeature"));
			const auto releaseExport = reinterpret_cast<PFN_NgxReleaseFeatureD3D12>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_ReleaseFeature"));
			const auto shutdownExport = reinterpret_cast<PFN_NgxShutdown1D3D12>(
				GetProcAddress(dlssgModule, "NVSDK_NGX_D3D12_Shutdown1"));
			if (initExport && createExport && evaluateExport &&
				releaseExport && shutdownExport) {
				impl->directModule = dlssgModule;
				// Native 0.2.3 的导出无身份检查，直接用；旧式包的导出有
				// nvngx.dll 身份检查（返回地址定位调用方模块），经跳板包装。
				const auto wrapExport = [nativeProxy](auto exportFn) noexcept {
					using Fn = decltype(exportFn);
					if (nativeProxy || !exportFn) {
						return exportFn;
					}
					return reinterpret_cast<Fn>(
						GetOrCreateTrampoline(reinterpret_cast<const void*>(exportFn)));
				};
				impl->directInit = wrapExport(initExport);
				impl->directCreateFeature = wrapExport(createExport);
				impl->directEvaluateFeature = wrapExport(evaluateExport);
				impl->directReleaseFeature = wrapExport(releaseExport);
				impl->directShutdown1 = wrapExport(shutdownExport);
				if (impl->directInit && impl->directCreateFeature &&
					impl->directEvaluateFeature && impl->directReleaseFeature &&
					impl->directShutdown1) {
					DWORD directSehCode = 0;
					const NVSDK_NGX_Result directInitResult = NgxRuntimeGuard::Invoke(
						[&]() {
							return impl->directInit(
								0, featurePath.c_str(), impl->device12.get(),
								(NVSDK_NGX_Version)0x0000013, nullptr);
						}, NVSDK_NGX_Result_FAIL_PlatformError, &directSehCode);
					if (NGXSucceeded(directInitResult) && !directSehCode) {
						impl->directInitialized = true;
						Logger::Get().Info(fmt::format(
							"DLSS FG 直接 snippet 模式：模块 Init 成功"
							"（{}，跳板宿主 {}）",
							nativeProxy ? "dlssg Native" : "310.1 重定向",
							nativeProxy ? "无需" :
							StrHelper::UTF16ToUTF8([]() {
								wchar_t name[MAX_PATH]{};
								GetModuleFileNameW(g_nvngxCave.host, name, MAX_PATH);
								return std::wstring(name);
							}())));
						// 参数块换成自有 Allocate 块（capability 块属于 app
						// 侧核心的上下文；vtable ABI 跨版本稳定，310.1 可直
						// 接消费）。
						NVSDK_NGX_Parameter* directParameters = nullptr;
						if (ngxCore.AllocateParameters(&directParameters, "DLSSFG") &&
							directParameters) {
							ngxCore.DestroyParameters(impl->parameters, "DLSSFG");
							impl->parameters = directParameters;
							// 空白块没有 capability 值（MultiFrameCountMax 读
							// 取会失败，倍率被钳到 2x）。用 310.1 自己的
							// PopulateParameters_Impl 填充——app 侧核心的
							// GetCapabilityParameters 内部正是调它，dlssg 代理
							// 在这条路径上报 MultiFrameCountMax（ini 的
							// MaxGeneratedFrames）。该导出同样有身份检查，经
							// 跳板调用。
							using PFN_PopulateParameters =
								NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
							const auto populateExport =
								reinterpret_cast<PFN_PopulateParameters>(
									GetProcAddress(dlssgModule,
										"NVSDK_NGX_D3D12_PopulateParameters_Impl"));
							if (populateExport) {
								const auto populateFn =
									nativeProxy ? populateExport :
									reinterpret_cast<PFN_PopulateParameters>(
										GetOrCreateTrampoline(
											reinterpret_cast<const void*>(populateExport)));
								if (populateFn) {
									DWORD populateSehCode = 0;
									const NVSDK_NGX_Result populateResult =
										NgxRuntimeGuard::Invoke(
											[&]() { return populateFn(impl->parameters); },
											NVSDK_NGX_Result_FAIL_PlatformError,
											&populateSehCode);
									uint32_t reportedMax = 0;
									DWORD maxSehCode = 0;
									GetParameterUISafely(impl->parameters,
										NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
										&reportedMax, &maxSehCode);
									Logger::Get().Info(fmt::format(
										"DLSS FG 直接模式 PopulateParameters: "
										"result={:#x}, seh={:#x}, MultiFrameCountMax={}",
										(uint32_t)populateResult, populateSehCode,
										reportedMax));
								}
							}
						}
					} else {
						Logger::Get().Warn(fmt::format(
							"DLSS FG 直接 snippet 模式：模块 Init 失败 ({:#x}, seh={:#x})",
							(uint32_t)directInitResult, directSehCode));
					}
				} else {
					Logger::Get().Warn(
						"DLSS FG 直接 snippet 模式：无法构造 nvngx.dll 身份跳板，Init 将被拒绝");
				}
			} else {
				Logger::Get().Warn(
					"DLSS FG 直接 snippet 模式：特征 DLL 缺少 snippet 导出，回退静态核心路径");
			}
		} else {
			Logger::Get().Warn(fmt::format(
				"DLSS FG capability 门禁旁路：加载 nvngx_dlssg.dll 失败 ({:#x})",
				GetLastError()));
		}
		Logger::Get().Warn(fmt::format(
			"DLSS FG capability 参数报告不可用 (result={:#x}, seh={:#x})，"
			"继续尝试 CreateFeature（direct={})",
			(uint32_t)initResult, sehCode ? sehCode : initSehCode,
			impl->directInitialized ? "on" : "off"));
	}

	uint32_t maxGeneratedFrames = 1;
	sehCode = 0;
	if (!NGXSucceeded(GetParameterUISafely(
		impl->parameters,
		NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
		&maxGeneratedFrames,
		&sehCode))) {
		maxGeneratedFrames = 1;
	}
	maxGeneratedFrames = std::clamp(maxGeneratedFrames, 1u, 3u);
	impl->maxSupportedMultiplier = maxGeneratedFrames + 1;
	impl->multiplier = std::min(
		_requestedSettings.multiplier, impl->maxSupportedMultiplier);
	if (impl->multiplier != _requestedSettings.multiplier) {
		Logger::Get().Warn(fmt::format(
			"DLSSFG {}x requested, hardware supports up to {}x; using {}x",
			_requestedSettings.multiplier,
			maxGeneratedFrames + 1, impl->multiplier));
	}
	for (uint32_t frameIndex = 1;
		frameIndex < impl->multiplier; ++frameIndex) {
		if (!CreateInterpolationDisableResources(*impl, frameIndex)) {
			return false;
		}
	}

	const uint32_t neverProvidedFlags =
		NVSDK_NGX_DLSSG_ResourceFlags_HUDLess |
		NVSDK_NGX_DLSSG_ResourceFlags_UI |
		NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
		NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
		NVSDK_NGX_DLSSG_ResourceFlags_OutputReal;
	sehCode = 0;
	const bool resourceFlagsSet = SetParameterUISafely(impl->parameters,
		NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
		neverProvidedFlags, &sehCode);
	if (!resourceFlagsSet) {
		Logger::Get().Error(fmt::format(
			"Set DLSSFG resource flags raised SEH {:#x}", sehCode));
		return false;
	}

	NVSDK_NGX_DLSSG_Create_Params createParams{};
	createParams.Width = impl->width;
	createParams.Height = impl->height;
	createParams.NativeBackbufferFormat = inputDesc.Format;
	createParams.RenderWidth = impl->renderWidth;
	createParams.RenderHeight = impl->renderHeight;
	createParams.DynamicResolutionScaling = false;
	sehCode = 0;
	if (impl->directInitialized) {
		// NGX_D3D12_CREATE_DLSSG 宏的逐字复刻，最终 CreateFeature 换成 310.1
		// 模块自己的导出。
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_Parameter_Width, createParams.Width);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_Parameter_Height, createParams.Height);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, createParams.NativeBackbufferFormat);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_DLSSG_Parameter_InternalWidth, createParams.RenderWidth);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_DLSSG_Parameter_InternalHeight, createParams.RenderHeight);
		NVSDK_NGX_Parameter_SetUI(impl->parameters, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, createParams.DynamicResolutionScaling);
		result = NgxRuntimeGuard::Invoke([&]() {
			return impl->directCreateFeature(
				impl->commandList12.get(), NVSDK_NGX_Feature_FrameGeneration,
				impl->parameters, &impl->feature);
			}, NVSDK_NGX_Result_FAIL_PlatformError, &sehCode);
	} else {
		result = CreateDlssgSafely(
			impl->commandList12.get(), &impl->feature,
			impl->parameters, &createParams, &sehCode);
	}
	if (!NGXSucceeded(result) || !impl->feature) {
		Logger::Get().Error(fmt::format(
			"NGX_D3D12_CREATE_DLSSG failed ({:#x}, seh={:#x}, direct={})",
			(uint32_t)result, sehCode, impl->directInitialized ? "on" : "off"));
		return false;
	}

	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		return false;
	}
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr) || !WaitForQueue(*impl)) {
		return false;
	}
	impl->guidanceInterop = std::make_unique<FrameGuidanceD3D12Interop>();
	if (!impl->guidanceInterop->Initialize(
		impl->device12.get(), impl->fence12.get())) {
		return false;
	}

	Logger::Get().Info(fmt::format(
		"DLSS FG_Experimental initialized: backbuffer={}x{}, render={}x{}, "
		"multiplier={}x, requestedMotion={}, depth=zero-contract, "
		"motionContract=current-to-previous/source-pixels scale=1,1",
		impl->width, impl->height, impl->renderWidth, impl->renderHeight,
		impl->multiplier,
		static_cast<uint32_t>(impl->settings.motionVectorQuality)));
	_impl = std::move(impl);
	return true;
}

bool DLSSFrameGenerator::Resize(
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	FrameGuidanceExtent guidanceExtent
) noexcept {
	return Initialize(resources, ngxCore, input, guidanceExtent, _requestedSettings);
}

FrameGuidanceRequirements
DLSSFrameGenerator::GetFrameGuidanceRequirements() const noexcept {
	FrameGuidanceRequirements result{ .zero = true };
	result.Add(MotionVectorRequest::Nvidia(
		_requestedSettings.motionVectorQuality));
	return result;
}

uint32_t DLSSFrameGenerator::Multiplier() const noexcept {
	return _impl ? _impl->multiplier : _requestedSettings.multiplier;
}

uint32_t DLSSFrameGenerator::MaxSupportedMultiplier() const noexcept {
	return _impl ? _impl->maxSupportedMultiplier : 2;
}

bool DLSSFrameGenerator::Draw(
	ID3D11Texture2D* input,
	FrameGuidanceFrameId frameId,
	const FrameGuidanceView& guidance,
	const FrameGuidanceView& zeroGuidance,
	const PublishCallback& publishGeneratedFrame
) noexcept {
	if (!_impl || !_impl->feature || !_impl->parameters) {
		return false;
	}
	Impl& impl = *_impl;
	const FrameGuidanceExtent renderExtent{
		impl.renderWidth, impl.renderHeight
	};
	const FrameGuidanceView selected = SelectFrameGuidanceChannels(
		guidance, zeroGuidance, frameId, renderExtent,
		impl.settings.motionVectorQuality != NvidiaOpticalFlowQuality::None);
	bool sharedGuidanceBound = false;
	bool realMotion = false;
	if (impl.settings.motionVectorQuality != NvidiaOpticalFlowQuality::None &&
		selected.IsValidFor(frameId, renderExtent) &&
		impl.guidanceInterop->Update(selected, frameId, renderExtent) &&
		impl.guidanceInterop->WaitForProducer(impl.context11, selected)) {
		sharedGuidanceBound = true;
		realMotion = impl.settings.motionVectorQuality !=
			NvidiaOpticalFlowQuality::None &&
			!selected.motion.metadata.isZero;
	}

	const uint8_t guidanceBinding = uint8_t(realMotion) |
		(uint8_t(impl.settings.motionVectorQuality !=
			NvidiaOpticalFlowQuality::None) << 1) |
		(uint8_t(sharedGuidanceBound) << 2);
	const bool bindingChanged = impl.lastGuidanceBinding != UINT8_MAX &&
		impl.lastGuidanceBinding != guidanceBinding;
	if (impl.lastGuidanceBinding != guidanceBinding) {
		Logger::Get().Info(fmt::format(
			"DLSS FG guidance frameId={}: requested motion={}, "
			"produced motion={}, bound motion={} depth=zero, fallback={}",
			frameId, impl.settings.motionVectorQuality !=
				NvidiaOpticalFlowQuality::None,
			guidance.motion.metadata.valid && !guidance.motion.metadata.isZero,
			realMotion ? "real" : "zero",
			!sharedGuidanceBound ? "interop-or-extent-zero" :
			(impl.settings.motionVectorQuality != NvidiaOpticalFlowQuality::None && !realMotion ?
				"provider-zero" : "none")));
	}
	const bool guidanceReset = bindingChanged ||
		(sharedGuidanceBound && selected.requiresHistoryReset &&
			impl.lastGuidanceResetFrameId != frameId);

	impl.context11->CopyResource(impl.sharedInput11.get(), input);
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		return false;
	}
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) {
		return false;
	}

	const bool resetThisFrame = impl.resetHistory || guidanceReset;
	const uint32_t generatedFrameCount = resetThisFrame ? 1 : impl.multiplier - 1;
	DWORD frameIdSehCode = 0;
	if (!SetParameterULLSafely(
		impl.parameters, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
		frameId, &frameIdSehCode)) {
		Logger::Get().Error(fmt::format(
			"Set DLSSFG BackbufferFrameID raised SEH {:#x}", frameIdSehCode));
		return false;
	}
	// 批量提交：整组生成帧的 Evaluate 记录进单个命令表、一次 Execute、
	// 一次 CPU 等待。原逐帧「提交→CPU 等 GPU→读回→发布」在游戏共享 GPU 时，
	// 每次等待都排在游戏的渲染队列后面（实测 4x 后端周期 ~50ms、真实帧采样
	// 仅 ~20/s——游戏 70fps 被采样丢掉 2/3，插值跨 3.5 个游戏帧，鬼影的直
	// 接来源）。批量化后每组只跨进程排队一次；读回与发布在单次等待之后
	// 逐帧进行（发布环互斥握手仍逐槽）。
	hr = impl.allocator12->Reset();
	if (SUCCEEDED(hr)) {
		hr = impl.commandList12->Reset(impl.allocator12.get(), nullptr);
	}
	if (FAILED(hr)) {
		return false;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition = {
		impl.sharedInput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition = {
		impl.sharedGenerated12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	};
	impl.commandList12->ResourceBarrier(2, barriers);
	if (sharedGuidanceBound) {
		impl.guidanceInterop->Transition(
			impl.commandList12.get(), D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	for (uint32_t frameIndex = 1; frameIndex <= generatedFrameCount; ++frameIndex) {
		NVSDK_NGX_D3D12_DLSSG_Eval_Params evalParams{};
		evalParams.pBackbuffer = impl.sharedInput12.get();
		evalParams.pDepth = sharedGuidanceBound ?
			impl.guidanceInterop->Depth() : impl.zeroDepth12.get();
		evalParams.pMVecs = sharedGuidanceBound ?
			impl.guidanceInterop->Motion() : impl.zeroMotion12.get();
		evalParams.pOutputInterpFrame = impl.sharedGenerated12.get();
		evalParams.pOutputDisableInterpolation =
			impl.interpolationDisable12[frameIndex].get();

		NVSDK_NGX_DLSSG_Opt_Eval_Params optionalParams{};
		optionalParams.multiFrameCount = generatedFrameCount;
		optionalParams.multiFrameIndex = frameIndex;
		SetIdentity(optionalParams.cameraViewToClip);
		SetIdentity(optionalParams.clipToCameraView);
		SetIdentity(optionalParams.clipToLensClip);
		SetIdentity(optionalParams.clipToPrevClip);
		SetIdentity(optionalParams.prevClipToClip);
		// Frame Guidance motion is already current-to-previous in pixels at the
		// motion-vector resource resolution. DLSSG therefore requires unit scale.
		optionalParams.mvecScale[0] = 1.0f;
		optionalParams.mvecScale[1] = 1.0f;
		optionalParams.cameraUp[1] = 1.0f;
		optionalParams.cameraRight[0] = 1.0f;
		optionalParams.cameraFwd[2] = 1.0f;
		optionalParams.cameraNear = 0.1f;
		optionalParams.cameraFar = 1000.0f;
		optionalParams.cameraFOV = 1.04719755f;
		optionalParams.cameraAspectRatio =
			float(impl.renderWidth) / float(impl.renderHeight);
		optionalParams.depthInverted = false;
		optionalParams.cameraMotionIncluded = realMotion;
		optionalParams.reset = resetThisFrame;
		optionalParams.motionVectorsInvalidValue = 0.0f;
		optionalParams.motionVectorsDilated = realMotion;
		optionalParams.menuDetectionEnabled = false;
		const FrameGuidanceRegion guidanceRegion = sharedGuidanceBound ?
			selected.motion.metadata.validRegion :
			FrameGuidanceRegion::Full(renderExtent);
		optionalParams.mvecsSubrectBase = {
			guidanceRegion.x, guidanceRegion.y
		};
		optionalParams.mvecsSubrectSize = {
			guidanceRegion.width, guidanceRegion.height
		};
		const FrameGuidanceRegion depthRegion = sharedGuidanceBound ?
			selected.depth.metadata.validRegion :
			FrameGuidanceRegion::Full(renderExtent);
		optionalParams.depthSubrectBase = {
			depthRegion.x, depthRegion.y
		};
		optionalParams.depthSubrectSize = {
			depthRegion.width, depthRegion.height
		};
		optionalParams.backbufferSubrectSize = {
			impl.width, impl.height
		};
		optionalParams.outputInterpSubrectSize = {
			impl.width, impl.height
		};

		DWORD sehCode = 0;
		NVSDK_NGX_Result result;
		if (impl.directInitialized) {
			DirectSetDlssgEvalParams(
				impl.parameters, &evalParams, &optionalParams);
			result = NgxRuntimeGuard::Invoke([&]() {
				return impl.directEvaluateFeature(
					impl.commandList12.get(), impl.feature, impl.parameters,
					nullptr);
				}, NVSDK_NGX_Result_FAIL_PlatformError, &sehCode);
		} else {
			result = EvaluateDlssgSafely(
				impl.commandList12.get(), impl.feature, impl.parameters,
				&evalParams, &optionalParams, &sehCode);
		}
		if (!NGXSucceeded(result)) {
			++impl.diagnosticEvaluateFailure[frameIndex];
			impl.commandList12->Close();
			Logger::Get().Error(fmt::format(
				"NGX_D3D12_EVALUATE_DLSSG failed ({:#x}, seh={:#x}, index={}/{}, direct={})",
				(uint32_t)result, sehCode, frameIndex, generatedFrameCount,
				impl.directInitialized ? "on" : "off"));
			return false;
		}
		++impl.diagnosticEvaluateSuccess[frameIndex];
		{
			D3D12_RESOURCE_BARRIER diagnosticBarrier{};
			diagnosticBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			diagnosticBarrier.Transition = {
				impl.interpolationDisable12[frameIndex].get(),
				D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_SOURCE
			};
			impl.commandList12->ResourceBarrier(1, &diagnosticBarrier);
			impl.commandList12->CopyBufferRegion(
				impl.interpolationDisableReadback12[frameIndex].get(), 0,
				impl.interpolationDisable12[frameIndex].get(), 0, 4);
			std::swap(
				diagnosticBarrier.Transition.StateBefore,
				diagnosticBarrier.Transition.StateAfter);
			impl.commandList12->ResourceBarrier(1, &diagnosticBarrier);
		}
	}

	if (sharedGuidanceBound) {
		impl.guidanceInterop->Transition(
			impl.commandList12.get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COMMON);
	}
	for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}
	impl.commandList12->ResourceBarrier(2, barriers);
	hr = impl.commandList12->Close();
	if (FAILED(hr)) {
		return false;
	}
	ID3D12CommandList* lists[]{ impl.commandList12.get() };
	impl.queue12->ExecuteCommandLists(1, lists);
	const uint64_t outputReady = ++impl.fenceValue;
	hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	if (SUCCEEDED(hr) && sharedGuidanceBound) {
		impl.guidanceInterop->MarkSubmitted(outputReady);
	}
	if (SUCCEEDED(hr)) {
		hr = impl.context11->Wait(impl.fence11.get(), outputReady);
	}
	if (FAILED(hr)) {
		return false;
	}
	// The flag is part of the SDK output, not optional telemetry. Also wait
	// on reset/disabled frames before reusing the allocator and output buffer.
	if (!WaitForFence(impl, outputReady)) return false;
	for (uint32_t frameIndex = 1; frameIndex <= generatedFrameCount; ++frameIndex) {
		const auto disabled = ReadInterpolationDisabled(impl, frameIndex);
		if (!disabled) return false;
		if (!resetThisFrame && !*disabled) {
			if (!publishGeneratedFrame(impl.sharedGenerated11.get())) {
				++impl.diagnosticGeneratedPublishFailure;
				return false;
			}
			++impl.diagnosticGeneratedPublishSuccess;
		}
	}

	impl.resetHistory = false;
	impl.lastGuidanceBinding = guidanceBinding;
	if (guidanceReset) impl.lastGuidanceResetFrameId = frameId;
	if (++impl.diagnosticRealFrames >= 120) {
		Logger::Get().Info(fmt::format(
			"DLSSFG 120-real-frame diagnostics: multiplier={}x "
			"evaluate[index1={}/{} index2={}/{} index3={}/{}] "
			"generatedPublish={}/{} "
			"interpolation[index1={}/{}/{} index2={}/{}/{} index3={}/{}/{}]",
			impl.multiplier,
			impl.diagnosticEvaluateSuccess[1],
			impl.diagnosticEvaluateFailure[1],
			impl.diagnosticEvaluateSuccess[2],
			impl.diagnosticEvaluateFailure[2],
			impl.diagnosticEvaluateSuccess[3],
			impl.diagnosticEvaluateFailure[3],
			impl.diagnosticGeneratedPublishSuccess,
			impl.diagnosticGeneratedPublishFailure,
			impl.diagnosticInterpolationEnabled[1],
			impl.diagnosticInterpolationDisabled[1],
			impl.diagnosticInterpolationReadbackFailure[1],
			impl.diagnosticInterpolationEnabled[2],
			impl.diagnosticInterpolationDisabled[2],
			impl.diagnosticInterpolationReadbackFailure[2],
			impl.diagnosticInterpolationEnabled[3],
			impl.diagnosticInterpolationDisabled[3],
			impl.diagnosticInterpolationReadbackFailure[3]));
		impl.diagnosticRealFrames = 0;
		impl.diagnosticEvaluateSuccess.fill(0);
		impl.diagnosticEvaluateFailure.fill(0);
		impl.diagnosticInterpolationEnabled.fill(0);
		impl.diagnosticInterpolationDisabled.fill(0);
		impl.diagnosticInterpolationReadbackFailure.fill(0);
		impl.diagnosticGeneratedPublishSuccess = 0;
		impl.diagnosticGeneratedPublishFailure = 0;
	}
	return true;
}

void DLSSFrameGenerator::RequestHistoryReset() noexcept {
	if (_impl) {
		_impl->resetHistory = true;
	}
}

bool DLSSFrameGenerator::Drain() noexcept {
	return !_impl || !_impl->queue12 || !_impl->fence12 || WaitForQueue(*_impl);
}

}

#else

namespace Magpie {

struct DLSSFrameGenerator::Impl {};
DLSSFrameGenerator::DLSSFrameGenerator() = default;
DLSSFrameGenerator::~DLSSFrameGenerator() = default;
bool DLSSFrameGenerator::Initialize(
	DeviceResources&, NgxD3D12Core&, ID3D11Texture2D*, FrameGuidanceExtent,
	const DLSSFrameGenerationSettings&) noexcept {
	Logger::Get().Error("DLSS Frame Generation is disabled at build time");
	return false;
}
bool DLSSFrameGenerator::Resize(
	DeviceResources&, NgxD3D12Core&, ID3D11Texture2D*,
	FrameGuidanceExtent) noexcept {
	return false;
}
bool DLSSFrameGenerator::Draw(
	ID3D11Texture2D*, FrameGuidanceFrameId,
	const FrameGuidanceView&, const FrameGuidanceView&,
	const PublishCallback&) noexcept {
	return false;
}
void DLSSFrameGenerator::RequestHistoryReset() noexcept {}
bool DLSSFrameGenerator::Drain() noexcept { return true; }
FrameGuidanceRequirements
DLSSFrameGenerator::GetFrameGuidanceRequirements() const noexcept { return {}; }
uint32_t DLSSFrameGenerator::Multiplier() const noexcept {
	return _requestedSettings.multiplier;
}
uint32_t DLSSFrameGenerator::MaxSupportedMultiplier() const noexcept { return 2; }

}

#endif
