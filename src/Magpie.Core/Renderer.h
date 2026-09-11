#pragma once
#include "BackendDescriptorStore.h"
#include "CursorDrawer.h"
#include "DeviceResources.h"
#include "EffectDrawer.h"
#include "EffectsProfiler.h"
#include "FrameGuidanceService.h"
#include "NgxD3D12Core.h"
#include "OverlayDrawer.h"
#include "PassThroughFrames.h"
#include "PresenterBase.h"
#include "ReflexController.h"
#include "PresentationFrameRate.h"
#include "FramePresentationTiming.h"
#include "ScalingOptions.h"
#include "ScalingSessionLifetime.h"
#include "StepTimer.h"
#include <mutex>
#include <unordered_map>

namespace Magpie {

class FrameSourceBase;

enum class DLSSFGFrameRenderResult : uint8_t {
	Presented,
	Retry,
	Dropped
};

class Renderer {
public:
	Renderer() noexcept;
	~Renderer() noexcept;

	Renderer(const Renderer&) = delete;
	Renderer(Renderer&&) = delete;

	ScalingError Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept;
	void BeginShutdown() noexcept;
	const std::string& InitializationContext() const noexcept { return _backendInitContext; }
	uint32_t InitializationSystemError() const noexcept { return _backendInitSystemError; }
	const std::wstring& MotionConfigurationNotice() const noexcept { return _motionConfigurationNotice; }

	bool Render(bool force = false, bool waitForGpu = false) noexcept;
	bool RenderOverlay() noexcept;
	// 帧复用奇帧呈现延迟查询：见 ScalingRuntime 渲染循环（方案 A）。
	bool ShouldDeferOddPresentation() noexcept;
	bool HasFrameGeneration() const noexcept { return _hasFrameGeneration; }
	PresentationRateSnapshot PresentationRate() const noexcept { return _presentationRate.Get(); }
	DLSSFGFrameRenderResult RenderDLSSFGFrame(
		uint32_t sharedTextureSlot,
		uint32_t sharedTextureGeneration,
		PresentationJobTiming& jobTiming
	) noexcept;
	bool HasPendingOverlayInput() const noexcept;
	bool HasUrgentOverlayInput() const noexcept;
	bool HasPendingContent() const noexcept;
	std::chrono::nanoseconds FrontendPollInterval() const noexcept {
		return _overlayPresentationClock.PollInterval();
	}

	// Sleep in the outer message pump, never inside a prepared frame.
	void WaitForFrontendWork(std::chrono::nanoseconds maximumWait) noexcept;
	bool OnResize() noexcept;

	void OnEndResize() noexcept;

	void OnMove() noexcept;

	void SwitchToolbarState() noexcept;
	void InvokeOverlayAction(OverlayAction action) noexcept;
	OverlaySessionState CaptureOverlayState() const noexcept { return _overlayDrawer.CaptureSessionState(); }
	bool IsEditingParameters() const noexcept { return _overlayDrawer.IsEditingParameters(); }
	bool IsParameterPreviewAt(POINT point) const noexcept { return _overlayDrawer.IsParameterPreviewAt(point); }
	HWND ParameterInputHandle() const noexcept { return _overlayDrawer.ParameterInputHandle(); }
	bool IsParameterFocusSettling() const noexcept { return _overlayDrawer.IsParameterFocusSettling(); }
	void SuspendParameterInput() noexcept { _overlayDrawer.SuspendParameterInput(); }
	void ReleaseParameterInput() noexcept { _overlayDrawer.ReleaseParameterInput(); }
	bool HasHeldParameterInput() const noexcept { return _overlayDrawer.HasHeldParameterInput(); }
	bool AllowAutomaticSourceFocus() const noexcept { return _overlayDrawer.AllowAutomaticSourceFocus(); }
	void UpdateParameterInputHost() noexcept { _overlayDrawer.UpdateParameterInputHost(); }
	void RefreshOverlay() noexcept { ++_overlayActionRevision; }
	void RestoreOverlayState(const OverlaySessionState& state) noexcept;
	bool IsPassThroughActive() const noexcept { return _isPassThroughActive; }
	bool SetPassThroughActive(bool value) noexcept;
	void TakeDisplayedScreenshot() noexcept {
		TakeScreenshot(std::numeric_limits<uint32_t>::max());
	}

