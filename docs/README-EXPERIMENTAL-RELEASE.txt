Magpie Experimental v0.6.9 x64

安装或升级
完全退出托盘中的 Magpie，将 Magpie-Experimental-x64.zip 完整解压到新目录，再运行其中的 Magpie.exe。
普通配置优先读取 %LOCALAPPDATA%\Magpie\config\v4e\config.json；没有 v4e 时自动导入 v4，后续只保存到 v4e。
便携用户可将原配置复制到新程序目录内的 config\v4e\config.json，保留原文件用于回退。

0.6.9 使用要点
效果组右上角“其他选项”提供导入、导出、配置文件夹和重置。重置仍需确认。
默认配置与应用配置统一使用“帧率与刷新”；内容、光标、空闲三项手动值为 15–360 FPS、整数、步进 1。有效旧设置自动迁移。
新配置默认内容自定义 60 FPS、Front Edge、原始帧优先并补充、光标最低 60 FPS、空闲重绘启用 30 FPS。修改自动保存，重启缩放后生效。
按住工具栏空白或 FPS 区域拖动，可上下停靠；不需要专用拖动按钮。
DLSSNR 默认 Multi Pass 1、抗闪烁无，支持 1/2/3 次处理。统一细节控制作用于全部 Pass 后的总残差；进阶调整默认隐藏，收起不重置参数。
FG 重复帧过滤默认开启，可在倍率与光流方法之间关闭；全局重复检测位于“帧率与刷新”的高级选项。
旧 XeSS x2/MFG 迁移为 XeSS_FrameGeneration，倍率设为 2，保留光流方法与质量。已有统一效果的倍率不重置。
效果组出现失效项时，恢复对应效果文件，或移除此项并添加替代效果。旧组重名时用铅笔按钮修改名称，效果内容保留。

完整更新、安装与限制见 RELEASE-NOTES.md，帧率说明见 FRAME_SYNC_GUIDE.md。
请保留许可证、THIRD-PARTY-NOTICES.md 和 build-manifest.json。
可选附件沿用 0.6.8：DLSSNR-DLL-Options-310.8.0.0.zip、NGX_OTA_Switch.bat。普通安装无需运行 OTA 工具。

English

Install or upgrade
Fully exit Magpie from the system tray, extract Magpie-Experimental-x64.zip completely into a new folder, then run its Magpie.exe.
Normal settings prefer %LOCALAPPDATA%\Magpie\config\v4e\config.json; if v4e is absent, v4 is imported automatically and subsequent saves use only v4e.
Portable users can copy their existing settings to config\v4e\config.json in the new program folder and retain the original for rollback.

0.6.9 essentials
Other options in the effect-group header provides Import, Export, Configuration folder and Reset. Reset still requires confirmation.
Default/application profiles share Frame rate and refresh. Manual content, cursor and idle values use 15–360 FPS, integers and 1-FPS steps. Valid legacy settings migrate automatically.
New defaults: Custom content 60 FPS, Front Edge, original frames with supplementation, cursor minimum 60 FPS, idle redraw enabled at 30 FPS. Edits save automatically and apply after restarting scaling.
Drag blank or FPS areas of the toolbar to dock at the top or bottom; no dedicated drag button is needed.
DLSSNR defaults to Multi Pass 1 and Anti-flicker None, with 1/2/3 passes available. Unified detail controls affect the total residual after all passes. Advanced starts hidden; collapsing it retains parameters.
FG duplicate filtering defaults to On and can be disabled between multiplier and optical-flow method. Global duplicate detection is in the advanced Frame rate and refresh settings.
Legacy XeSS x2/MFG migrates to XeSS_FrameGeneration at 2x, retaining optical-flow method and quality. Existing unified effects keep their multiplier.
For an invalid effect, restore its file or remove it and add a replacement. Rename duplicate legacy groups with the pencil button; their effects are retained.

See RELEASE-NOTES.md for complete updates, installation and limitations, and FRAME_SYNC_GUIDE.md for frame-rate settings.
Retain the licenses, THIRD-PARTY-NOTICES.md and build-manifest.json.
Optional assets reuse the 0.6.8 DLSSNR-DLL-Options-310.8.0.0.zip and NGX_OTA_Switch.bat. Normal installation does not require the OTA tool.
