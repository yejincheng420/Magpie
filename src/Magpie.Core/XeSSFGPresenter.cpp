#include "pch.h"
#include "FramePacingWait.h"
#include "FrameTrace.h"
#include "XeSSFGPresenter.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include <atomic>
#include <dcomp.h>

#ifdef MP_ENABLE_XESS_FRAME_GENERATION
#include <d3d12.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>

namespace Magpie {

static constexpr uint32_t BUFFER_COUNT = 3;
// XeSS-FG HDR terminal contract: HDR10/BT.2100 packed 10:10:10:2 UNORM.
// The proxy swap-chain, shared color surface, and back buffers all use this
// exact format so the SDK observes one consistent terminal resource format.
static constexpr DXGI_FORMAT HDR_COLOR_FORMAT = DXGI_FORMAT_R10G10B10A2_UNORM;
static constexpr DXGI_FORMAT LDR_COLOR_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
// DirectComposition virtual surfaces do not accept the XeSS terminal's
// packed R10 format. Keep the independent UI surface in FP16/scRGB; the
// XeSS proxy swap chain and terminal color resources remain HDR10 R10.
static constexpr DXGI_FORMAT OVERLAY_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

static DXGI_FORMAT ColorFormat(bool hdr) noexcept {
	return hdr ? HDR_COLOR_FORMAT : LDR_COLOR_FORMAT;
}
static DXGI_FORMAT OverlayFormat(bool hdr) noexcept {
	return hdr ? OVERLAY_FORMAT : LDR_COLOR_FORMAT;
}

static bool XeFGSucceeded(xefg_swapchain_result_t result) noexcept {
	return result >= XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

static bool XeLLSucceeded(xell_result_t result) noexcept {
	return result == XELL_RESULT_SUCCESS;
}

static void LogXeFGResult(std::string_view operation, xefg_swapchain_result_t result) noexcept {
	if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
		return;
	}
	const std::string message = fmt::format(
		"XeSSFG {} ({})", operation, static_cast<int32_t>(result));
	if (XeFGSucceeded(result)) {
		Logger::Get().Warn(message);
	} else {
		Logger::Get().Error(message);
	}
}

static void XeFGLogCallback(
	const char* message,
	xefg_swapchain_logging_level_t level,
	void*
) {
	if (!message) {
		return;
	}
	switch (level) {
	case XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR:
		Logger::Get().Error(fmt::format("XeSSFG SDK: {}", message));
		break;
	case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING:
		Logger::Get().Warn(fmt::format("XeSSFG SDK: {}", message));
		break;
	default:
		break;
	}
}

// ---- XeSS-FG MFG 解锁（非 Intel 设备 3x/4x） ---------------------------
// Intel libxess_fg.dll 1.3.1.78 在非 Intel 设备上把插帧上限钳到 1（设备
// 上报 maxSupportedInterpolations=1）。社区 OptiScaler（XeFGUnlock）的解锁
// 方式是进程内 .text 字节补丁：两条条件跳转改无条件（能力谓词恒真）+
// 三条 imm32 重写（默认上限/每上下文钳制值/上报值替换为请求的插帧数）。
// 补丁表按该 DLL 的 TimeDateStamp+SizeOfImage 识别构建，逐条校验期望字
// 节后才写入（先全验后写、回读校验、幂等、失败回滚）。我们部署的 DLL 与
// OptiScaler 使用的完全同版（MD5 一致）；SDK 升级后身份校验失败只是 MFG
// 干净降级，x2 路径不受影响。
namespace {

struct XeSSMfgPatch {
	const char* name;
	uint32_t rva;
	uint32_t length;
	// 替换字节内 imm32 的偏移；补丁应用时把请求的插帧数写进这 4 字节。
	static constexpr uint32_t kNoOverlay = 0xFFFFFFFF;
	uint32_t imm32Offset;
	std::array<uint8_t, 10> expected{};
	std::array<uint8_t, 10> replacement{};
};

bool ApplyXeSSMfgUnlock(uint32_t interpolationCount) noexcept {
	static constexpr std::array<XeSSMfgPatch, 5> patches{ {
		{ "U1/frame-count-fallback", 0x20DA4F, 6, XeSSMfgPatch::kNoOverlay,
			{ 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 },
			{ 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 } },
		{ "U2/model-downgrade", 0x1A5DE4, 2, XeSSMfgPatch::kNoOverlay,
			{ 0x74, 0x09 }, { 0xEB, 0x06 } },
		{ "U3/default-ceiling", 0x1A517D, 5, 1,
			{ 0xBB, 0x03, 0x00, 0x00, 0x00 },
			{ 0xBB, 0x00, 0x00, 0x00, 0x00 } },
		{ "U4/override-clamp", 0x1A45C2, 10, 6,
			{ 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 },
			{ 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
		{ "U5/reported-maximum", 0x20973B, 5, 1,
			{ 0xB8, 0x01, 0x00, 0x00, 0x00 },
			{ 0xB8, 0x00, 0x00, 0x00, 0x00 } },
	} };

	const HMODULE module = GetModuleHandleW(L"libxess_fg.dll");
	if (!module) {
		Logger::Get().Warn("XeSS-FG MFG unlock: libxess_fg.dll is not loaded");
		return false;
	}
	const auto* base = reinterpret_cast<const uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	// 身份校验：只对已验证过补丁表的构建打补丁。
	if (nt->FileHeader.TimeDateStamp != 0x69cb0f4d ||
		nt->OptionalHeader.SizeOfImage != 0x15ed000) {
		Logger::Get().Warn(fmt::format(
			"XeSS-FG MFG unlock: unrecognised libxess_fg build {:#x}/{:#x}, skipping",
			nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage));
		return false;
	}
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	const IMAGE_SECTION_HEADER* text = nullptr;
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		if (memcmp(section->Name, ".text", 5) == 0) {
			text = section;
			break;
		}
	}
	if (!text) {
		Logger::Get().Warn("XeSS-FG MFG unlock: libxess_fg has no .text section");
		return false;
	}

	// 预生成最终替换字节（imm32 覆盖 = 请求的插帧数）。
	std::array<std::array<uint8_t, 10>, patches.size()> finalBytes{};
	bool allApplied = true;
	for (size_t i = 0; i < patches.size(); ++i) {
		const XeSSMfgPatch& patch = patches[i];
		if (patch.rva < text->VirtualAddress ||
			patch.rva + patch.length >
				text->VirtualAddress + text->Misc.VirtualSize) {
			Logger::Get().Warn(fmt::format(
				"XeSS-FG MFG unlock: {} at {:#x} falls outside .text, aborting",
				patch.name, patch.rva));
			return false;
		}
		finalBytes[i] = patch.replacement;
		if (patch.imm32Offset != XeSSMfgPatch::kNoOverlay) {
			std::memcpy(finalBytes[i].data() + patch.imm32Offset,
				&interpolationCount, 4);
		}
		const uint8_t* const target = base + patch.rva;
		if (memcmp(target, finalBytes[i].data(), patch.length) == 0) {
			continue;	// 已应用（重建缩放会话的二次 Initialize）——幂等
		}
		allApplied = false;
		if (memcmp(target, patch.expected.data(), patch.length) != 0) {
			Logger::Get().Warn(fmt::format(
				"XeSS-FG MFG unlock: {} at {:#x} has unexpected bytes, aborting",
				patch.name, patch.rva));
			return false;
		}
	}
	if (allApplied) {
		return true;
	}

	// 全部校验通过后统一写入；任何一条写失败即回滚已写的条目。
	size_t appliedCount = 0;
	for (size_t i = 0; i < patches.size(); ++i) {
		const XeSSMfgPatch& patch = patches[i];
		uint8_t* const target = const_cast<uint8_t*>(base + patch.rva);
		if (memcmp(target, finalBytes[i].data(), patch.length) == 0) {
			++appliedCount;
			continue;
		}
		DWORD oldProtect = 0;
		if (!VirtualProtect(target, patch.length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			Logger::Get().Warn(fmt::format(
				"XeSS-FG MFG unlock: {} at {:#x} VirtualProtect failed ({:#x})",
				patch.name, patch.rva, GetLastError()));
			break;
		}
		std::memcpy(target, finalBytes[i].data(), patch.length);
		FlushInstructionCache(GetCurrentProcess(), target, patch.length);
		VirtualProtect(target, patch.length, oldProtect, &oldProtect);
		if (memcmp(target, finalBytes[i].data(), patch.length) != 0) {
			// 写校验失败：还原期望字节（回滚本条）。
			std::memcpy(target, patch.expected.data(), patch.length);
			FlushInstructionCache(GetCurrentProcess(), target, patch.length);
			Logger::Get().Warn(fmt::format(
				"XeSS-FG MFG unlock: {} at {:#x} failed write verification, rolled back",
				patch.name, patch.rva));
			break;
		}
		++appliedCount;
	}
	if (appliedCount != patches.size()) {
		// 回滚本次已写入的其余条目。
		for (size_t i = 0; i < patches.size() && i < appliedCount; ++i) {
			const XeSSMfgPatch& patch = patches[i];
			uint8_t* const target = const_cast<uint8_t*>(base + patch.rva);
			if (memcmp(target, finalBytes[i].data(), patch.length) == 0) {
				DWORD oldProtect = 0;
				if (VirtualProtect(target, patch.length,
						PAGE_EXECUTE_READWRITE, &oldProtect)) {
					std::memcpy(target, patch.expected.data(), patch.length);
					FlushInstructionCache(GetCurrentProcess(), target, patch.length);
					VirtualProtect(target, patch.length, oldProtect, &oldProtect);
				}
			}
		}
		return false;
	}
	Logger::Get().Info(fmt::format(
		"XeSS-FG MFG unlock: {} patches applied (libxess_fg 1.3.1.78, {}X)",
		appliedCount, interpolationCount + 1));
	return true;
}

}

struct XeSSFGPresenter::Impl {
	~Impl();

	HWND hwnd = NULL;
	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	IDXGIFactory7* factory = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, BUFFER_COUNT> allocators12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<IDXGISwapChain4> swapChain;
	std::array<winrt::com_ptr<ID3D12Resource>, BUFFER_COUNT> backBuffers;

	winrt::com_ptr<ID3D11Texture2D> color11;
	winrt::com_ptr<ID3D11RenderTargetView> colorRtv11;
	winrt::com_ptr<ID3D12Resource> color12;
	winrt::com_ptr<ID3D11Texture2D> motion11;
	winrt::com_ptr<ID3D11UnorderedAccessView> motionUav11;
	winrt::com_ptr<ID3D12Resource> motion12;
	winrt::com_ptr<ID3D12Resource> zeroMotion12;
	winrt::com_ptr<ID3D12Resource> flatDepth12;
	winrt::com_ptr<ID3D12DescriptorHeap> clearHeap12;

	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	std::array<uint64_t, BUFFER_COUNT> allocatorFenceValues{};
	uint64_t fenceValue = 0;
	wil::unique_event_nothrow fenceEvent;
	wil::unique_event_nothrow frameLatencyWaitableObject;
	FrameLatencyGate frameLatencyGate;
	winrt::com_ptr<IDCompositionDesktopDevice> overlayDCompDevice;
	winrt::com_ptr<IDCompositionTarget> overlayDCompTarget;
	winrt::com_ptr<IDCompositionVisual2> overlayDCompVisual;
	winrt::com_ptr<IDCompositionVirtualSurface> overlayDCompSurface;
	bool overlayDrawActive = false;

	xell_context_handle_t xell = nullptr;
	xefg_swapchain_handle_t xefg = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t frameId = 1;
	FrameGuidanceFrameId guidanceFrameId = 0;
	FrameGuidanceFrameId lastSubmittedGuidanceFrameId = 0;
	uint32_t multiplier = 2;
	uint32_t limiterIntervalUs = 0;
	uint32_t consecutiveFailures = 0;
	bool frameGenerationEnabled = false;
	bool externalMotionEnabled = false;
	bool externalMotionValid = false;
	bool externalMotionReset = true;
	bool resetHistory = true;
	bool hdrEnabled = false;
	std::chrono::steady_clock::time_point lastPresent{};
	// 残差转移组合的节奏控制（parity 真值驱动）。parity 与发布时刻由前端在
	// _SubmitFrontendFrame 按消费槽位传入（SetReuseParity）——不再读后端
	// 发布时写的单一原子：偶帧发布后 1~3ms 奇帧即跟发布，EndFrame 时读原子
	// 几乎必然读到下一帧的 parity，偶帧被系统性误判为奇帧（实测
	// odd=110/120）。P 的估计只用偶帧的发布时刻间隔（后端到达，不受 hold/
	// 呈现延迟影响，无自反馈）。前端线程独占读写。
	int32_t frameParity = -1;
	int64_t framePublishNs = 0;
	int64_t lastEvenPublishNs = 0;
	uint32_t pairRejectCount = 0;
	float pairPeriodMs = 0.0f;
	// present 间隔诊断统计（120-present 窗口,验证节奏均匀性）
	std::chrono::steady_clock::time_point prevPresent{};
	bool prevPresentValid = false;
	int presentStatCount = 0;
	int presentOddCount = 0;
	double presentIntervalSumMs = 0.0;
	double presentIntervalMinMs = 0.0;
	double presentIntervalMaxMs = 0.0;
	int presentHeldCount = 0;
};

static bool WaitForFence(XeSSFGPresenter::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) {
		return true;
	}
	if (FAILED(impl.fence12->SetEventOnCompletion(value, impl.fenceEvent.get()))) {
		return false;
	}
	return WaitForSingleObject(impl.fenceEvent.get(), 3000) == WAIT_OBJECT_0;
}

// 到绝对时刻的高精度等待（EndFrame 前端 present 线程专用,单线程无并发）:
// 高分辨率 waitable timer 睡到剩 1.5ms,YieldProcessor 自旋收尾,抖动 <1ms。
// Sleep 的 15ms 档误差不可接受,也不复用 WaitForFramePacing——它会因消息
// 到达提前返回,而 present 不能被消息打断。等待期间不泵消息（EndFrame 深处
// dispatch 有重入风险）:消息在队列排队,hold（最长 ~P/2）结束后由外层消息
// 循环正常处理,不会假死。timer 创建/设置失败时退化为整毫秒 Sleep 兜底。
static void HighResWaitUntil(std::chrono::steady_clock::time_point target) noexcept {
	using namespace std::chrono;
	// 高分辨率 timer;失败时退化为普通 timer（Sleep 精度档,仅极端环境）。
	static wil::unique_handle timer(CreateWaitableTimerExW(nullptr, nullptr,
		CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE));
	constexpr nanoseconds spinTail{ 1500 };
	while (true) {
		const auto now = steady_clock::now();
		if (now >= target) return;
		const auto remaining = duration_cast<nanoseconds>(target - now);
		if (remaining <= spinTail) break;
		LARGE_INTEGER due{ .QuadPart = -((remaining - spinTail).count() + 99) / 100 };
		if (!timer || !SetWaitableTimerEx(timer.get(), &due, 0,
			nullptr, nullptr, nullptr, 0)) {
			const DWORD ms = static_cast<DWORD>(duration_cast<milliseconds>(
				remaining - spinTail).count());
			if (ms > 0) Sleep(ms);
			break;
		}
		WaitForSingleObject(timer.get(), INFINITE);
	}
	while (steady_clock::now() < target) YieldProcessor();
}

static bool WaitForQueue(XeSSFGPresenter::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	return SUCCEEDED(impl.queue12->Signal(impl.fence12.get(), value)) &&
		WaitForFence(impl, value);
}

XeSSFGPresenter::Impl::~Impl() {
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}