	const RECT& SrcRect() const noexcept;

	// 屏幕坐标而不是窗口局部坐标
	const RECT& DestRect() const noexcept {
		return _destRect;
	}

	const FrameSourceBase& FrameSource() const noexcept {
		return *_frameSource;
	}

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory);

	void OnSourceFocusChanged() noexcept;

	bool MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
	void ClearOverlayStates() noexcept;
	bool IsEffectParameterInputActive() const noexcept { return _overlayDrawer.IsEffectParameterInputActive(); }
	bool IsEffectParametersVisible() const noexcept { return _overlayDrawer.IsEffectParametersVisible(); }
	FrameSyncBackend ActiveFrameSyncBackend() const noexcept {
		return _frameSyncBackend == FrameSyncBackend::Reflex && _reflex.CanUseAsync()
			? FrameSyncBackend::Async : _frameSyncBackend;
	}

	const std::vector<const EffectDesc*>& ActiveEffectDescs() const noexcept {
		return _activeEffectDescs;
	}

	const std::vector<std::vector<EffectParameterRuntimeInfo>>&
		EffectParameterRuntimeInfos() const noexcept {
		return _effectParameterRuntimeInfos;
	}

	bool QueueEffectParameterUpdate(
		uint32_t effectIdx,
		uint32_t parameterIdx,
		float value,
		bool waitForOverlaySave = true
	) noexcept;

	void StartProfile() noexcept;

	void StopProfile() noexcept;

	bool IsCursorOnOverlayCaptionArea() const noexcept {
		return _overlayDrawer.IsCursorOnCaptionArea();
	}

	winrt::fire_and_forget TakeScreenshot(
		uint32_t effectIdx,
		uint32_t passIdx = std::numeric_limits<uint32_t>::max(),
		uint32_t outputIdx = std::numeric_limits<uint32_t>::max()
	) noexcept;

private:
	bool _frameTraceStarted = false;
	// Set before starting the backend; immutable for this session.
	bool _frameSyncEnabled = false;
	FrameSyncBackend _frameSyncBackend = FrameSyncBackend::None;
	// Backend thread only: detects driver availability transitions and installs
	// the corresponding limiter before another capture can be accepted.
	FrameSyncBackend _appliedFrameSyncBackend = FrameSyncBackend::None;
	bool _reflexFallbackLogged = false;
	bool _frameSyncUsesSharedSlot = false;
	bool _frameSyncLimiterFailed = false;
	uint32_t _configuredFrameGenerationMultiplier = 1;
	std::atomic<double> _presentationRefreshRate = 60.0;
	std::atomic<double> _existingBaseFrameRateLimit = 0.0;
	double _FrameSyncFrameRate() const noexcept;
	FrontEdgeSyncClock _frontEdgeClock;
	std::optional<std::chrono::steady_clock::time_point> _frontendPacingDeadline;
	wil::unique_handle _frontendPacingTimer;
	std::atomic<uint64_t> _frameSyncAcknowledgedKey = 0;
	wil::unique_handle _frameSyncConsumedEvent;
	// Backend-owned staged input retains NR/SR and guidance until FG is due.
	FrontEdgeSyncClock _fgInputClock;
	wil::unique_handle _fgInputTimer;
	winrt::com_ptr<ID3D11Texture2D> _pendingFrameGenerationInput;
	bool _backendMayDeferFG = false;
	void _CompleteBackendFrame(ID3D11Texture2D* effectsOutput, bool isNewCaptureFrame,
		uint64_t captureSequence) noexcept;
	void _LogHdrTextureStats(ID3D11Texture2D* texture, std::string_view label) noexcept;
	struct PendingFrontendFrame {
		bool stableBaseOnly = false;
		bool contentFrame = false;
		bool generatedFrame = false;
		bool independentOverlay = false;
		bool waitForGpu = false;
		bool paced = false;
		uint64_t overlayRevision = 0;
		uint64_t contentKey = 0;
	};
	std::optional<PendingFrontendFrame> _pendingFrontendFrame;
	bool _SubmitFrontendFrame() noexcept;
	OverlayPresentationClock _overlayPresentationClock;
	HMONITOR _overlayMonitor = nullptr;
	uint64_t _overlayActionRevision = 0;
	uint64_t _presentedOverlayActionRevision = 0;
	bool _HasPendingOverlayAction() const noexcept {
		return _overlayActionRevision != _presentedOverlayActionRevision;
	}
	void _UpdateOverlayRefreshRate() noexcept;
	bool _CanRenderOverlay() const noexcept;
	struct FrontendRenderTimings {
		std::chrono::nanoseconds beginFrame{};
		std::chrono::nanoseconds draw{};
		std::chrono::nanoseconds endFrame{};
		bool capacityBusy = false;
	};

