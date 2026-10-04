"""Extract production view-model mode/number/visibility/default decisions."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
source = (repo / "src/Magpie/ProfileViewModel.cpp").read_text(encoding="utf-8-sig")
names = ("ContentFrameRateModeIndex", "ContentFrameRate", "FrameSyncModeIndex", "IsContentPacingEnabled",
         "ShowContentFrameRate", "CursorRefreshModeIndex", "CursorSupplementModeIndex", "CursorSupplementRate",
         "ShowCursorSupplement", "ShowCursorSupplementRate", "IdleRedrawModeIndex", "IdleRedrawRate",
         "ShowIdleRedrawRate", "ResetFrameRefresh", "_SaveFrameRefresh")
declarations, definitions = [], []
for name in names:
    for match in re.finditer(r"^(?:bool|double|int32_t|void) ProfileViewModel::" + name + r"\([^\n]+?\{", source, re.M):
        signature = match[0][:-1].strip()
        declarations.append(signature.replace("ProfileViewModel::", "") + ";")
        start, end, depth = match.start(), match.end(), 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        definitions.append(source[start:end])
prefix = r'''
#include "FrameRefreshSettings.h"
#include <cassert>
#include <iostream>
#include <limits>
namespace Magpie {
struct Profile { FrameRefreshSettings frameRefresh; };
struct ProfileService {
    struct Changed { unsigned count=0; void Invoke(Profile&) { ++count; } } FrameRefreshChanged;
    static ProfileService& Get() { static ProfileService s; return s; }
};
struct AppSettings { unsigned saves=0; void SaveAsync() { ++saves; }
    static AppSettings& Get() { static AppSettings s; return s; }
};
struct ProfileViewModel {
    Profile* _data;
'''
suffix = r'''
}
int main() {
    using namespace Magpie;
    Profile defaults, app;
    ProfileViewModel vm{&defaults};
    vm.ContentFrameRate(37); vm.CursorSupplementRate(144); vm.IdleRedrawRate(17);
    for (int content=0; content<3; ++content) for (int pacing=0; pacing<3; ++pacing) for (int cursor=0; cursor<3; ++cursor)
    for (int supplement=0; supplement<2; ++supplement) for (int idle=0; idle<2; ++idle) {
        vm.ContentFrameRateModeIndex(content); vm.FrameSyncModeIndex(pacing); vm.CursorRefreshModeIndex(cursor);
        vm.CursorSupplementModeIndex(supplement); vm.IdleRedrawModeIndex(idle);
        assert(vm.ContentFrameRate()==37 && vm.CursorSupplementRate()==144 && vm.IdleRedrawRate()==17);
        assert(vm.ShowContentFrameRate()==(content==2) && vm.IsContentPacingEnabled()==(content!=0));
        assert(vm.ShowCursorSupplement()==(cursor==2) && vm.ShowCursorSupplementRate()==(cursor==2 && supplement==1));
        assert(vm.ShowIdleRedrawRate()==bool(idle));
        assert(app.frameRefresh==FrameRefreshSettings{});
    }
    auto saved=defaults.frameRefresh; const auto previousSaves=AppSettings::Get().saves;
    vm.ContentFrameRateModeIndex(-1); vm.CursorRefreshModeIndex(3); vm.CursorSupplementModeIndex(2);
    vm.IdleRedrawModeIndex(2); vm.FrameSyncModeIndex(9);
    assert(defaults.frameRefresh==saved && AppSettings::Get().saves==previousSaves);
    defaults.frameRefresh.legacyContentLimit=20; vm.ContentFrameRate(38);
    assert(defaults.frameRefresh.legacyContentLimit==0);
    defaults.frameRefresh.legacyResponsiveMinimum=true; vm.CursorRefreshModeIndex(0);
    assert(!defaults.frameRefresh.legacyResponsiveMinimum);
    vm.ContentFrameRate(std::numeric_limits<double>::infinity());
    vm.CursorSupplementRate(std::numeric_limits<double>::quiet_NaN()); vm.IdleRedrawRate(0);
    assert(vm.ContentFrameRate()==60 && vm.CursorSupplementRate()==60 && vm.IdleRedrawRate()==30);
    // Explicit edits quantize, without quantizing saved legacy values on read.
    vm.ContentFrameRate(59.6); vm.CursorSupplementRate(143.4); vm.IdleRedrawRate(29.5);
    assert(vm.ContentFrameRate()==60 && vm.CursorSupplementRate()==143 && vm.IdleRedrawRate()==30);
    vm.ContentFrameRate(1); vm.CursorSupplementRate(1000); vm.IdleRedrawRate(10.4);
    assert(vm.ContentFrameRate()==15 && vm.CursorSupplementRate()==360 && vm.IdleRedrawRate()==15);
    defaults.frameRefresh.contentRate=59.94f;
    assert(vm.ContentFrameRate()==double(59.94f));
    vm.FrameSyncModeIndex(2); vm.ContentFrameRateModeIndex(0);
    assert(vm.FrameSyncModeIndex()==2 && !vm.IsContentPacingEnabled());
    vm.ResetFrameRefresh();
    assert(defaults.frameRefresh==FrameRefreshSettings{} && app.frameRefresh==FrameRefreshSettings{});
    assert(AppSettings::Get().saves==ProfileService::Get().FrameRefreshChanged.count);
    std::cout << "Production refresh view model: 108 mode combinations, hidden numeric retention, invalid edits, compatibility release, profile isolation and reset passed\n";
}
'''
(output / "frame_refresh_vm.cpp").write_text(prefix + "\n".join(declarations) + "\n};\n" +
                                             "\n".join(definitions) + suffix, encoding="utf-8")
