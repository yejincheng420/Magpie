# Magpie 实验分支文档索引

当前公开版本为 [Magpie Experimental v0.6.9](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.9-experimental)：[完整中英 Release Note](../RELEASE_NOTES_v0.6.9-experimental.md)、[分发准备记录](reviews/20261002-v0.6.9-release-preparation.md)。默认分支 `experimental` 已同步完整 0.6.9 源码，`version.json` 指向该版本。发布标签及运行包的准确构建提交为 `27c5df91177a29b33be612e98274169f3d2fca49`；后续文档与版本入口更新保留为独立提交。

帧同步当前状态：保留 Front Edge 默认，普通效果与 DLSS FG 支持 Async／Reflex 驱动基础限帧，XeSS FG 由 XeLL 接管；自动回退仅记简短日志，实时参数面板不显示运行状态。见 [使用指南](../FRAME_SYNC_GUIDE.md) 与 [实施记录](reviews/20260909-v0.6.7-frame-sync-modes.md)。

HDR 当前状态：旧配置页入口保持隐藏，新旧配置中的旧开关保持关闭；效果器选择器新增「HDR 组件」分类，仅显式转换链按节点启用 HDR 捕获／输出。见 [HDR 组件实施记录](reviews/20260908-v0.6.7-hdr-conversion-effects-plan.md)。

历史版本 [Magpie Experimental v0.6.8](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.8-experimental.1) 的[更新说明](../RELEASE_NOTES_v0.6.8-experimental.md)与[分发准备记录](reviews/20260914-v0.6.8-release-preparation.md)保留供查阅。下方 Beta、TODO 与部署状态均为当时的历史记录，具体使用说明以当前版本 Release Note 为准。

当前功能已包含 Beta 1–6 的全部迭代，见 [Beta 6 TODO](todos/20260910-v0.6.7-beta6-TODO.md)、[Reflex 驱动限帧](reviews/20260910-beta6-reflex-pacing.md)、[HDR 组件](reviews/20260908-v0.6.7-hdr-conversion-effects-plan.md) 与 [参数输入验证](testing/PARAMETER-INPUT.md)。历史 [Beta 6 说明](../RELEASE_NOTES_v0.6.7-beta6.md)、[Beta 1 说明](../RELEASE_NOTES_v0.6.7-beta1.md)、[0.6.6 说明](../RELEASE_NOTES_v0.6.6-experimental.md) 保留。

- [0.6.6 参数交互与本地验证](reviews/20260906-v0.6.6-parameter-interaction.md)
- [0.6.6 停止缩放后的过期回调保护](reviews/20260906-v0.6.6-capture-shutdown.md)
- [0.6.6 NGX 异常后的死锁保护](reviews/20260906-v0.6.6-ngx-deadlock.md)

- [r10 DLSSNR 参数延迟重新启用](reviews/20260905-r10-dlssnr-parameter-restart.md)
- [Front Edge Sync 使用说明](../FRAME_SYNC_GUIDE.md)


本目录只收纳实验功能的路线图、TODO、测试矩阵和运行记录。上游通用 Wiki 文档继续保留在 `docs/` 根目录，避免实验说明与用户文档混在一起。

## 当前工作

- [069 “其他选项”按钮样式协调 TODO](todos/20261002-v0.6.9-effect-options-button-style-TODO.md)：已继承普通按钮的 14 号字与默认留白，保留 36 DIP 高度、纯文字、中性填充和零边框。144 组原生页头布局及页脚、容器和配置目录检查通过；单线程完整 Release 与包／编译资源校验通过。按用户明确授权关闭 Magpie 后，已归档旧完整目录、保留最新配置并替换至原 Beta3 路径，替换后的实际文件校验通过；Magpie 保持关闭，实际界面验收待完成。[实施与部署记录](reviews/20261002-v0.6.9-effect-options-button-style.md)。

- [069 效果组其他选项菜单与效果器说明 Review TODO](todos/20261002-v0.6.9-effect-options-and-description-review-TODO.md)：菜单源码与全部 162 项双语说明审查完成，入口沿用旧版 36 DIP 高度及当前普通按钮外观；目录生成、144 组原生页头布局、选择器模型与页面语法检查通过。已按单线程、低优先级和单逻辑核心完成 Beta3 构建部署及包／编译资源校验，游戏期间未启动应用；实际主窗口操作、完整 DPI 和安装后选择器验收仍待完成。[实施与验证](reviews/20261002-v0.6.9-effect-options-and-description-review.md)、[Beta3 部署记录](reviews/20261002-v0.6.9-beta3-deployment.md)。