	bool _FrontendRender(
		bool waitForGpu = false,
		uint32_t sharedTextureSlot = std::numeric_limits<uint32_t>::max(),
		FrontendRenderTimings* timings = nullptr,
		bool stableBaseOnly = false,
		bool* droppedFrame = nullptr
	) noexcept;
	bool _FrontendOverlayRender(bool contentChanged = false) noexcept;
	enum class FrontendBaseResult { Ready, Retry, Dropped };
	FrontendBaseResult _UpdateFrontendBase(uint32_t sharedTextureSlot) noexcept;
	bool _OpenFrontendSharedTextures() noexcept;
	void _ResetDLSSFGSlotEvents() noexcept;
	void _CopySceneToTarget(ID3D11Texture2D* scene, ID3D11Texture2D* target,
		ID3D11RenderTargetView* rtv, POINT drawOffset) noexcept;
	void _RecordDLSSFGFrontendTimings(
		bool usesFrameLatencyWaitableObject,
		const PresentationJobTiming& timings,
		bool dropped
	) noexcept;

	void _BackendThreadProc() noexcept;
	bool _IsDLSSFGQueueFull() const noexcept {
		return _dlssFrameGenerator && _synchronousFramePresentationEnabled.load(std::memory_order_acquire) &&
			_pendingDLSSFGFrontendFrames.load(std::memory_order_acquire) >= _sharedTextureSlotCount;
	}

	HANDLE _InitBackend() noexcept;

	bool _InitFrameSource() noexcept;

	ID3D11Texture2D* _BuildEffects() noexcept;

	void _UpdateHdrEffectBoundaryContexts() noexcept;
	void _FailColorPipeline(std::string effect, ScalingError error) noexcept;
	HdrFrameMetadata _pipelineOutputMetadata{};
	HdrComponentPlan _runtimeHdrComponents;
	bool _colorPipelineFailed = false;

	void _UpdateActiveEffectDescs() noexcept;

	bool _ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept;

	bool _AppendBicubic(ID3D11Texture2D** inOutTexture) noexcept;

	ID3D11Texture2D* _ResizeEffects() noexcept;

	void _BuildEffectParameterRuntimeInfos() noexcept;
	void _ApplyPendingEffectParameters() noexcept;
	void _UpdateFrameRateLimits() noexcept;

	void _UpdateDestRect() noexcept;

	HANDLE _CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept;

	void _BackendRender(
		ID3D11Texture2D* effectsOutput,
		bool isNewCaptureFrame
	) noexcept;

	bool _PublishBackendTexture(
		ID3D11Texture2D* texture,
		bool synchronous,
		bool generatedFrame = false,
		uint64_t captureSequence = 0,
		uint64_t resourceGeneration = 0,
		uint64_t reflexPresentId = 0
	) noexcept;

	bool _InitializeDLSSFrameGenerator(
		ID3D11Texture2D* input,
		const struct DLSSFrameGenerationSettings& settings
	) noexcept;
	void _HandleDLSSFrameGenerationFailure(ID3D11Texture2D* input) noexcept;
	void _DisableDLSSFrameGenerationForSession() noexcept;
	bool _DrainNgxConsumers() noexcept;
	void _ReleaseNgxConsumers() noexcept;

