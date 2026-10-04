#include "EffectFrameState.h"
#include "DLSSNRTemporalState.h"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace Magpie;
static void Check(bool value, const char* message) {
	if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
	EffectFrameState shader, nr;
	EffectFrameKey key{ .frameId = 1, .inputRevision = 1, .captureSequence = 1,
		.resourceGeneration = 1, .input = { 1, 1920, 1080, 28 }, .output = { 2, 1920, 1080, 28 } };
	Check(shader.NeedsDraw(key, false) && shader.RequiresHistoryReset(key), "first output was reused");
	shader.Commit(key, true);
	const auto firstVersion = shader.OutputRevision();
	const auto firstHistory = shader.OutputHistoryRevision();
	Check(!shader.NeedsDraw(key, false), "minimum-FPS/UI redraw advanced image work");
	Check(shader.OutputRevision() == firstVersion, "reuse advanced output version");
	Check(shader.NeedsDraw(key, true), "time-animation shader was frozen");
	EffectFrameKey downstream = key;
	downstream.input = key.output; downstream.output.texture = 3;
	downstream.inputRevision = shader.OutputRevision();
	downstream.inputHistoryRevision = shader.OutputHistoryRevision();
	nr.Commit(downstream, true);
	Check(!nr.NeedsDraw(downstream, false), "unchanged NR input was not reusable");
	// An actual old-frame dynamic draw changes downstream content even when
	// no parameter and no capture ID changed (the original SMAA -> NR defect).
	shader.Commit(key, true);
	downstream.inputRevision = shader.OutputRevision();
	Check(nr.NeedsDraw(downstream, false) && nr.RequiresHistoryReset(downstream),
		"same-frame upstream output change was hidden by NR cache");
	nr.Commit(downstream, true);
	// Ordinary new frames change content versions without resetting history.
	++key.frameId; ++key.inputRevision;
	Check(shader.NeedsDraw(key, false) && !shader.RequiresHistoryReset(key), "new capture discarded normal temporal history");
	shader.Commit(key, true);
	Check(shader.OutputHistoryRevision() == firstHistory, "ordinary capture advanced history epoch");
	++downstream.frameId;
	downstream.inputRevision = shader.OutputRevision();
	Check(!nr.RequiresHistoryReset(downstream), "NR reset for every new content version");
	nr.Commit(downstream, true);
	// Parameter change is local until its completed output reaches downstream.
	shader.ParametersChanged(); key.parameterRevision = shader.ParameterRevision();
	Check(shader.NeedsDraw(key, false), "live parameter edit was hidden by cache");
	shader.Commit(key, true);
	downstream.inputRevision = shader.OutputRevision();
	downstream.inputHistoryRevision = shader.OutputHistoryRevision();
	Check(nr.RequiresHistoryReset(downstream), "upstream parameter history change was lost");
	nr.Commit(downstream, true);
	// A parameter edit arriving with a new capture still invalidates history.
	shader.ParametersChanged(); key.parameterRevision = shader.ParameterRevision();
	++key.frameId; ++key.inputRevision; shader.Commit(key, true);
	++downstream.frameId;
	downstream.inputRevision = shader.OutputRevision();
	downstream.inputHistoryRevision = shader.OutputHistoryRevision();
	Check(nr.RequiresHistoryReset(downstream), "new-capture parameter edit lost its reset");
	// Each part of the resource/semantic contract must invalidate reuse.
	std::array<EffectFrameKey, 8> changed{ key,key,key,key,key,key,key,key };
	++changed[0].captureSequence; ++changed[1].resourceGeneration;
	++changed[2].input.texture; ++changed[3].output.texture;
	++changed[4].input.width; ++changed[5].output.height;
	++changed[6].input.format; ++changed[7].inputHistoryRevision;
	for (const auto& candidate : changed) {
		Check(shader.NeedsDraw(candidate, false) && shader.RequiresHistoryReset(candidate), "resource or color reset reused stale output");
	}
	shader.Commit(key, false);
	Check(shader.NeedsDraw(key, false) && shader.RequiresHistoryReset(key), "failed SDK/fallback output became a valid cache");
	shader.Commit(key, true); shader.Invalidate();
	Check(shader.NeedsDraw(key, false), "resize/export/rebuild did not invalidate output");
	for (auto name : { "SMAA\\SMAA_T2x_Experimental", "SMAA\\SMAA_T2x_NoJitter_Experimental",
		"SMAA\\SMAA_4x_Experimental", "SMAA\\SMAA_4x_NoJitter_Experimental" })
		Check(UsesCaptureFrameClock(name), "a temporal SMAA variant retained the render clock");
	Check(!UsesCaptureFrameClock("Custom\\Animated") && !UsesCaptureFrameClock("SMAA\\SMAA_1x"), "unrelated dynamic effects were frozen");
	DLSSNRTemporalState temporal;
	temporal.Commit(1, 1, 1, 1'000'000);
	Check(temporal.Weight(2, 1, 1, 1'166'667, false) > 0, "history epoch blocked normal EMA accumulation");
	Check(temporal.Weight(2, 2, 1, 1'166'667, false) == 0, "history epoch change failed to reset EMA");
	Check(temporal.Weight(1, 1, 1, 1'166'667, false) == 0, "same-frame re-evaluation advanced EMA time");
	std::cout << "Effect frame state: capture/reuse/dynamic/parameter/resource/failure/history regressions passed.\n";
}
