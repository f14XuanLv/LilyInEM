# 键位矩阵与输入权责

> 原则（2026-10-19 与用户确认）：
> - **莉莉的动作键以 EL 键位设置为准。** 莉莉的能力是 EL 的，和莱拉克不是一一对等的（见 §3），所以不按莱拉克的槽位硬套。玩家在 EL 设置的「控制器 / 键盘」页（F5 → 设置）给莉莉配键。
> - **交互用 EM 的交互键。** 交互是 EM 的世界业务，EM 世界里的提示显示的也是 EM 的键。
> - **撞键时 EM 优先。** EM 在莉莉模式下仍保留的系统键（暂停 / 游戏菜单 / 小地图 / 镜头偏移 / 交互）如果和莉莉的某个绑定撞键，莉莉那条绑定失效，并弹出提示和写日志。
> - **菜单只有一套键：EM 的菜单键。** EL 菜单的键盘和手柄 UI 绑定都改成 EM 的，提示与实际按键一致。
> - **同一时刻只有一个输入主人**（Host 决定，`Ctx`）。切换主人的那一刻还按着的键归上一个主人，直到松开。
>
> 代码：映射与合成 `native/core/input_map.h`（离线单测 `test_input_routing`）；EM 键位读取与合成 `native/host/h_input.cpp`；
> 路由 `native/host/h_render.cpp` `poll_input`；EL 键位读取 / 菜单键 / 键位页 `native/guest/g_body.cpp`
> （`publish_el_bindings` / `sync_el_menu_keys` / `kHiddenBindingRows`）；投递 `g_frame.cpp` `forward_menu_keys`；手柄透传 `g_input.cpp` `on_xinput`。

## 1. 数据来源（运行时读取，跟随玩家改键）

| 内容 | 读法 | 频率 |
|---|---|---|
| 莉莉动作键（EL 设置） | Guest：EL `InputSettingsSubsystem::GetInputChordsForAction`（Jump / Dodge / Attack / Summon_1 / Summon_2 / SummonSetSwitch / Hook / Heal）、`GetInputChordsForAxis`（Vertical / Horizontal，键盘移动）→ `Control::el_bindings`。EL 的设置存档读完（`SaveSubsystem::IsSettingsLoaded`）之后才发布，之前的值是 `DefaultInput.ini`，不是玩家的设置 | 每 2 秒 |
| EM 交互键与保留键 | Host：EM `InputSettingsSubsystem::GetMappedKey`（`kEmInteract`、`kEmReserved`） | 每 2 秒 |
| EM 菜单键 | Host：EM 没有 getter，只能对约 140 个键逐个调 `UMGBPFLibrary::IsKeyUIInputType` → `Control::ui_layout` | 每 10 秒，约 1 ms |
| 静态参考 | CUE4Parse：EM `IMC_Default` / `IMC_Chord` / `IMC_Interact`；EL `DefaultInput.ini`、`WBP_Settings*` / `WBP_KeyBindings*` 控件树与字节码（menuprobe `--tree` / `--funcs`） | — |

合成（`compose_bindings`）：EL 绑定（去掉 EL 的「调查」）+ EM 交互键，再去掉所有和 `kEmReserved` 撞键的项。结果交给渲染线程的 `lily_actions`。

### 撞键提示（实时）

| 时机 | 延迟 | 提示 |
|---|---|---|
| 在 EL 键位页改完一个键（"请按键"结束） | 1 帧：Guest 立即连续发布 3 帧，Host 每帧读 `el_bindings` | ① 那一行立刻变红（Guest 对 `WBP_KeyBindingEntry` 调 `SetColorAndOpacity`，按 `Control::conflicts_kb/pad` 的行位 `conflict_bit`）；② 键位页左下角常驻说明"红色的键与 EM 撞键…：祈祷（R）→ EM 小地图"，冲突解除后自动消失 |
| 在 EM 设置里改键 / 刚进游戏 | ≤ 2 秒 | 页面外：屏幕上方提示 6 秒，列出冲突，并说明在哪里改键；冲突全部解除时提示"冲突已解除" |
| 一直存在的冲突 | — | 每次打开 EL 键位页都会看到红色行和左下角说明；日志 `Lily binding dropped: <动作> on <键> (EM <映射> wins)` |

键位页上不弹顶部提示，避免挡住键位行，由红色行和左下角说明代替。

## 2. 输入主人（`Ctx`）与切换锁存