- **Beta2 按钮与刷新控件样式对齐**：四个操作采用效果组／效果器的普通中性填充、零边框；六个下拉框与三组数值统一为 225 DIP，提供行内步进，并统一窄窗口换行与条件提示。完整 Release、原生标题／刷新控件测量及模型回归通过，已部署到原 Beta2；主窗口启动与三个现有配置保留检查通过。[TODO 与验证范围](todos/20261002-v0.6.9-beta2-ui-style-alignment-TODO.md)、[实施与部署记录](reviews/20261002-v0.6.9-beta2-ui-style-alignment.md)。

- **Beta2 刷新设置布局与文案**：重复帧检测移入“帧率与刷新 → 高级选项”，沿用全局设置；刷新设置说明改为简短陈述，移除重复保存提示。[修改与部署记录](reviews/20261002-v0.6.9-beta2-refresh-layout.md)。

- **Beta2 整数帧率修正**：实时面板与配置页的内容、光标、空闲三项均为 15–360 FPS、整数、普通步进 1；保留自动计算及旧配置浮点兼容。生产滑块 2,076 次编辑、统一刷新和工具栏回归通过，继续部署到原 Beta2。[修复与恢复记录](reviews/20261002-v0.6.9-beta2-integer-rates.md)。

- **0.6.9 Beta2 已部署**：全部 11 个功能分支已核对合入主线，包含 Beta1 fix1–3；完整 Release x64、19 组回归、45 组真实 NGX 合成场景、xBR 编译、资源／包校验和主窗口启动通过。保留旧配置语义及原 Toolbar 热键占用；实际游戏、Sunshine／外接显卡及完整 FG/HDR／多显示器验收待实测。[累计中英更新清单](../RELEASE_NOTES_v0.6.9-beta2.md)、[整合与部署记录](reviews/20261002-v0.6.9-beta2-integration.md)。

- [069 帧率、空闲重绘与光标刷新统一 TODO](todos/20261002-v0.6.9-unified-frame-refresh-TODO.md)：源码实现及离线验证完成，内容／光标／空闲 60／60／30 默认值和统一配置入口已接入；节奏方式位于内容选择与基础数值之间。保留旧配置语义，光标呈现独立于内容时钟；产品编译部署、原生 UI 与真实 GPU 验收交给 069 beta2。[实施记录](reviews/20261002-v0.6.9-unified-frame-refresh.md)。

- [069 效果组页面 TODO](todos/20261002-v0.6.9-effect-groups-page-TODO.md)：已在独立功能分支实现并合并回本地 experimental；标题右侧四个紧凑文字操作、新建按钮恢复及配置目录动作已实现，144 组标题、40 组新建、64 组隐藏容器、4 组目录处理回归和页面语法检查通过；完整编译部署及实际主窗口验收交给 069 beta2 会话。