	backBuffers = {};
	swapChain = nullptr;
	frameLatencyWaitableObject.reset();
	if (xefg) {
		xefgSwapChainSetEnabled(xefg, false);
		const xefg_swapchain_result_t result = xefgSwapChainDestroy(xefg);
		if (!XeFGSucceeded(result)) {
			LogXeFGResult("destroy failed", result);
		}
		xefg = nullptr;
	}
	if (xell) {
		xellDestroyContext(xell);
		xell = nullptr;
	}
}

static bool CreateSharedColor(XeSSFGPresenter::Impl& impl) noexcept {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = impl.width;
	desc.Height = impl.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = ColorFormat(impl.hdrEnabled);
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, impl.color11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateRenderTargetView(
			impl.color11.get(), nullptr, impl.colorRtv11.put());
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG D3D11 output texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = impl.color11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Query XeSSFG shared output resource failed", hr);
		return false;
	}
	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG shared output handle failed", hr);
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(impl.color12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open XeSSFG shared output in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool CreateSharedMotion(XeSSFGPresenter::Impl& impl) noexcept {
	if (!impl.externalMotionEnabled) {
		return true;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = impl.width;
	desc.Height = impl.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, impl.motion11.put());
	if (SUCCEEDED(hr)) {
		hr = impl.device11->CreateUnorderedAccessView(
			impl.motion11.get(), nullptr, impl.motionUav11.put());
	}
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	if (SUCCEEDED(hr)) {
		hr = impl.motion11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	}
	HANDLE rawHandle = nullptr;
	if (SUCCEEDED(hr)) {
		hr = dxgiResource->CreateSharedHandle(
			nullptr, GENERIC_ALL, nullptr, &rawHandle);
	}
	wil::unique_handle handle(rawHandle);
	if (SUCCEEDED(hr)) {
		hr = impl.device12->OpenSharedHandle(
			handle.get(), IID_PPV_ARGS(impl.motion12.put()));
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG D3D11/D3D12 motion interop failed", hr);
		return false;
	}
	static constexpr float ZERO[4]{};
	impl.context11->ClearUnorderedAccessViewFloat(impl.motionUav11.get(), ZERO);
	return true;
}

static bool CreateFlatResource(
	XeSSFGPresenter::Impl& impl,
	DXGI_FORMAT format,
	winrt::com_ptr<ID3D12Resource>& resource,
	D3D12_CPU_DESCRIPTOR_HANDLE cpu,
	D3D12_GPU_DESCRIPTOR_HANDLE gpu,
	const float clearValue[4],
	D3D12_RESOURCE_BARRIER& barrier
) noexcept {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = impl.width;
	desc.Height = impl.height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = impl.device12->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(resource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG virtual input failed", hr);
		return false;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl.device12->CreateUnorderedAccessView(resource.get(), nullptr, &uav, cpu);
	impl.commandList12->ClearUnorderedAccessViewFloat(
		gpu, cpu, resource.get(), clearValue, 0, nullptr);

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = {
		resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	return true;
}

static bool CreateSizeDependentResources(XeSSFGPresenter::Impl& impl) noexcept {
	if (!CreateSharedColor(impl) || !CreateSharedMotion(impl)) {
		return false;
	}

	for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
		HRESULT hr = impl.swapChain->GetBuffer(i, IID_PPV_ARGS(impl.backBuffers[i].put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Get XeSSFG back buffer failed", hr);
			return false;
		}
	}

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HRESULT hr = impl.device12->CreateDescriptorHeap(
		&heapDesc, IID_PPV_ARGS(impl.clearHeap12.put()));
	if (FAILED(hr)) {
		return false;
	}

	if (!WaitForQueue(impl)) {
		return false;
	}
	hr = impl.allocators12[0]->Reset();
	if (SUCCEEDED(hr)) {
		hr = impl.commandList12->Reset(impl.allocators12[0].get(), nullptr);
	}
	if (FAILED(hr)) {
		return false;
	}
	ID3D12DescriptorHeap* heaps[]{ impl.clearHeap12.get() };
	impl.commandList12->SetDescriptorHeaps(1, heaps);
	const UINT stride = impl.device12->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		impl.clearHeap12->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu =
		impl.clearHeap12->GetGPUDescriptorHandleForHeapStart();
	D3D12_RESOURCE_BARRIER barriers[2]{};
	static constexpr float ZERO[4]{};
	static constexpr float FLAT_DEPTH[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
	if (!CreateFlatResource(impl, DXGI_FORMAT_R16G16_FLOAT,
		impl.zeroMotion12, cpu, gpu, ZERO, barriers[0])) {
		return false;
	}
	cpu.ptr += stride;
	gpu.ptr += stride;
	if (!CreateFlatResource(impl, DXGI_FORMAT_R32_FLOAT,
		impl.flatDepth12, cpu, gpu, FLAT_DEPTH, barriers[1])) {
		return false;
	}
	impl.commandList12->ResourceBarrier(2, barriers);
	hr = impl.commandList12->Close();
	if (FAILED(hr)) {
		return false;
	}
	ID3D12CommandList* lists[]{ impl.commandList12.get() };
	impl.queue12->ExecuteCommandLists(1, lists);
	return WaitForQueue(impl);
}

static void ReleaseSizeDependentResources(XeSSFGPresenter::Impl& impl) noexcept {
	impl.backBuffers = {};
	impl.colorRtv11 = nullptr;
	impl.color11 = nullptr;
	impl.color12 = nullptr;
	impl.motionUav11 = nullptr;
	impl.motion11 = nullptr;
	impl.motion12 = nullptr;
	impl.zeroMotion12 = nullptr;
	impl.flatDepth12 = nullptr;
	impl.clearHeap12 = nullptr;
}

bool XeSSFGPresenter::_ResizeOverlaySurface() noexcept {
	if (!_impl) {
		return false;
	}
	Impl& impl = *_impl;
	HRESULT hr = S_OK;
	if (!impl.overlayDCompDevice) {
		hr = DCompositionCreateDevice3(
			impl.device11, IID_PPV_ARGS(impl.overlayDCompDevice.put()));
		if (SUCCEEDED(hr)) {
			hr = impl.overlayDCompDevice->CreateTargetForHwnd(
				impl.hwnd, TRUE, impl.overlayDCompTarget.put());
		}
		if (SUCCEEDED(hr)) {
			hr = impl.overlayDCompDevice->CreateVisual(impl.overlayDCompVisual.put());
		}
		if (SUCCEEDED(hr)) {
			hr = impl.overlayDCompTarget->SetRoot(impl.overlayDCompVisual.get());
		}
		if (FAILED(hr)) {
			Logger::Get().ComError("Create XeSSFG independent UI visual failed", hr);
			return false;
		}
	}

	if (impl.overlayDCompSurface) {
		hr = impl.overlayDCompSurface->Resize(impl.width, impl.height);
	} else {
		hr = impl.overlayDCompDevice->CreateVirtualSurface(
			impl.width, impl.height, OverlayFormat(impl.hdrEnabled),
			DXGI_ALPHA_MODE_PREMULTIPLIED, impl.overlayDCompSurface.put());
		if (SUCCEEDED(hr)) {
			hr = impl.overlayDCompVisual->SetContent(impl.overlayDCompSurface.get());
		}
	}
	if (SUCCEEDED(hr)) {
		hr = impl.overlayDCompDevice->Commit();
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Resize XeSSFG independent UI surface failed", hr);
		return false;
	}
	return true;
}

XeSSFGPresenter::XeSSFGPresenter(
	XeSSFGVariant variant,
	uint32_t requestedMultiplier,
	bool useExternalMotion
) :
	_variant(variant),
	_requestedMultiplier(requestedMultiplier),
	_useExternalMotion(useExternalMotion),
	_initializationError(requestedMultiplier > 2 ?
		ScalingError::XeSSMfgUnsupported : ScalingError::ScalingFailedGeneral) {}
XeSSFGPresenter::~XeSSFGPresenter() noexcept = default;

bool XeSSFGPresenter::_Initialize(HWND hwndAttach) noexcept {
	if ((_variant == XeSSFGVariant::X2 && _requestedMultiplier != 2) ||
		(_variant == XeSSFGVariant::MultiFrame &&
			(_requestedMultiplier < 2 || _requestedMultiplier > 4))) {
		Logger::Get().Error(fmt::format(
			"Invalid XeSSFG multiplier: variant={}, requested={}x",
			_variant == XeSSFGVariant::X2 ? "x2" : "MFG",
			_requestedMultiplier));
		_initializationError = _variant == XeSSFGVariant::MultiFrame ?
			ScalingError::XeSSMfgMultiplierUnsupported :
			ScalingError::ScalingFailedGeneral;
		return false;
	}

	auto impl = std::make_unique<Impl>();
	impl->hwnd = hwndAttach;
	impl->device11 = _deviceResources->GetD3DDevice();
	impl->context11 = _deviceResources->GetD3DDC();
	impl->factory = _deviceResources->GetDXGIFactory();
	const SIZE size = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
	impl->width = static_cast<uint32_t>(size.cx);
	impl->height = static_cast<uint32_t>(size.cy);
	impl->externalMotionEnabled = _useExternalMotion;
	impl->hdrEnabled = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled();

	HRESULT hr = D3D12CreateDevice(
		_deviceResources->GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(impl->device12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG D3D12 device failed", hr);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(
		&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	for (uint32_t i = 0; SUCCEEDED(hr) && i < BUFFER_COUNT; ++i) {
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(impl->allocators12[i].put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocators12[0].get(),
			nullptr, IID_PPV_ARGS(impl->commandList12.put()));
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG D3D12 command objects failed", hr);
		return false;
	}
	impl->commandList12->Close();

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG D3D11 shared fence failed", hr);
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSSFG shared fence handle failed", hr);
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open XeSSFG shared fence in D3D12 failed", hr);
		return false;
	}
	if (!impl->fenceEvent.try_create(wil::EventOptions::None, nullptr)) {
		Logger::Get().Win32Error("Create XeSSFG fence event failed");
		return false;
	}

	xell_result_t xellResult = xellD3D12CreateContext(impl->device12.get(), &impl->xell);
	if (!XeLLSucceeded(xellResult)) {
		Logger::Get().Error(fmt::format("Create XeSSFG XeLL context failed ({})",
			static_cast<int32_t>(xellResult)));
		return false;
	}
	xell_sleep_params_t sleepParams{};
	sleepParams.bLowLatencyMode = 1;
	xellResult = xellSetSleepMode(impl->xell, &sleepParams);
	if (!XeLLSucceeded(xellResult)) {
		Logger::Get().Error(fmt::format("Enable XeSSFG XeLL low latency mode failed ({})",
			static_cast<int32_t>(xellResult)));
		return false;
	}

	// 非 Intel MFG 解锁：在 CreateContext 之前对 libxess_fg 应用进程内补丁
	// （见 ApplyXeSSMfgUnlock；OptiScaler 同样在上下文创建前应用，避免
	// CreateContext 内部缓存钳制值）。x2 变体与 Intel 适配器完全不触发；
	// x2 路径零改动。
	bool mfgUnlocked = false;
	if (_variant == XeSSFGVariant::MultiFrame && _requestedMultiplier > 2) {
		DXGI_ADAPTER_DESC1 adapterDesc{};
		_deviceResources->GetGraphicsAdapter()->GetDesc1(&adapterDesc);
		if (adapterDesc.VendorId != 0x8086) {
			mfgUnlocked = ApplyXeSSMfgUnlock(_requestedMultiplier - 1);
		}
	}

	xefg_swapchain_result_t result = xefgSwapChainD3D12CreateContext(
		impl->device12.get(), &impl->xefg);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("context creation failed", result);
		return false;
	}
	xefgSwapChainSetLoggingCallback(
		impl->xefg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XeFGLogCallback, nullptr);
	result = xefgSwapChainSetLatencyReduction(impl->xefg, impl->xell);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("XeLL connection failed", result);
		return false;
	}
	xefg_swapchain_properties_t properties{};
	result = xefgSwapChainGetProperties(impl->xefg, &properties);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("query interpolation support failed", result);
		return false;
	}
	const uint32_t requestedInterpolations = _requestedMultiplier - 1;
	Logger::Get().Info(fmt::format(
		"XeSSFG capabilities: variant={}, requested={}x, "
		"maxSupportedInterpolations={} unlock={}",
		_variant == XeSSFGVariant::X2 ? "x2" : "MFG",
		_requestedMultiplier, properties.maxSupportedInterpolations,
		mfgUnlocked ? "applied" : "off"));
	if (properties.maxSupportedInterpolations < requestedInterpolations) {
		if (!mfgUnlocked) {
			Logger::Get().Error(fmt::format(
				"XeSSFG {}x is unsupported: hardware supports at most {}x",
				_requestedMultiplier, properties.maxSupportedInterpolations + 1));
			_initializationError = _requestedMultiplier > 2 ?
				ScalingError::XeSSMfgMultiplierUnsupported :
				ScalingError::ScalingFailedGeneral;
			return false;
		}
		// 已解锁但上报仍不足：以 USE_MAX_SUPPORTED 哨兵初始化，init 后按
		// GetInitializationParameters 的实际生效值做最终裁决。
		Logger::Get().Warn(fmt::format(
			"XeSSFG MFG unlock applied but properties report {} < {}; "
			"initializing with USE_MAX_SUPPORTED sentinel",
			properties.maxSupportedInterpolations, requestedInterpolations));
	}
	const bool useSentinel =
		properties.maxSupportedInterpolations < requestedInterpolations;
	const uint32_t interpolatedFrames = requestedInterpolations;
	impl->multiplier = _requestedMultiplier;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
	swapChainDesc.Width = impl->width;
	swapChainDesc.Height = impl->height;
	swapChainDesc.Format = ColorFormat(impl->hdrEnabled);
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = BUFFER_COUNT;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
		(_deviceResources->IsTearingSupported() && ScalingWindow::Get().Options().isVRREnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

	xefg_swapchain_d3d12_init_params_t initParams{};
	initParams.maxInterpolatedFrames = useSentinel ?
		XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES : interpolatedFrames;
	initParams.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
	result = xefgSwapChainD3D12InitFromSwapChainDesc(
		impl->xefg, hwndAttach, &swapChainDesc, nullptr,
		impl->queue12.get(), impl->factory, &initParams);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("proxy swap-chain initialization failed", result);
		return false;
	}
	result = xefgSwapChainD3D12GetSwapChainPtr(
		impl->xefg, IID_PPV_ARGS(impl->swapChain.put()));
	if (!XeFGSucceeded(result) || !impl->swapChain) {
		LogXeFGResult("get proxy swap chain failed", result);
		return false;
	}
	if (impl->hdrEnabled) {
		HRESULT colorSpaceHr = impl->swapChain->SetColorSpace1(
			DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		if (FAILED(colorSpaceHr)) {
			Logger::Get().ComError("Set XeSSFG HDR10/BT.2100 color space failed", colorSpaceHr);
			return false;
		}
		Logger::Get().Info("XeSSFG endpoint: format=R10G10B10A2_UNORM colorSpace=HDR10/BT.2100");
	}
	impl->swapChain->SetMaximumFrameLatency(1);
	impl->frameLatencyWaitableObject.reset(
		impl->swapChain->GetFrameLatencyWaitableObject());
	if (!impl->frameLatencyWaitableObject) {
		Logger::Get().Error("Get XeSSFG frame latency object failed");
		return false;
	}
	impl->factory->MakeWindowAssociation(hwndAttach, DXGI_MWA_NO_ALT_ENTER);

	result = xefgSwapChainSetNumInterpolatedFrames(impl->xefg, interpolatedFrames);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("set requested interpolation count failed", result);
		return false;
	}
	if (useSentinel) {
		// 哨兵初始化的最终裁决：SDK 实际生效的插帧数必须达到请求值。
		xefg_swapchain_d3d12_init_params_t applied{};
		const xefg_swapchain_result_t appliedResult =
			xefgSwapChainD3D12GetInitializationParameters(impl->xefg, &applied);
		if (!XeFGSucceeded(appliedResult) ||
			applied.maxInterpolatedFrames < interpolatedFrames) {
			Logger::Get().Error(fmt::format(
				"XeSSFG MFG init applied only {} interpolated frames (requested {})",
				XeFGSucceeded(appliedResult) ? applied.maxInterpolatedFrames : 0,
				interpolatedFrames));
			_initializationError = ScalingError::XeSSMfgMultiplierUnsupported;
			return false;
		}
		Logger::Get().Info(fmt::format(
			"XeSSFG MFG sentinel init confirmed: {} interpolated frames",
			applied.maxInterpolatedFrames));
	}
	result = xefgSwapChainSetEnabled(impl->xefg, true);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("enable failed", result);
		return false;
	}
	impl->frameGenerationEnabled = true;

	if (!CreateSizeDependentResources(*impl)) {
		return false;
	}

	_impl = std::move(impl);
	if (!_ResizeOverlaySurface()) {
		_impl.reset();
		return false;
	}
	Logger::Get().Info(fmt::format(
		"XeSSFG initialized: {}x{}, multiplier={}x, motion={}, flat depth, "
		"XeLL enabled, independent UI layer",
		_impl->width, _impl->height, _impl->multiplier,
		_impl->externalMotionEnabled ? "external optical flow" : "Zero-MV"));
	return true;
}

void XeSSFGPresenter::SetReuseParity(int32_t parity, int64_t publishNs) noexcept {
	if (!_impl) {
		return;
	}
	// 前端线程在 _SubmitFrontendFrame 调用（按本帧消费的槽位取值）。
	_impl->frameParity = parity;
	_impl->framePublishNs = publishNs;
}

void XeSSFGPresenter::SetFrameGuidance(
	ID3D11Texture2D* motion,
	FrameGuidanceFrameId frameId,
	bool requiresHistoryReset,
	const RECT& destinationRect
) noexcept {
	if (!_impl || !_impl->externalMotionEnabled) {
		return;
	}
	Impl& impl = *_impl;
	impl.guidanceFrameId = frameId;
	impl.externalMotionValid = false;
	impl.externalMotionReset = true;
	if (!motion || !impl.motion11 || !impl.motionUav11 || frameId == 0 ||
		frameId <= impl.lastSubmittedGuidanceFrameId) {
		return;
	}

	D3D11_TEXTURE2D_DESC sourceDesc{};
	motion->GetDesc(&sourceDesc);
	const LONG destinationWidth = destinationRect.right - destinationRect.left;
	const LONG destinationHeight = destinationRect.bottom - destinationRect.top;
	if (sourceDesc.Format != DXGI_FORMAT_R16G16_FLOAT ||
		destinationRect.left < 0 || destinationRect.top < 0 ||
		destinationRect.right > static_cast<LONG>(impl.width) ||
		destinationRect.bottom > static_cast<LONG>(impl.height) ||
		destinationWidth != static_cast<LONG>(sourceDesc.Width) ||
		destinationHeight != static_cast<LONG>(sourceDesc.Height)) {
		Logger::Get().Warn(fmt::format(
			"XeSSFG motion frame mismatch: frameId={}, motion={}x{} format={}, "
			"destination={},{},{},{}; using Zero Motion",
			frameId, sourceDesc.Width, sourceDesc.Height,
			static_cast<uint32_t>(sourceDesc.Format),
			destinationRect.left, destinationRect.top,
			destinationRect.right, destinationRect.bottom));
		return;
	}

	static constexpr float ZERO[4]{};
	impl.context11->ClearUnorderedAccessViewFloat(impl.motionUav11.get(), ZERO);
	impl.context11->CopySubresourceRegion(
		impl.motion11.get(), 0,
		static_cast<UINT>(destinationRect.left),
		static_cast<UINT>(destinationRect.top), 0,
		motion, 0, nullptr);
	impl.externalMotionValid = true;
	impl.externalMotionReset = requiresHistoryReset ||
		(impl.lastSubmittedGuidanceFrameId != 0 && frameId != impl.lastSubmittedGuidanceFrameId + 1);

}

bool XeSSFGPresenter::SetBaseFrameRateLimit(double baseFPS) noexcept {
	if (!_impl) return false;
	auto& impl = *_impl;
	const double outputFPS = baseFPS * (impl.frameGenerationEnabled ? impl.multiplier : 1u);
	const uint32_t intervalUs = outputFPS > 0 ?
		static_cast<uint32_t>(std::max(1.0, std::round(1'000'000.0 / outputFPS))) : 0;
	if (intervalUs == impl.limiterIntervalUs) return true;
	// Only configuration changes drain outstanding work, never every frame.
	_WaitForGpu();
	if (!WaitForQueue(impl)) return false;
	xell_sleep_params_t params{};
	params.bLowLatencyMode = 1;
	params.minimumIntervalUs = intervalUs;
	const auto result = xellSetSleepMode(impl.xell, &params);
	if (!XeLLSucceeded(result)) {
		Logger::Get().Error(fmt::format("XeLL input frame limit failed ({})", static_cast<int32_t>(result)));
		return false;
	}
	impl.limiterIntervalUs = intervalUs;
	Logger::Get().Info(fmt::format("XeLL input pacing: baseTarget={:.3f} outputTarget={:.3f} intervalUs={} VRR={}",
		baseFPS, outputFPS, intervalUs, ScalingWindow::Get().Options().isVRREnabled));
	return true;
}

bool XeSSFGPresenter::BeginFrame(
	winrt::com_ptr<ID3D11Texture2D>& frameTex,
	winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
	POINT& drawOffset
) noexcept {
	if (!_impl || !_impl->color11 || !_impl->colorRtv11) {
		return false;
	}
	Impl& impl = *_impl;
	const DWORD capacity = impl.frameLatencyGate.TryAcquire(impl.frameLatencyWaitableObject.get());
	if (capacity == WAIT_TIMEOUT) {
		return false;
	}
	if (capacity != WAIT_OBJECT_0) {
		Logger::Get().Win32Error("XeSSFG frame latency wait failed");
		return false;
	}
	xellSleep(impl.xell, impl.frameId);
	xellAddMarkerData(impl.xell, impl.frameId, XELL_INPUT_SAMPLE);
	xellAddMarkerData(impl.xell, impl.frameId, XELL_SIMULATION_START);
	drawOffset = {};
	frameTex = impl.color11;
	frameRtv = impl.colorRtv11;
	return true;
}

bool XeSSFGPresenter::WaitForFrameCapacity(DWORD timeout) noexcept {
	if (!_impl) return false;
	DWORD result = WAIT_TIMEOUT;
	if (!_impl->frameLatencyGate.Wait(_impl->frameLatencyWaitableObject.get(), timeout, result)) return false;
	if (result == WAIT_FAILED) {
		Logger::Get().Win32Error("XeSSFG capacity event wait failed");
		return false;
	}
	return true;
}

bool XeSSFGPresenter::HasIndependentOverlay() const noexcept {
	return _impl && _impl->overlayDCompSurface;
}

bool XeSSFGPresenter::BeginOverlayFrame(
	winrt::com_ptr<ID3D11Texture2D>& frameTex,
	winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
	POINT& drawOffset
) noexcept {
	if (!_impl || !_impl->overlayDCompSurface || _impl->overlayDrawActive) {
		return false;
	}
	Impl& impl = *_impl;
	HRESULT hr = impl.overlayDCompSurface->BeginDraw(
		nullptr, IID_PPV_ARGS(&frameTex), &drawOffset);
	if (FAILED(hr)) {
		Logger::Get().ComError("Begin XeSSFG independent UI draw failed", hr);
		return false;
	}
	impl.overlayDrawActive = true;
	hr = impl.device11->CreateRenderTargetView(
		frameTex.get(), nullptr, frameRtv.put());
	if (FAILED(hr)) {
		impl.overlayDCompSurface->EndDraw();
		impl.overlayDrawActive = false;
		Logger::Get().ComError("Create XeSSFG independent UI RTV failed", hr);
		return false;
	}
	return true;
}

bool XeSSFGPresenter::EndOverlayFrame() noexcept {
	if (!_impl || !_impl->overlayDrawActive) {
		return false;
	}
	Impl& impl = *_impl;
	impl.overlayDrawActive = false;
	HRESULT hr = impl.overlayDCompSurface->EndDraw();
	if (SUCCEEDED(hr)) {
		hr = impl.overlayDCompDevice->Commit();
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Commit XeSSFG independent UI frame failed", hr);
		return false;
	}
	return true;
}

static void SetIdentity(float matrix[16]) noexcept {
	for (uint32_t row = 0; row < 4; ++row) {
		for (uint32_t column = 0; column < 4; ++column) {
			matrix[row * 4 + column] = row == column ? 1.0f : 0.0f;
		}
	}
}

bool XeSSFGPresenter::EndFrame(bool waitForGpu) noexcept {
	if (!_impl) {
		return false;
	}
	Impl& impl = *_impl;
	impl.frameLatencyGate.Reset();
	xellAddMarkerData(impl.xell, impl.frameId, XELL_SIMULATION_END);
	xellAddMarkerData(impl.xell, impl.frameId, XELL_RENDERSUBMIT_START);

	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		return false;
	}
	impl.context11->Flush();
	if (FAILED(impl.queue12->Wait(impl.fence12.get(), inputReady))) {
		return false;
	}

	const uint32_t bufferIndex = impl.swapChain->GetCurrentBackBufferIndex();
	if (!WaitForFence(impl, impl.allocatorFenceValues[bufferIndex])) {
		Logger::Get().Error("XeSSFG command allocator wait timed out");
		return false;
	}
	hr = impl.allocators12[bufferIndex]->Reset();
	if (SUCCEEDED(hr)) {
		hr = impl.commandList12->Reset(impl.allocators12[bufferIndex].get(), nullptr);
	}
	if (FAILED(hr)) {
		return false;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition = {
		impl.color12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE
	};
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition = {
		impl.backBuffers[bufferIndex].get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST
	};
	impl.commandList12->ResourceBarrier(2, barriers);
	impl.commandList12->CopyResource(
		impl.backBuffers[bufferIndex].get(), impl.color12.get());
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

	// 残差转移组合的节奏控制（parity 真值驱动,详见 Impl 状态注释）。实测
	// 证明时间阈值分类不可行:奇帧的前端 present 间隔实际是 24-45ms（渲染
	// 管线延迟底线）而非背靠背 2ms,而偶帧间隔可低至 50ms——两类几乎重叠,
	// 0.4×EMA 阈值导致 hold 几乎不触发(实测 5/60)且把纯 XeSSFG 的输入抖动
	// 误判为 burst,污染 frameRenderTime 造成回归。parity 由后端发布时直接
	// 写入,零猜测:奇帧 hold 到「上次 present + P/2」,偶帧立即发布,P 用偶帧
	// 到达间隔的 EMA（不受 hold 影响）。parity==-1（未启用复用）时全部旁路,
	// 行为与上游完全一致。
	const int32_t parity = impl.frameGenerationEnabled ? impl.frameParity : -1;
	const auto arrivalNow = std::chrono::steady_clock::now();
	if (parity == 0 && impl.framePublishNs > 0) {
		// 偶帧:配对周期 EMA 只用发布时刻间隔（后端到达，不受 hold/呈现
		// 延迟影响，无自反馈）。拒收阈值 1.5×——原 2.5× 会级联接受
		// 60→150→400ms 的间隔把 EMA 拉爆（实测 pairMs=400、present 停顿
		// 190ms）；连续 3 次拒收说明节奏真实改变（读盘/场景切换结束），
		// 重置为新周期。
		if (impl.lastEvenPublishNs > 0 && impl.framePublishNs > impl.lastEvenPublishNs) {
			const float pairDeltaMs = static_cast<float>(
				impl.framePublishNs - impl.lastEvenPublishNs) / 1e6f;
			constexpr float kMinPairMs = 4.0f;
			if (impl.pairPeriodMs < kMinPairMs) {
				impl.pairPeriodMs = pairDeltaMs;
				impl.pairRejectCount = 0;
			} else if (pairDeltaMs > impl.pairPeriodMs * 1.5f) {
				// 场景切换/暂停后的长间隔:拒收;连续 3 次则重置。
				if (++impl.pairRejectCount >= 3) {
					impl.pairPeriodMs = pairDeltaMs;
					impl.pairRejectCount = 0;
				}
			} else {
				impl.pairPeriodMs = impl.pairPeriodMs * 0.5f + pairDeltaMs * 0.5f;
				impl.pairRejectCount = 0;
			}
		}
		impl.lastEvenPublishNs = impl.framePublishNs;
	}
	bool presentHeld = false;
	if (parity == 1 && impl.pairPeriodMs >= 4.0f &&
		impl.lastPresent.time_since_epoch().count() != 0) {
		auto target = impl.lastPresent + std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double, std::milli>(impl.pairPeriodMs * 0.5f));
		// 防呆:hold 上限 100ms（EMA 异常时保护 UI 响应）。
		const auto holdLimit = arrivalNow + std::chrono::milliseconds(100);
		if (target > holdLimit) target = holdLimit;
		if (arrivalNow < target) {
			HighResWaitUntil(target);
			presentHeld = true;
		}
	}

	if (impl.frameGenerationEnabled) {
		xefg_swapchain_d3d12_resource_data_t motion{};
		motion.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
		motion.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
		motion.resourceSize = { impl.width, impl.height };
		motion.pResource = impl.externalMotionValid ?
			impl.motion12.get() : impl.zeroMotion12.get();
		motion.incomingState = impl.externalMotionValid ?
			D3D12_RESOURCE_STATE_COMMON :
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		xefg_swapchain_result_t result = xefgSwapChainD3D12TagFrameResource(
			impl.xefg, nullptr, impl.frameId, &motion);

		xefg_swapchain_d3d12_resource_data_t depth{};
		depth.type = XEFG_SWAPCHAIN_RES_DEPTH;
		depth.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
		depth.resourceSize = { impl.width, impl.height };
		depth.pResource = impl.flatDepth12.get();
		depth.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		if (XeFGSucceeded(result)) {
			result = xefgSwapChainD3D12TagFrameResource(
				impl.xefg, nullptr, impl.frameId, &depth);
		}

		xefg_swapchain_frame_constant_data_t constants{};
		SetIdentity(constants.viewMatrix);
		SetIdentity(constants.projectionMatrix);
		constants.motionVectorScaleX = 1.0f;
		constants.motionVectorScaleY = 1.0f;
		constants.resetHistory =
			(impl.resetHistory || (impl.externalMotionEnabled && impl.externalMotionReset)) ? 1u : 0u;
		const auto now = std::chrono::steady_clock::now();
		if (impl.lastPresent.time_since_epoch().count() != 0) {
			if (now - impl.lastPresent >= std::chrono::milliseconds(500)) constants.resetHistory = 1;
			// 节奏欺骗（parity 驱动）:奇帧报告半配对周期,与 hold 后的真实
			// present 间隔一致——XeSS 看到的输入时刻分布均匀,插值时刻回归
			// 配对中点。偶帧报告真实值（hold 生效后也≈P/2,自然一致）。
			// parity==-1 时恒报真实值——与上游行为完全一致,零回归。
			if (parity == 1 && impl.pairPeriodMs >= 4.0f) {
				constants.frameRenderTime = std::max(1.0f, impl.pairPeriodMs * 0.5f);
			} else {
				constants.frameRenderTime = static_cast<float>(
					std::chrono::duration<double, std::milli>(now - impl.lastPresent).count());
			}
		}
		if (XeFGSucceeded(result)) {
			result = xefgSwapChainTagFrameConstants(
				impl.xefg, impl.frameId, &constants);
		}
		if (XeFGSucceeded(result)) {
			result = xefgSwapChainSetPresentId(impl.xefg, impl.frameId);
		}
		if (!XeFGSucceeded(result)) {
			LogXeFGResult("frame resource tagging failed", result);
			impl.resetHistory = true;
		} else {
			impl.resetHistory = false;
		}
	}


	xellAddMarkerData(impl.xell, impl.frameId, XELL_RENDERSUBMIT_END);
	xellAddMarkerData(impl.xell, impl.frameId, XELL_PRESENT_START);
	const UINT flags = ScalingWindow::Get().Options().isVRREnabled && _deviceResources->IsTearingSupported()
		? DXGI_PRESENT_ALLOW_TEARING : 0;
	const auto presentStart = std::chrono::steady_clock::now();
	const auto tracePresent = FrameTrace::Tick();
	hr = impl.swapChain->Present(0, flags);
	FrameTrace::Presentation(tracePresent, FrameTrace::Tick(), hr,
		reinterpret_cast<uintptr_t>(impl.swapChain.get()));
	_lastPresentedFrameCount = hr == S_OK ? std::optional<uint32_t>(1) : std::optional<uint32_t>(0);
	xellAddMarkerData(impl.xell, impl.frameId, XELL_PRESENT_END);
	impl.lastPresent = presentStart;
	// present 间隔诊断:量化验证节奏均匀性。组合模式预期:odd≈presents/2、
	// held≈odd、avgMs≈P/2、min/max 收敛在 P/2 附近、pairMs≈后端配对周期;
	// 纯 XeSSFG 预期:odd=0、held=0、avgMs≈真实输入间隔（零干预证据）。
	if (impl.prevPresentValid) {
		const float intervalMs = static_cast<float>(
			std::chrono::duration<double, std::milli>(presentStart - impl.prevPresent).count());
		if (impl.presentStatCount == 0) {
			impl.presentIntervalMinMs = intervalMs;
			impl.presentIntervalMaxMs = intervalMs;
		} else {
			impl.presentIntervalMinMs = std::min(impl.presentIntervalMinMs, static_cast<double>(intervalMs));
			impl.presentIntervalMaxMs = std::max(impl.presentIntervalMaxMs, static_cast<double>(intervalMs));
		}
		if (parity == 1) ++impl.presentOddCount;
		if (presentHeld) ++impl.presentHeldCount;
		impl.presentIntervalSumMs += intervalMs;
		++impl.presentStatCount;
		if (impl.presentStatCount >= 120) {
			Logger::Get().Info(fmt::format(
				"XeSSFG present pacing: presents={} odd={} held={} avgMs={:.1f} minMs={:.1f} maxMs={:.1f} pairMs={:.1f}",
				impl.presentStatCount, impl.presentOddCount, impl.presentHeldCount,
				impl.presentIntervalSumMs / impl.presentStatCount,
				impl.presentIntervalMinMs, impl.presentIntervalMaxMs,
				impl.pairPeriodMs));
			impl.presentStatCount = 0;
			impl.presentOddCount = 0;
			impl.presentHeldCount = 0;
			impl.presentIntervalSumMs = 0.0;
		}
	}
	impl.prevPresent = presentStart;
	impl.prevPresentValid = true;

	if (FAILED(hr)) {
		Logger::Get().ComError("XeSSFG proxy Present failed", hr);
		impl.resetHistory = true;
	} else if (hr == S_OK && impl.frameGenerationEnabled) {
		xefg_swapchain_present_status_t status{};
		const xefg_swapchain_result_t statusResult =
			xefgSwapChainGetLastPresentStatus(impl.xefg, &status);
		_lastPresentedFrameCount = statusResult == XEFG_SWAPCHAIN_RESULT_SUCCESS ?
			std::optional<uint32_t>(status.framesPresented) : std::nullopt;
		if (!XeFGSucceeded(statusResult) || status.frameGenResult < 0) {
			++impl.consecutiveFailures;
			if (impl.consecutiveFailures == 1 || impl.consecutiveFailures == 3) {
				LogXeFGResult("frame generation failed", !XeFGSucceeded(statusResult) ? statusResult : status.frameGenResult);
			}
			impl.resetHistory = true;
			if (impl.consecutiveFailures >= 3) {
				xefgSwapChainSetEnabled(impl.xefg, false);
				impl.frameGenerationEnabled = false;
				Logger::Get().Error(
					"XeSSFG disabled for this scaling session after repeated failures");
			}
		} else {
			impl.consecutiveFailures = 0;
		}
	}

	const uint64_t copyDone = ++impl.fenceValue;
	HRESULT syncResult = impl.queue12->Signal(impl.fence12.get(), copyDone);
	if (SUCCEEDED(syncResult)) syncResult = impl.context11->Wait(impl.fence11.get(), copyDone);
	if (FAILED(syncResult)) {
		Logger::Get().ComError("XeSSFG post-present input synchronization failed", syncResult);
		impl.resetHistory = true;
		return false;
	}
	impl.allocatorFenceValues[bufferIndex] = copyDone;

	if (waitForGpu) {
		WaitForFence(impl, copyDone);
	}
	impl.context11->DiscardView(impl.colorRtv11.get());
	if (impl.externalMotionValid) {
		impl.lastSubmittedGuidanceFrameId = impl.guidanceFrameId;
	}
	impl.externalMotionValid = false;
	impl.externalMotionReset = true;
	++impl.frameId;
	return SUCCEEDED(hr);
}

bool XeSSFGPresenter::OnResize() noexcept {
	if (!_impl) {
		return false;
	}
	Impl& impl = *_impl;
	const SIZE size = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
	const uint32_t width = static_cast<uint32_t>(size.cx);
	const uint32_t height = static_cast<uint32_t>(size.cy);
	if (width == impl.width && height == impl.height) {
		return true;
	}

	xefgSwapChainSetEnabled(impl.xefg, false);
	impl.frameGenerationEnabled = false;
	if (!WaitForQueue(impl)) {
		return false;
	}
	ReleaseSizeDependentResources(impl);
	impl.frameLatencyGate.Reset();
	impl.frameLatencyWaitableObject.reset();
	const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
		(_deviceResources->IsTearingSupported() && ScalingWindow::Get().Options().isVRREnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	HRESULT hr = impl.swapChain->ResizeBuffers(
		BUFFER_COUNT, width, height, ColorFormat(impl.hdrEnabled), flags);
	if (FAILED(hr)) {
		Logger::Get().ComError("Resize XeSSFG proxy swap chain failed", hr);
		return false;
	}
	if (impl.hdrEnabled) {
		hr = impl.swapChain->SetColorSpace1(
			DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		if (FAILED(hr)) {
			Logger::Get().ComError(
				"Restore XeSSFG HDR10/BT.2100 color space after resize failed", hr);
			return false;
		}
	}
	impl.width = width;
	impl.height = height;
	impl.frameLatencyWaitableObject.reset(
		impl.swapChain->GetFrameLatencyWaitableObject());
	if (!impl.frameLatencyWaitableObject || !CreateSizeDependentResources(impl)) {
		return false;
	}
	const xefg_swapchain_result_t result = xefgSwapChainSetEnabled(impl.xefg, true);
	if (!XeFGSucceeded(result)) {
		LogXeFGResult("re-enable after resize failed", result);
		return false;
	}
	impl.frameGenerationEnabled = true;
	impl.resetHistory = true;
	impl.consecutiveFailures = 0;
	return _ResizeOverlaySurface();
}

}

#else

namespace Magpie {

struct XeSSFGPresenter::Impl {};
XeSSFGPresenter::XeSSFGPresenter(
	XeSSFGVariant variant,
	uint32_t requestedMultiplier,
	bool useExternalMotion
) :
	_variant(variant),
	_requestedMultiplier(requestedMultiplier),
	_useExternalMotion(useExternalMotion),
	_initializationError(requestedMultiplier > 2 ?
		ScalingError::XeSSMfgUnsupported : ScalingError::ScalingFailedGeneral) {}
XeSSFGPresenter::~XeSSFGPresenter() noexcept = default;
bool XeSSFGPresenter::_Initialize(HWND) noexcept {
	Logger::Get().Error("XeSS Frame Generation is disabled at build time");
	return false;
}
bool XeSSFGPresenter::BeginFrame(
	winrt::com_ptr<ID3D11Texture2D>&,
	winrt::com_ptr<ID3D11RenderTargetView>&,
	POINT&) noexcept {
	return false;
}
bool XeSSFGPresenter::EndFrame(bool) noexcept { return false; }
bool XeSSFGPresenter::SetBaseFrameRateLimit(double) noexcept { return false; }
bool XeSSFGPresenter::WaitForFrameCapacity(DWORD) noexcept { return false; }
void XeSSFGPresenter::SetReuseParity(int32_t, int64_t) noexcept {}
void XeSSFGPresenter::SetFrameGuidance(
	ID3D11Texture2D*, FrameGuidanceFrameId, bool, const RECT&) noexcept {}
bool XeSSFGPresenter::HasIndependentOverlay() const noexcept { return false; }
bool XeSSFGPresenter::BeginOverlayFrame(
	winrt::com_ptr<ID3D11Texture2D>&,
	winrt::com_ptr<ID3D11RenderTargetView>&,
	POINT&) noexcept { return false; }
bool XeSSFGPresenter::EndOverlayFrame() noexcept { return false; }
bool XeSSFGPresenter::OnResize() noexcept { return false; }
bool XeSSFGPresenter::_ResizeOverlaySurface() noexcept { return false; }

}

#endif
