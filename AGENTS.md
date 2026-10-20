# LilyInEM

双进程透传：Ender Magnolia（EM，UE5.4.4，**Host**）通过 UE4SS + Detours 内嵌运行 Ender Lilies（EL，UE4.26.2，无头 **Guest**）。EM 管地形 / 敌人 / 相机 / 存档 / 暂停 / 交互；EL 管莉莉 / 精灵 / 动作 / 原生菜单与特效，画面按真 alpha 合成进 EM。

## 目录

| | |
|---|---|
| `native/host/` `native/guest/` `native/rt/` `native/core/` | Host / Guest / 共享运行时 / 可离线单测的纯函数（协议、映射） |
| `native/tests/` | 不依赖引擎的单测（`test_core.cpp`） |
| `tools/` | `start.ps1` / `stop.ps1` / `deploy.ps1` / `eval.ps1` / `capture.ps1` / `menuprobe/` |
| `docs/` | `TODO.md` 需求与进度、`INPUT_MATRIX.md` 输入权责、`TRUE_ALPHA.md` 真 alpha、`REST_MENU_PANEL.md` 面板校准、`MANUAL_TEST.md` 手动测试 |

## 约定

- **PowerShell 一律先设编码**：`[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;`；Python 用 `C:\Dev\miniconda\python.exe`。
- 构建 / 测试：`build.bat`（或 `build.bat test`），输出在 `build/`。
- 部署：`tools\deploy.ps1 -Target all|host|guest`；启动 `tools\start.ps1 -Role host`（Host 会拉起 Guest 并用 Job 绑定）。
- **只用 `devconfig.json` 指定的隔离实例**（`Win64_LilyInEM`）。禁止碰 `Win64_For_Mod_Dev`、`Win64_LAN_Host/Client`、`LAN_Test_Data`、ELMP 研究构建和正式存档。
- **碰撞 / 能否穿越**：几何不分类，只看“与莱拉克胶囊互相 Block”+ 逐组件形状校验（开放曲面只保留表面）；行为机制（单向、移动、可破坏…）**只登记在 `native/core/terrain_kinds.h`**，Host 分类和 Guest 替身都从这张表读，不在别处按类名判断。验证用差分 Oracle：`tools\collision_check.ps1`（EM 用莱拉克 Profile、EL 用莉莉 Profile 探同一网格，退出码 = 不一致格数，理想为 0）；报告假障碍时先跑它。
- 在线探针：`tools\eval.ps1 -Role host|guest -Code '<lua>'`（UEBridge，游戏线程执行）；截图：`tools\capture.ps1 -Role host -Name <n>`；离线资产：`tools\menuprobe --tree / --funcs`（CUE4Parse）。
- 反射调用 UE 函数用 `ue::Call`（`ue.h`）；**不允许给事件传空 / 错误类型的结构体参数**（曾因 `FKeyEvent` 空指针把 Guest 弄崩）。
- 类成员 / 函数签名以 `tools\menuprobe` dump 和 `native\*\` 里的 SDK 头为准；语义不确定时 grep `C:\Code2\UnrealEngine-4.26.2-release` 引擎源码，不猜。
- 输入规则：见 `docs\INPUT_MATRIX.md` —— 莉莉动作键 = EL 设置，交互 = EM 键，撞键 EM 优先；菜单键一套（EM 的）。
- 生成物不入库：`Detex.dll`、`build/`、各实例 `Saved/`。不主动 commit / push。