| Ctx | 谁收键 | 莉莉 | EM |
|---|---|---|---|
| `Gameplay` 莉莉模式自由行动 | 莉莉动作键 → 莉莉；EM 保留键 → EM | 行动 | 只响应保留键（莱拉克 `DisableInput`） |
| `LilyMenu` EL 菜单（休息点 / 子菜单 / EL Esc 菜单 / 对话框） | 全部 → EL 菜单 | 冻结 | 键盘、手柄全部吞掉，世界暂停 |
| `EmMenu` EM 菜单 / 暂停 / 事件（含 EM 快速移动地图） | 全部 → EM | 冻结 | 原生 |
| `Overlay` F6 装配面板 | ImGui | 冻结 | 全部吞掉 |
| `Inactive` 莱拉克模式 / Guest 不在 | 全部 → EM | — | 原生 |

**切换锁存（Latch）**：`Ctx` 改变的那一刻还按着的键 / 按钮 / 扳机 / 摇杆，对新主人来说算"没按"，松开后才恢复。

- **莉莉侧**：每次切换都生效，进入或离开 EL 的"请按键"状态也算一次切换。例：B 关掉 EM 菜单后，莉莉不会放出技能 2；在长椅按交互键打开休息菜单后，菜单不会立刻被操作。
- **EM 侧**：离开 LilyMenu / Overlay 时生效。WndProc 丢弃锁存键的 `WM_KEYDOWN` 自动重复，XInput 屏蔽锁存的按钮。例：Esc 关掉莉莉菜单后，EM 暂停菜单不会跟着弹出。

## 3. 能力对照表：莉莉 ≠ 莱拉克

| 莉莉（EL `ECommandInputTypes`） | 莱拉克（EM） | 莉莉模式下用谁的键 |
|---|---|---|
| 跳跃 Jump | 跳 `IA_Jump` | EL 设置 |
| 回避 Dodge | 闪避 `IA_Dodge` | EL 设置 |
| 攻击 Attack（主技能精灵） | 攻击 A–D `IA_Attack_A..D`（四个侍从各一个键） | EL 设置 |
| 技能 1 Summon_1 | 同上 | EL 设置 |
| 技能 2 Summon_2 | 同上 | EL 设置 |
| **切换技能组合 SummonSetSwitch** | **没有**（莱拉克没有实时切换侍从组） | EL 设置 |
| 勾上 Hook（单键） | 钩爪 `IA_Hook`（手柄是 `Special_Modifier` + 键的组合键） | EL 设置 |
| 恢复的祈祷 Heal | 回复 `IA_Heal` | EL 设置 |
| — | 特殊攻击 `IA_SpecialAttack_*`、修饰键 `IA_Special_Modifier` | 莉莉没有，不映射 |
| 调查 Interact | 交互 `IA_Interact` | **EM 的交互键**（EL 页上这一行已隐藏） |
| 小地图 / 主菜单 / 暂停 / 镜头（EL 世界业务） | 小地图 / 游戏菜单 / 暂停 / 镜头偏移 | **EM**（EL 页上这几行已隐藏，EL 的输入事件已掐） |

旧做法是把莱拉克的攻击 A–D 硬套成莉莉的主技能 / 技能 1 / 技能 2 / 切换组，再借用莱拉克的组合钩爪。这在语义上不成立，而且会和玩家在 EL 里配的键不一致（例如 EL 设的是 A = 切换技能组合、RT = 勾上）。已删除。

## 4. 矩阵（本机实测值，2026-10-19）

### ① EM 菜单键（EM 原生；莉莉菜单也用这套）

| 意图 | 键盘 | 手柄 |
|---|---|---|
| 确认 / 取消 / 上下文 | Enter / Esc / Tab | A / B / Y |
| 下一页 / 上一页 | E / Q | RB / LB |
| 导航 | WASD、方向键 | 十字键、左摇杆 |

### ② 莱拉克（EM 原生，本机改过键）

键盘：Space 跳、LShift 闪避、H/J/K/L 攻击 A–D、Q 回复、E 钩爪与交互、Tab 菜单、Esc 暂停、R 小地图、方向键镜头。
手柄：RB 跳、LB 闪避、X/B/Y/A 攻击 A–D、LT 回复、L3 交互、RT+RB 钩爪、Back 菜单、Start 暂停、R3 小地图、右摇杆镜头。

### ③ 莉莉（`Gameplay`）= EL 设置 + EM 交互，EM 保留键优先