	bool _UpdateDynamicConstants() const noexcept;


	winrt::IAsyncOperation<bool> _TakeScreenshotImpl(
		uint32_t effectIdx,
		uint32_t passIdx,
		uint32_t outputIdx
	) noexcept;

	static LRESULT CALLBACK _LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam);

	// 只能由前台线程访问
	DeviceResources _frontendResources;
	ReflexController _reflex;
	std::unique_ptr<PresenterBase> _presenter;
	
	CursorDrawer _cursorDrawer;
	OverlayDrawer _overlayDrawer;
	PassThroughFrames _passThroughFrames;
	bool _isPassThroughActive = false;

	// DLSSFG pair 相位节奏化需要发布环容纳整对 2M 帧（4x=8），否则前端的
	// hold 会经有界环反压后端发布，形成「等待→环阻塞→NGX 延迟→周期膨胀」
	// 的正反馈（历史实测 pair 67→151ms 发散）。1440p 下 +4 槽 ≈ +59MiB。
	static constexpr uint32_t MAX_SHARED_TEXTURE_SLOTS = 8;
	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedTextures;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedTextureMutexes;
	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedMotionTextures;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedMotionTextureMutexes;
	std::array<uint64_t, MAX_SHARED_TEXTURE_SLOTS> _lastAccessMutexKeys{};
	std::array<uint64_t, MAX_SHARED_TEXTURE_SLOTS> _discardedFrontendKeys{};
	std::array<std::mutex, MAX_SHARED_TEXTURE_SLOTS> _sharedTextureAccessMutexes;
	winrt::com_ptr<ID3D11Texture2D> _frontendBaseTexture;
	winrt::com_ptr<ID3D11Texture2D> _frontendPresentedBaseTexture;
	winrt::com_ptr<ID3D11Texture2D> _frontendMotionTexture;
	FrameGuidanceFrameId _frontendMotionFrameId = 0;
	bool _frontendMotionValid = false;
	bool _frontendMotionReset = true;
	bool _frontendBaseValid = false;
	bool _frontendPresentedBaseValid = false;
	bool _frontendBaseNeedsPresent = false;
	RECT _destRect{};
	
	const std::shared_ptr<ScalingSessionLifetime> _sessionLifetime;
	std::thread _backendThread;

	wil::unique_hhook _hKeyboardHook;
	
	// 只能由后台线程访问
	DeviceResources _backendResources;
	Magpie::BackendDescriptorStore _backendDescriptorStore;
	std::unique_ptr<FrameSourceBase> _frameSource;
	FrameGuidanceService _frameGuidanceService;
	bool _hasFrameGeneration = false;
	mutable PresentationFrameRate _presentationRate;
	FrameGuidanceFrameId _capturedFrameId = 0;
	std::chrono::steady_clock::time_point _lastCapturedFrameTime{};
	NgxD3D12Core _ngxD3D12Core;
	std::vector<EffectDrawer> _effectDrawers;
	std::vector<EffectOption> _runtimeEffectOptions;
	std::vector<uint64_t> _effectInputRevisions;
	struct PendingEffectParameterUpdate {
		uint32_t effectIdx = 0;
		uint32_t parameterIdx = 0;
		float value = 0.0f;
	};
	std::vector<PendingEffectParameterUpdate> _pendingEffectParameterUpdates;
	bool _forceNextRender = false;
	std::vector<std::unique_ptr<class NativeEffectBackend>> _nativeEffectBackends;
	std::unique_ptr<class DLSSFrameGenerator> _dlssFrameGenerator;
	uint32_t _dlssFgConsecutiveFailures = 0;
	uint32_t _dlssFgRecoveryAttempts = 0;

	StepTimer _stepTimer;
	// Frontend-owned before backend creation; backend-owned for the session.
	void _EnsureGpuPriority(bool force = false) noexcept;
	std::chrono::steady_clock::time_point _nextGpuPriorityCheck{};
	bool _gpuPriorityVerified = false;
	bool _gpuPriorityFailureLogged = false;
	EffectsProfiler _effectsProfiler;

	winrt::com_ptr<ID3D11Fence> _d3dFence;
	uint64_t _fenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;

	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedTextures;
	HdrSurfaceAdapter _hdrPresentationAdapter;
	winrt::com_ptr<ID3D11Texture2D> _hdrPresentationTexture;
	winrt::com_ptr<ID3D11Texture2D> _dlssFgNormalizedInput;
	winrt::com_ptr<ID3D11Texture2D> _dlssFgCanonicalGenerated;
	float _dlssFgHdrNormalizationScale = 1.0f;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedTextureMutexes;
	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedMotionTextures;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedMotionTextureMutexes;
	std::array<HANDLE, MAX_SHARED_TEXTURE_SLOTS> _sharedTextureHandles{};
	std::array<wil::unique_handle, MAX_SHARED_TEXTURE_SLOTS>
		_sharedMotionTextureHandles;
	std::array<wil::unique_handle, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureAvailableEvents;
	uint32_t _sharedTextureSlotCount = 1;
	uint32_t _nextBackendSharedTextureSlot = 0;

	winrt::com_ptr<ID3D11Buffer> _dynamicCB;


	// 可由所有线程访问
	std::array<std::atomic<uint64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureMutexKeys{};
	std::array<std::atomic<bool>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureContainsGeneratedFrame{};
	// DLSSFG pair 相位节奏化：每次发布随 slot 交换的发布时刻（steady ns）与
	// 奇偶性（后端 _PublishBackendTexture 写，前端 RenderDLSSFGFrame 读）。
	// -1 = 未知/无残差转移；0 = 偶（NGX）组；1 = 奇（转移）组。
	std::array<std::atomic<int64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedFramePublishNs{};
	std::array<std::atomic<int32_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedFrameParity{};
	std::array<std::atomic<FrameGuidanceFrameId>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedMotionFrameIds{};
	std::array<std::atomic<bool>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedMotionValid{};
	std::array<std::atomic<bool>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedMotionReset{};
	std::array<std::atomic<uint64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureCaptureSequences{};
	std::array<std::atomic<FrameGuidanceFrameId>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureFrameIds{};
	std::array<std::atomic<uint64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureResourceGenerations{};
	std::array<std::atomic<int64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureTimestamps{};
	std::array<HdrFrameMetadata, MAX_SHARED_TEXTURE_SLOTS> _sharedFrameMetadata{};
	HdrFrameMetadata _frontendFrameMetadata{};
	// Protected by the slot's existing publication/consumption mutex. These
	// IDs survive retries and stay paired with exactly the copied colour image.
	std::array<std::pair<uint64_t, uint64_t>, MAX_SHARED_TEXTURE_SLOTS> _sharedReflexIds{};
	std::pair<uint64_t, uint64_t> _frontendReflexIds{};
	HdrFrameMetadata _frontendPresentedFrameMetadata{};
	std::atomic<uint64_t> _activeCaptureSequence = 0;
	std::atomic<uint64_t> _activeResourceGeneration = 0;
	std::atomic<uint32_t> _latestSharedTextureSlot = 0;
	std::atomic<uint32_t> _sharedTextureGeneration = 0;
	std::atomic<bool> _synchronousFramePresentationEnabled = false;
	std::atomic<uint32_t> _pendingDLSSFGFrontendFrames = 0;
	float _frameRateFilterTarget = 0.0f;
	std::optional<float> _captureMaxFrameRate;
	// Backend-owned cadence; publish an immutable interval with each ring slot.
	CaptureFrameCadence _captureCadence;
	uint64_t _captureSequence = 0;
	// 残差转移（帧复用）的前端奇帧呈现延迟：后端发布奇偶标记+时间戳（原子），
	// 前端 Render 入口对「奇帧且距发布不足半周期」的 pending 短暂跳过（非阻塞，
	// 返回 false 交回消息循环），把「偶帧+奇帧 2ms 背靠背 + 80ms 空窗」的脉冲
	// 节奏变为半周期交替。等待发生在前端线程，后端 Draw/publish 全速不受影响
	//（区别于已证伪的后端 CPU pacing：串行线程里的等待=纯损耗）。
	// -1 = 未知/禁用；0 = 偶帧（立即呈现）；1 = 奇帧（延迟呈现候选）。
	std::atomic<int32_t> _reuseParityPublished = -1;
	std::atomic<int64_t> _reuseOddPublishNs = 0;
	// 奇帧消费周期估计（奇→奇间隔的 EMA）与上次奇帧到期时刻（纳秒 epoch）。
	// 组合模式（XeSSFG）下不再用于 hold（物理等待已删——串行后端里任何等待
	// 都阻塞 NGX/捕获,且自测会正反馈发散）;XeSSFGPresenter 的 frameRenderTime
	// 欺骗用它做半周期参考。
	std::atomic<int64_t> _reusePairIntervalNs = 0;
	std::atomic<int64_t> _reuseLastOddConsumedNs = 0;
	uint32_t _publicationTimingSamples = 0;
	double _publicationTransactionTotalMs = 0;
	double _publicationTransactionMaxMs = 0;
	double _publicationFenceTotalMs = 0;
	double _publicationFenceMaxMs = 0;
	// Backend-only; separate from the frontend's exchanged diagnostic counters.
	std::chrono::steady_clock::duration _captureCadenceQueueWait{};
	double _baseFrameRateLimit = 0;
	std::chrono::nanoseconds _synchronousPresentInterval{};
	std::array<std::atomic<int64_t>, MAX_SHARED_TEXTURE_SLOTS> _sharedPresentIntervalNs{};
	FramePresentationClock _presentationClock;
	FrameGuidanceFrameId _frontendCaptureFrameId = 0;
	FrameGuidanceFrameId _lastCountedRealFrameId = 0;
	uint32_t _dlssFgFrontendTimingFrames = 0;
	bool _dlssFgFrontendTimingModeInitialized = false;
	bool _dlssFgFrontendTimingUsesWaitableObject = false;
	std::chrono::nanoseconds _dlssFgFrontendPacingWait{};
	std::chrono::nanoseconds _dlssFgFrontendCapacityWait{}, _dlssFgFrontendResourceWait{};
	std::chrono::nanoseconds _dlssFgFrontendCpu{}, _dlssFgFrontendQueueAge{};
	uint32_t _dlssFgFrontendDropped = 0;
	std::chrono::nanoseconds _dlssFgFrontendBeginFrame{};
	std::chrono::nanoseconds _dlssFgFrontendDraw{};
	std::chrono::nanoseconds _dlssFgFrontendEndFrame{};
	// DLSSFG pair 相位节奏化（仅 RenderDLSSFGFrame 触碰，前端线程独占）：
	// 后端到达是脉冲式——残差转移下 NGX 半周期憋帧、奇帧快出；单用时每组
	// M 帧在几 ms 内突发发布（评估循环连续出帧）。固定间隔 deadline 对突发
	// 结构性失效（每次迟到重置基准 → paceWait 恒 0），脉冲原样穿透到显示。
	// 两种模式统一：以锚定组首帧到达（发布）时刻为锚点，把锚定周期内的帧
	// 均匀铺开——Pair（残差转移，parity 0/1）：偶（NGX）组锚定，一对 2M 帧
	// 铺到配对周期 P，槽距 P/(2M)；Uniform（单用，parity 恒 -1）：每组锚定，
	// M 帧铺到组间周期 T，槽距 T/M。早到 Retry 等槽（外层循环 1ms 唤醒，
	// 输入不被阻塞），晚到立即呈现。锚点与周期估计只用发布时刻（到达），
	// hold 不影响后端发布（发布环已扩到整对容量，无环反压）。
	std::chrono::steady_clock::time_point _dlssFgPairAnchor{};
	bool _dlssFgPairAnchorValid = false;
	double _dlssFgPairPeriodMs = 0.0;
	int64_t _dlssFgLastEvenPublishNs = 0;
	uint32_t _dlssFgPairFrameIndex = 0;
	bool _dlssFgGroupClosed = true;
	uint32_t _dlssFgSeenRingGeneration = 0;
	bool _dlssFgPairModeActive = false;
	uint32_t _dlssFgPairPacedFrames = 0;
	// 节奏化决策诊断（_RecordDLSSFGFrontendTimings 每 120 帧汇总清零）：
	// 分类在 due 计算块（幂等），计数在成功呈现后——一次测试即可定位
	// pairPaced=0 时卡在哪个分支。
	uint32_t _dlssFgDiagAnchorFrame = 0;	// 锚定组首（按设计立即呈现）
	uint32_t _dlssFgDiagPaced = 0;			// 早到被 hold 到槽位
	uint32_t _dlssFgDiagLate = 0;			// due 已过（到达晚于槽位）
	uint32_t _dlssFgDiagNoAnchor = 0;		// 锚点无效（首个周期前）
	uint32_t _dlssFgDiagNoPeriod = 0;		// 周期 EMA 未估出（第二个锚定组前）
	uint32_t _dlssFgDiagAnchorCommits = 0;	// 锚定组提交数
	uint32_t _dlssFgDiagGroups = 0;			// 组关闭数（=真实帧呈现数）
	uint32_t _dlssFgDiagParity[3] = {};		// parity[-1/0/1] 直方图
	std::atomic<uint64_t> _dlssFgRingWaitNanoseconds = 0;
	std::atomic<uint64_t> _dlssFgRingWaitSamples = 0;
	// DLSSFG 实际生效倍率（后端 init 后写，前端读；请求倍率可能被 SDK 上限钳制）。
	std::atomic<uint32_t> _dlssFgActiveMultiplier = 1;
	std::chrono::steady_clock::time_point _dlssFgDiagnosticsStart{};
	uint32_t _dlssFgCapturedFrameCount = 0;
	uint32_t _dlssFgPresentedFrameCount = 0;
	uint32_t _dlssFgGeneratedPublishSuccess = 0;
	uint32_t _dlssFgGeneratedPublishFailure = 0;
	uint32_t _dlssFgRealPublishSuccess = 0;
	uint32_t _dlssFgRealPublishFailure = 0;
	// 本帧奇偶（后端线程独占；_CompleteBackendFrame 帧首计算，
	// _PublishBackendTexture 随 slot 写出，供前端 pair 相位节奏化）。
	int32_t _dlssFgPendingParity = -1;
	bool _dlssFgPresentationStopping = false;
	bool _isXeSSFrameGenerationActive = false;
	FrameGenerationEffectKind _xessFrameGenerationKind =
		FrameGenerationEffectKind::None;
	MotionVectorRequest _xessMotionRequest{};

	// INVALID_HANDLE_VALUE 表示后端初始化失败
	std::atomic<HANDLE> _sharedTextureHandle{ NULL };
	// 下面四个成员由 _sharedTextureHandle 同步
	winrt::Windows::System::DispatcherQueue _backendThreadDispatcher{ nullptr };
	ScalingError _backendInitError = ScalingError::NoError;
	std::string _backendInitContext;
	uint32_t _backendInitSystemError = 0;
	std::vector<std::pair<std::string, MotionVectorRequest>> _motionConsumers;
	std::wstring _motionConfigurationNotice;
	std::vector<EffectDesc> _effectDescs;
	bool _dlssnrAutoHdr = false;
	std::string _dlssnrHdrDiagnostic;
	// 包含追加的 Bicubic
	std::vector<const EffectDesc*> _activeEffectDescs;
	std::vector<std::vector<EffectParameterRuntimeInfo>>
		_effectParameterRuntimeInfos;

	std::mutex _effectParameterMailboxMutex;
	std::unordered_map<uint64_t, PendingEffectParameterUpdate>
		_effectParameterMailbox;
	bool _effectParameterWakeQueued = false;
};

}