- [069 PR #62 排除仅显示的间接适配器合并 TODO](todos/20261002-v0.6.9-pr62-display-only-adapters-TODO.md)：已在独立修复分支接入并合并回本地 experimental，保留原 PR 作者及历史；失败日志、51 项生产代码回归、3 个改动翻译单元语法检查和真实适配器查询通过。整项目构建、部署及 Sunshine DLSS NR／FG 实机验收交由 069 beta2 会话。关联 Issue #61。

- **0.6.9 Beta1 fix3 已部署**：消除工具栏 FPS 继承按钮基线导致的垂直下偏；128 组布局与既有交互回归、完整编译、包核验和主窗口启动检查通过。实际捕获视觉验收仍待实测；[更新说明](../RELEASE_NOTES_v0.6.9-beta1-fix3.md)、[实施与恢复记录](reviews/20261001-v0.6.9-beta1-fix3.md)。

- **0.6.9 Beta1 fix2 已部署**：[TODO](todos/20261001-v0.6.9-beta1-fix2-TODO.md)、[实施与验证](reviews/20261001-v0.6.9-beta1-fix2.md)、[参数文案 Review](reviews/20261001-v0.6.9-beta1-fix1-dlssnr-parameter-copy-review.md)。原生主窗口与配置迁移通过；完整 GUI／实际捕获验收待实测，保留两项快捷键注册冲突。

- [0.6.9 Beta1 fix1 TODO](todos/20261001-v0.6.9-beta1-fix1-TODO.md)：参数浮层、DLSSNR 进阶／名称提示与数值迁移、工具栏样式及居中吸附已部署到 Beta1。21 项完成，完整原生 GUI 与实际内容验收仍待完成；[实施与部署记录](reviews/20261001-v0.6.9-beta1-fix1.md)记录版本、包哈希、真实启动、三语提示、45 组 GPU 合成验证和恢复路径。

- [069 DLSSNR 细节控制 TODO](todos/20261001-v0.6.9-dlssnr-detail-control-TODO.md)：基础、保护／压缩、频率与色度时域控制已实施，旧模式兼容规则已由 Beta1 fix1 的统一迁移替代；[原始实施及验证记录](reviews/20261001-v0.6.9-dlssnr-detail-control.md)保留生产 shader 响应图与当时证据。

- [069 快捷键清除与“未设置”状态 TODO](todos/20261001-v0.6.9-shortcut-clear-TODO.md)：行内垃圾桶、弹窗清除、显式空绑定持久化与正常注销已实现，自动回归通过；真实界面验收和 069 beta1 编译部署交由独立会话。关联 Issue #34。
- [069 工具栏拖拽与上下停靠 TODO](todos/20261001-v0.6.9-toolbar-drag-TODO.md)：专用拖拽柄、上下蓝条、按应用 / 模式保存上下位置和会话期左右位置已实现，自动回归通过；真实界面 / GPU 验收与 069 beta1 编译部署交由独立会话。关联 Issue #50。
- [069 重复帧过滤优化 TODO](todos/20261001-v0.6.9-duplicate-frame-filter-optimization-TODO.md)：效果输出版本、旧帧复用、时序历史重置及 HDR 去重前置已实施，CPU / WARP 回归与源码语法检查通过；同步读回和 Dynamic 策略保留，069 beta1 产品编译部署与真实 SDK / 画质 / 性能验收交由独立会话。
- [069 光标刷新与主页选项 TODO](todos/20261001-v0.6.9-cursor-refresh-and-home-options-TODO.md)：应用配置光标策略、默认开启的 60 FPS 最低刷新、主页过滤及开发者入口已实施，386 项离线检查、相关回归和源码语法检查通过；[实施及 beta1 交接](reviews/20261001-v0.6.9-cursor-refresh-and-home-options.md)，产品编译部署与真实 UI / SDK / 性能验收交由独立会话。
- [069 DLSSNR 重复资源开销优化 To do](todos/20261001-v0.6.9-dlssnr-overhead-optimization-TODO.md)：入口共用缩放 / 引导、出口统一总残差、D3D12 集中调度与缓存优化已实施；自动回归及有限真实 NGX 冒烟通过，069 beta1 产品编译部署与完整画质 / 性能验收交由独立会话。
- [069 To do - xBR](todos/20260921-v0.6.9-xbr-TODO.md)：筛选标准 LV2 固定倍率、NoBlend 与 Hybrid 为首批范围；LV3、MLV4 和 Super-xBR 需经过画质/性能门槛后再决定是否进入 0.6.9。
- [DLSSNR Multi Pass 残差时域稳定路线](design/20260914-dlssnr-temporal-stabilization-routes.md)：比较无光流／光流累积、快慢历史、稳健统计、分频与联合升采样，说明当前 068 串联实现的接入边界和验证顺序。
- [v0.6.5 r8：参数状态、光流降级与 SR 入口收敛](todos/20260905-v0.6.5-r8-TODO.md)。
- [r8 本地版本说明](../RELEASE_NOTES_v0.6.5-r8-local.md)。

下列旧版条目保留历史；与 r8 记录不一致的方案已被取代。

- [v0.6.5 r4：统一光流方法与运动向量质量下拉菜单](todos/20260904-v0.6.5-r4-optical-flow-selection-TODO.md)：为 DLSS SR/FG/NR 定义 NVOF 4F/4M/4S/2M 四档，为 XeSSFG 增加 None/AMDOF/NVOF 方法选择、提供器专属质量档、能力错误与多消费者共享合同。
- [v0.6.5 r3 re fix2：恢复工具栏式的同帧浮窗呈现](todos/20260904-v0.6.5-r3-re-fix2-TODO.md)：移除提前生成 DrawData 和浮窗计时门控，让工具栏、参数窗与光标在实际前端帧内生成、绘制并统一 Present。
- [v0.6.5 r3 re fix1：浮窗与光标统一刷新调度](todos/20260904-v0.6.5-r3-re-fix1-TODO.md)：输入边沿即时推进，高频移动合并到正常前端 tick，并在交互期间使用约 8 ms 的按需刷新。
- [v0.6.5 r3：缩放期间调节效果参数](todos/20260904-v0.6.5-r3-TODO.md)：参数级区分实时应用与重新缩放后生效，并提供保存、还原和一次性完整重启流程。
- [v0.6.5 r2 fix4：参数列间距](todos/20260904-v0.6.5-r2-fix4-TODO.md)：将参数列间距增至 24 DIP，并保持分隔线居中。
- [v0.6.5 r2 fix3：参数列分隔线](todos/20260904-v0.6.5-r2-fix3-TODO.md)：在相邻可见参数列之间增加淡色细竖线，并随列显隐更新。
- [v0.6.5 r2 fix2：Residual 命名与 NR Style 换行](todos/20260904-v0.6.5-r2-fix2-TODO.md)：补齐两个 HSL Multiplier 名称，并让显式多行数值标签的说明行使用整列宽度。
- [v0.6.5 r2 fix1：参数列、残差语义与 DLSS 参数收敛](todos/20260904-v0.6.5-r2-fix1-TODO.md)：修正 Flyout 宽度、残差处理顺序、长标签、NR 范围与 Motion 开关。
- [v0.6.5 r2：参数列直展与 DLSSNR 残差精细控制](todos/20260904-v0.6.5-r2-TODO.md)：参数 Flyout 直接展开所有列，重排 DLSSNR 两列并增加 HSL 与有符号残差控制。
- [v0.6.5 r1：移除深度估算与效果参数多列分组](todos/20260904-v0.6.5-r1-TODO.md)：保留 Zero Depth 合同，移除学习型深度链，并为效果参数增加可选 GROUP 多列布局。
- [v0.6.1 Feature 4：缩放模式交互修复与引导性错误提示](todos/20260903-v0.6.1-feature4-interaction-errors-TODO.md)：修复拖拽闪退、重复新建不弹自动命名，并系统整理可恢复的失败提示与后续错误码拆分。
- [Frame Guidance 测试矩阵](testing/FRAME_GUIDANCE-TEST-MATRIX.md)：统一记录 Zero/Motion 两态、场景、性能和日志证据。

## 已完成的路线图

- [DLSS5 Frame Guidance 阶段 1–4](todos/completed/20260829-164413-DLSS5-FrameGuidance-short-term-TODO.md)：工程实现已完成，GPU 运行时验收转入当前 TODO。

## 已被 v0.6.5 取代

- [DLSS 组合兼容性短期 TODO](todos/20260830-DLSS-combination-compatibility-short-term-TODO.md)
- [DLSSNR 参数与性能短期 TODO](todos/20260829-DLSSNR-parameters-performance-short-term-TODO.md)
- [DLSSNR 性能与可观测性 TODO](todos/20260829-DLSSNR-performance-observability-TODO.md)

## 交接、发布与授权

- [实验分支 Git 工作流](GIT-WORKFLOW.md)
- [实验版发布规范](RELEASE-WORKFLOW.md)：版本与标签、双语 Release Note、附件布局、Draft 审核门禁及发布检查。
- [实验分支交接](../EXPERIMENTAL_HANDOFF_ZH.md)
- [v0.6.1 Hotfix 说明](../RELEASE_NOTES_v0.6.1-experimental-hotfix.md)
- [v0.6.1 已发布实验版说明](../RELEASE_NOTES_v0.6.1-experimental.md)
- [下一实验版计划](../RELEASE_NOTES_NEXT.md)
- [v0.6.0 Hotfix 说明](../RELEASE_NOTES_v0.6.0-experimental-hotfix.md)
- [v0.6.0 实验版说明](../RELEASE_NOTES_v0.6.0-experimental.md)
- [v0.5.9 未发布历史草稿](../RELEASE_NOTES_v0.5.9-experimental.md)
- [v0.5.8 实验版说明](../RELEASE_NOTES_v0.5.8-experimental.md)
- [v0.5.7 实验版说明](../RELEASE_NOTES_v0.5.7-experimental.md)
- [v0.5.6 实验版说明](../RELEASE_NOTES_v0.5.6-experimental.md)
- [v0.5.3 实验版说明](../RELEASE_NOTES_v0.5.3-experimental.md)
- [v0.5.2 实验版说明](../RELEASE_NOTES_v0.5.2-experimental.md)
- [实验包 README](../README-EXPERIMENTAL-RELEASE.txt)
- [第三方组件与再分发](../THIRD_PARTY_AND_REDISTRIBUTION.md)

## 目录约定

- `todos/`：按日期命名的执行清单；完成的阶段性清单归档到 `todos/completed/`。
- `testing/`：可复用测试矩阵、素材约定、截图/DDS/日志记录格式。
- 后续若增加设计说明，放入 `design/`；若增加一次性测试结果，放入 `results/YYYYMMDD/`。