| 动作 | 键盘 | 手柄 | 来源 |
|---|---|---|---|
| 移动 | WASD | 左摇杆、十字键 | EL（键盘）/ 固定（手柄） |
| 跳跃 | Space | RB | EL |
| 回避 | LShift | LB | EL |
| 攻击 | J | X | EL |
| 技能 1 | K | Y | EL |
| 技能 2 | L | B | EL |
| 切换技能组合 | Q | A | EL |
| 勾上 | P | RT | EL |
| 恢复的祈祷 | ~~R~~ **失效** | LT | EL；键盘 R 和 EM 小地图 R 撞键，EM 优先。EL 键盘页上这一行显示为红色，到 EL 设置里改一个键即可恢复 |
| 交互（长椅休息等） | E | L3 | EM |
| 归 EM | Tab / Esc / R / 方向键 | Back / Start / R3 / 右摇杆 | EM 保留键 |
| Mod 热键 | F5 EL Esc 菜单 · F6 装配面板 · F7 莉莉/莱拉克 · F8 显示莱拉克 · F10 调试 | — | — |

### ④ 莉莉菜单（`LilyMenu`）——与 ① 一致

- **键盘**：Host 用 EM 的菜单键得出意图位（`InputFrame.menu`），Guest 按 `Control::ui_layout.vk` 投递同一个键。EL 的键盘 UI 绑定被改成同样的键（EL 默认是 Z / X / C / D / A）。读存档后 EL 会把自己的绑定恢复一次，所以每 2 秒按"显示的键"（`GetKeyForUIInput`）检查一次并重设。
- **手柄**：手柄状态原样交给 Guest 的 XInput 钩子（`InputFrame.pad`），只去掉 Start / Back。EL 的手柄 UI 绑定也改成 EM 的手柄菜单键（`OverrideUIInput` 按键所属的设备只替换那一条绑定），所以 EL 按原生方式导航，提示是手柄图标。
- **掐掉的 EL 动作**：`PC_Base` 的 `InpActEvt_MainMenu`（Tab）、`Minimap`（Q）、`DEBUG`、`DEBUG_Modifier`、`Pause`。
- **EL 设置页**：保留 游戏 / 声音 / 控制器 / 键盘，移除画面（EM 管画面）。键位页里只显示莉莉的动作行，下面这些行已隐藏（`kHiddenBindingRows`，按控件名）：
  - 调查（交互归 EM）、小地图、主菜单、镜头上 / 下 / 左 / 右；
  - 整个「UI Inputs」分组（菜单键跟随 EM）。
- **在 EL 键位页改键**：选中一行按确认后，EL 进入"请按键"状态（`OnKeyBindingModeChanged(IsSelectingKey)` → `GuestStatus.selecting_key`）。这段时间 Host 不再发菜单意图，改为把所有按下的键原样发给 EL（`InputFrame.keys`），所以任意键都能绑定。手柄改键本来就走原生透传。改完约 2 秒后，Host 收到新的 EL 绑定并重新合成；如果新键撞了 EM 保留键，会立刻提示。

## 5. 验证

- 单测 `test_input_routing`：
  - 键名解析；
  - 按本机 EL / EM 设置合成：手柄 A = 切换技能组合、RT 单键 = 勾上、L3 = 交互；键盘 F（EL 调查）不生效、E（EM 交互）生效；R（祈祷）和 Up（移动）因撞键被丢弃并上报；
  - EL 绑定未发布时只有手柄移动和 EM 交互可用；
  - 菜单意图、EM 改 UI 键后跟随、手柄去掉 Start / Back；
  - Latch。
- 实机：
  - Host 日志 `EL bindings received: Jump=SpaceBar/Gamepad_RightShoulder … SummonSetSwitch=Q/Gamepad_FaceButton_Bottom Hook=P/Gamepad_RightTrigger Heal=R/Gamepad_LeftTrigger`、`EM world keys read (interact E / Gamepad_LeftThumbstick)`、`Lily binding dropped: Heal on R (EM Keyboard_Minimap wins)`；
  - 键位页截图：键盘页只剩移动 / 跳跃 / 回避 / 攻击 / 技能1 / 技能2 / 祈祷 / 切换 / 勾上，手柄页同理；
  - 选中「恢复的祈祷」按确认 → "请按键"，Guest 日志 `EL key binding: waiting for a key (raw keys)`，取消后 `done`，Esc×2 回到自由行动；
  - 撞键提示：键盘页「恢复的祈祷 R」为红色，左下角说明"祈祷（R）→ EM 小地图"。运行时把跳跃改到 Tab，跳跃行立即变红，说明里加上"跳（Tab）→ EM 游戏菜单"，日志 `Lily binding dropped: Jump on Tab (EM Keyboard_Menu wins)`；改回 Space 后，红色和那一条说明一起消失。
- 需要真实按键的场景：见 `MANUAL_TEST.md` D9–D17。

## 6. 以后

- 手柄打开 EL Esc 菜单：现在只能用 F5。
- 手柄移动固定为左摇杆和十字键（EL 的手柄移动轴不读）。
- F6 装配面板上的键位标签（"主技能 H/X"）还是静态文字，应该改为显示 EL 的实际键。
