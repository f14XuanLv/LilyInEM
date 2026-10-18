# LilyInEM 未实现功能与已知问题（非测试项，2026-10-17，v0.3.0）

> 本仓库全新编写，`D:\CodeT\LilyInMagnolia` 已冻结、只读，只迁移知识，不复制文件。
> 设计依据：`D:\CodeT\LilyInMagnolia\docs\{REQUIREMENTS.md, ROUTE_CORRECTION_v3.md}`（v3 路线）。
> 当前实测基线：两端连通；木偶位置误差 0；地形扫描 P99 约 0.33–0.40 ms；莉莉在 EM 世界中可移动、可走上斜坡。

## 0. 代码地图（给下一位 Agent）
| 模块 | 文件 | 职责 |
|---|---|---|
| 版本相关二进制数据 | `data/binary/{EL,EM}.json` | SHA-256、RVA + 前缀、原生偏移；运行时由 `rt/binary.*` 读取；`tools/verify_binary.py` 离线校验 |
| 核心（纯 C++，有单测） | `native/core/*.h` | 协议 `protocol.h`、映射 `frame_map.h`、姿态环 `pose_ring.h`、地形合并 `terrain_merge.h`、伤害换算 `combat_map.h`、事实 `facts.h` |
| 运行时基础 | `native/rt/*` | 共享内存、hook、反射调用 `ue.*`（支持按位置或按参数名传参）、输入、D3D |
| Guest | `native/guest/*` | `g_body` 主循环 / 映射 / 发布；`g_terrain` 盒池；`g_combat` 代理 / 受击；`g_stage` 摄影棚；`g_input`；`g_frame` |
| Host | `native/host/*` | `h_world` 编排 / 上下文 / 菜单 / 锁定 / 面片；`h_puppet`；`h_terrain`；`h_combat`；`h_render` |
| 命令 | `build.bat [core] [test]`、`tools/{deploy,start,stop,eval,capture}.ps1` | 路径只来自 `devconfig.json` |

## 0.5 EL 无头引擎（摄影棚）现状
- **所在地图**：EL 标题画面自动读取存档槽 0（独立 UserDir 里的副本），停在存档所在的 EL 地图。2026-10-17 实测是 `map_church_08`（`PersistentGameMap`）。没有专用空地图：EL 的游戏流程（WorldLoaderSubsystem、休息点、精灵）需要一张真实的 GameMap。
- **地图的角色**：EL 地图的地形 **不重要，也不应该起任何作用**。莉莉脚下唯一有效的碰撞是 EM 地形镜像的 Static 盒池；EL 地图只提供一个能运行游戏逻辑的世界。
- **隔离规则**（`g_stage.cpp`）：锚定时全量扫描一次，之后对 BeginPlay 增量判定。
  - 除以下几类外，所有 Actor 都隐藏并关闭碰撞：莉莉、精灵及其能力、相机、`BP_CameraClampVolume_C`（必须保活，只把它的 4 个阻挡盒关掉，并扩大到 500000）、引擎基础设施、`DefaultPhysicsVolume`、EL 休息点（用于跪坐和打开菜单）。
  - EL 敌人额外关闭 Tick。
- **摄影棚抬升（2026-10-17 新增）**：锚定时把莉莉抬到 EL 地图上方 60000 UU（`kStudioLift`），被漏掉的物体、后生成的物体、流式加载的子关卡都碰不到她。
- **已修复的问题**：旧规则只要类名含 `PhysicsVolume` 就保留，导致 EL 的 `BP_CustomPhysicsVolume_Water_C`（`map_church_08` 里有 3 个）可见且有碰撞。莉莉走到 EL 水体的位置时会进入游泳状态，EL 的水面也会被导出画面带进 EM。
  - 现在只保留 `DefaultPhysicsVolume`。水体以后应由 EM 侧提供（S4：EM 水体 → EL `ACustomPhysicsVolumeWater` 镜像）。
- **待做**：在 Guest 状态输出里加入“非自有、仍可见或仍有碰撞的 Actor 数”作为 Oracle（理想值为 0）；考虑固定一个“摄影棚存档”（选一张实体最少的 EL 地图），不依赖玩家存档在哪张图。

## 1. 已知问题（需要修）
1. **地形版本号刷新过于频繁**：Lilac 移动时窗口边缘每移动一列，矩形集合就变一次，revision 几乎逐帧递增（实测约 5 秒涨了 500+）。Guest 每次只重摆变化的盒子，功能上可用，但会产生不必要的 SetMobility 切换。
   - 修法：发布时以“窗口内部、离开边缘 N 列”的区域为准；或给边缘矩形做 hysteresis，只在内部内容变化时 bump revision。
2. **地形接缝 / 下落帧偏多**（待手动测试确认）：曾观察到 `falling_frames` 增长较多。可能原因：
   - 斜坡链没有合并成功，退化成 16 UU 的台阶；
   - 斜坡板（`roll`）的符号或位置有误差；
   - 相邻盒子之间的接缝（目前只在 Y 方向外扩 0.5 UU）。
   - 排查：在 EL 侧把盒子画出来（`DrawDebugBox`），或导出 `TerrainSnapshot` 与 EM 射线结果对照。
3. **列扫描的垂直窗口**：Lilac 垂直移动超过 1/4 窗口高度时，会把所有列标记为过期并全部重扫；扫完之前 `ready=false`。Guest 已放开时不受影响，但会短暂停止发布新地形。可以改成增量重扫。
4. **区域切换后的大跳变**：EM 切换区域后，木偶会请求 rebase（以“当前莉莉脚底 ↔ 当前莱拉克脚底”为准）。如果切换时莉莉正在半空，会继续下落到新地形上。需要手动测试确认体验。
5. **Lilac 碰撞类型缓存**：每个组件是否算作地形的判定（verdict）只在 `terrain_reset` 时清空。运行中改变碰撞响应的物体（机关门、可破坏墙）判定可能已过期。

## 2. 未实现功能（按阶段）

### S1 收尾
- [ ] `tools/verify.ps1`：读取两端的 `Host/GuestCounters`，按 ROUTE_CORRECTION_v3 §7 的 Oracle 自动判定（位置一致、脚底贴地、下落帧数、攀爬计数、地形缝、性能），截图附 SHA-256。
- [ ] Guest 计数器 `autoclimbs` 没有接线（需要 hook `UAutoClimbComponent.OnAutoClimb`，或观察 Spine 动画名）。
- [ ] “脚底贴地”Oracle：Host 只读射线测量木偶脚底到地面的距离（目前没有这项计数）。
- [ ] 帧时间 Oracle（Host 全帧时间，不只是地形扫描耗时）。

### S2 受击管线（代码已写、未实测，可能需要调整）
- [ ] 回放现在用的是兜底路径 B（`SetLastAttackerDirection` + `HPComponent::DoDamage` + `LaunchKnockback`）。击退数据暂时借用了莉莉 `KnockbackComponent.AirborneLaunchKnockback`。
  - 目标是路径 A：生成 EL 敌人判定框（`/Game/_Zenith/Gameplay/Abilities/e2000_Knight/AbilityHitbox_e2000_Default`），设 `AE_Damage.AbsoluteDamage`、`AE_Knockback.bBypassStaminaCheck=1`，再调 `ExecuteAbilityEffectsOnTarget`（RVA `0x00CA5580`，需要先加进 EL.json），并保持实例存活直到效果队列跑完。这样格挡 / 弹反（芙莉蒂雅之戒）、受击无敌才完全走原生。
- [ ] 元素 / 状态异常（燃烧、冰冻、电击）没有映射到 EL；目前只传了 element / type 字节，没有使用。
- [ ] 持续伤害（毒雾）每跳都会产生一个 Hurt 事件，需要确认频率和体验。
- [ ] `HurtPayload.source_id`：伤害来源可能是能力 Actor，Owner 链最多只回溯 4 层，可能取不到对应实体。

### S3 造伤管线（代码已写、未实测）
- [ ] 代理类 `BP_e2010_Slime_C` 的 AssetRegistry 加载、`AutoPossessAI=0`、冻结、`HurtCapsule` 缩放：都还没验证。代理的 `HitCapsule` 关闭后可能仍有接触伤害组件需要处理。
- [ ] EM 侧结算目前是轻量路径（`StatHP::DoDamage` + `LaunchHitStop`）。没有 `ShakeComponent::LaunchShake`（需要 `FShakeData`）、削韧、部位破坏、火花和音效。
  - 完整路径：`Lilac.AbilityComponent.LaunchAbilityInstant(AEP_Player_Attacks_C …)`。注意：Lilac 的 `AbilityComponent` 目前在锁定列表里，Tick 已关闭。
- [ ] 代理对莉莉攻击触发的击退，没有转发给 EM 的 `StartKnockback`。
- [ ] 实体尺寸取自敌人胶囊，没有读取 `UCollisionComponent.HurtboxReferences` 的盒子尺寸。
- [ ] 实体普查只在激活时跑一次 `FindAllOf`，之后靠 BeginPlay 增量；rebase 或流式关卡加载后是否会漏掉敌人，需要确认。

### S4 场景机制
- [ ] 单向平台：Guest 池化 `BP_OneWayPlatform_C`（对象类型 17），以支持 `UCA_FallThrough_C` 下穿。目前 `kTerrainOneWay` 被当作实心块。
- [ ] 动平台 / 电梯（`AMovingPlatform_Elevator / _Spline`、`AMovingBlock_Elevator`）：进实体通道，在 EL 侧做成运动学盒，让莉莉原生跟随底座移动。目前它们被当作静态地形每秒重扫一次，而且木偶不会跟随底座。
- [ ] 水体：`ACustomPhysicsVolumeWater`。
- [ ] 钩爪点：`AHookPoint(_Moving)` 映射为 EL `AHookPoint` 代理。
- [ ] EM 对莱拉克施加的推动（风、传送带）。

### S5 HUD 与画面
- [ ] 莉莉 HUD 目前是 ImGui 简易版（HP 条、祈祷、奥义槽、精灵 ID 文本）。需要换成 EL 原版贴图和排版（贴图经 CUE4Parse 提取，可参考 `D:\CodeT\ELMP\tools\icons\extract.ps1`），并显示精灵剩余次数 / 冷却（`Vitals.slots` 字段已留好，Guest 还没有填）。
- [x] 抠像已替换为真 alpha 通道（2026-10-19，详见 `docs\TRUE_ALPHA.md`）：Guest `USceneCaptureComponent2D`（`SCS_SceneColorHDR` → 预乘 RGB + inv-opacity A）经第二块共享纹理发 Host，`s = bb.rgb / cov`。`keyed()` 保留为 capture 未就绪时的回退。
- [x] EL 导出分辨率跟随 EM（2026-10-19）：`Control::guest_res_w/h` = EM backbuffer × `ResScale`（`LilyInEM.ini` 可调，默认 5/3，上不封顶）；Host 在 Present 里推送，Guest 子类化窗口解除 `WM_GETMINMAXINFO` 桌面钳制后 `SetWindowPos`（`r.setres` 会被钳到桌面）应用，ResizeBuffers → 共享纹理 + capture RT 自动重建。实测全链路（1280×720×4/3 → 1707×960；默认档 EM 1920×1080×5/3 → 3200×1800），世界层与菜单均降采样、Split 正常。
- [ ] 色彩：EM 色调映射会让莉莉偏灰（`Toggles.quad_gain` 已预留，没有逆色调映射）。
- [ ] 世界层通道不可用时的 Present 兜底合成没有实现（v3 只保留了世界层主通道）。
- [x] **休息点的莉莉 / 英灵画面：已改为 SceneCapture 真 alpha**（2026-10-19，`docs\TRUE_ALPHA.md`）：
  - ~~世界层仍是单帧亮度抠像~~ → `chars` 通道给真实覆盖率，暗色盔甲 / 半透明边缘 / 纯黑特效都正确；`keyed()` 仅作 capture 未就绪回退。
  - EL 摄影棚背景像素实测为精确 0；EL 后缓冲是 A2B10G10R10，alpha 被写成不透明，**不能直接用 alpha**——所以 alpha 走独立的 SceneCapture RT，不碰后缓冲。
  - 双背景求 alpha（EL 每帧交替黑 / 白：`PlayerCameraManager::SetManualCameraFade(1, white)` 画在 UI 之下、场景之上）对 UI 数学上精确，但 Guest 的逐帧标记与帧内容不能稳定对齐（游戏线程 post-tick 置标记 vs 渲染线程 Present 读取），同色两帧相邻即整屏白闪；用帧角落判黑白又会被底部页脚 / 全屏菜单遮挡误判。已整体撤掉，现为单帧合成（无闪烁）。
  - camera fade 会把整个场景（含莉莉）刷白，所以它只能给 UI 求 alpha；要给角色 / 英灵求精确 alpha，需要只在**背景**后放一块纯白无光照板（或 SceneCapture / 每帧可靠的内容标记），并保证同一 Present 里的黑白配对。
  - 休息期间 EL 相机 `WorldSpaceLocationOffset.Y = -280`，莉莉与英灵位于画面右侧（UV≈0.47–0.97），`FrameKind::Split` 的世界矩形 = 莉莉脚底左 330 / 右 540 / 上 430 / 下 40 UU；英灵位置取自 EL 休息点 `SpiritLocation_1..3`（-170 / +170 / +340 UU，播 `rest` 动画 track 9）。
  - 全屏子菜单（技能 / 遗物 / 强化）与 Esc 菜单按 EL 原样**不透明**显示（`FrameKind::Menu`，EM 世界被遮住，莉莉在世界层停在最后一帧）。

- [x] **EL 休息菜单左侧黑色背景：已按 EL 原值复刻**（2026-10-19，详见 `docs\REST_MENU_PANEL.md`）
  - 已落地：`ps_menu` 从提取的图集 alpha 精确重放两块黑叠黑的背板——`BG`（`T_Menu_RestPointTitle_Bg`，800 设计像素宽、全屏高、alpha 0.8 平台到 x=480、x=791 渐隐到 0）和页脚横条（`T_Menu_Footer_Bg`，底部 128 设计像素高、0→0.30 纵向渐变）。LUT 在 `native/host/rest_panel_alpha.h`（menuprobe 生成），800×2 R8 纹理 t1。
  - UI 缩放按 EL `ScaleToFit`（min(w/1920, h/1080)）由实际帧尺寸推导：1280×720 时面板物理宽 533 px、横条高 85 px。
  - `Show`/`Close`/`HideUI` 动画驱动根控件 `ColorAndOpacity.A`，Guest 每帧发布为 `FrameMeta::menu_alpha`，Host 用它乘背板 alpha（面板随菜单淡入淡出）。
  - `BG` 保持 Collapsed 不变：它在帧里黑叠黑无法取 alpha，Host 侧合成（面板在内容之下，over 合成 `a = key + dim*(1-key)`）。

### 菜单 / 系统
- [x] （2026-10-18）**EM 长椅 → EL 休息菜单**
  - EM 长椅菜单在莉莉模式下保持存在但不可见：它在 `WBP_Event` 里，开场动画驱动 RenderOpacity，所以用 `Hidden` 可见性并每帧重设；自身按钮全部拦截；`CloseMenu` 的 HideAnim 在 Hidden 下不 Tick，改为直接 `RemoveFromParent`；它的开场动画导航锁永不释放，打开 EM 地图时 / 长椅结束时调用 `UMGSubsystem::ResetNavigationLocks` 并给 EM 地图 `SetFocus`（否则 EM 快速移动地图无法交互、Esc 无法回退）。
  - EL 休息菜单直接 `Create + AddToViewport`（不走 EL 坐椅子事件、不依赖 EL 休息点），标题统一为“烟尘之国”，只留 快速移动 / 精灵 / 遗物 / 强化，上下导航在四项间循环；保存 / 离开 / 回想 / 挑战隐藏并拦截（离开 = 取消键）。
  - 快速移动：EL 菜单休眠（Collapsed）+ EM `WBP_RestMenu::OpenGameMenu(7)`；地图取消 → 唤醒 EL 菜单；选目的地（`OnFastTravelRequested`）→ 两边关闭并执行 EM 传送。EL 菜单取消 → EM `CloseMenu`；EM 菜单 Destruct → EL `CloseMenu(false)`。
  - 休息时：Lilac 的英灵（`BP_*_RestPoint_C`）每 0.1 s 隐藏一次；EL 当前精灵组的英灵按 EL 休息点 `SpiritLocation_1..3` 站位并播 `rest`；莉莉播 `rest_loop`。
  - 画面：休息菜单单独打开时为 `FrameKind::Split`（莉莉 / 英灵矩形实时进世界层、动画不冻结，矩形外为菜单）；打开子菜单 / Esc 菜单为 `FrameKind::Menu`（不透明显示，莉莉隐藏、世界层冻结）。
- [x] （2026-10-18）长椅镜头慢：`EVT_RestPoint` 的 `EventAction_MoveTo_Actor`（AcceptanceRadius 2）要把莱拉克走到长椅中心，木偶的移动组件不 Tick，只能等 EM 的卡住判定超时。现在交互时把莱拉克与莉莉按同一 Y 偏移对齐到长椅（映射不变），休息期间 ctx=EmMenu、木偶不驱动。
- [x] （2026-10-18）EL Esc 菜单（`WBP_Pause`）只能用 **F5** 打开（Gameplay 上下文，`MenuKind::Pause` → `PC_Base::TryPause`）；EL 自己的入口全部拦截：`InpActEvt_Pause`（菜单里转发的 Esc 曾误开它）、`OnApplicationWillDeactivate`（EL 窗口永远不在前台）、`TriggerOrDeferPause`。
- [ ] **需求：EL Esc 菜单条目全部保留，世界业务由 EM 接管**。现状：**一项都还没接管**，F5 打开的 `WBP_Pause` 全部按钮仍执行 EL 原生逻辑。按 `WBP_Pause` 实际按钮（CUE4Parse）分类：

  | 按钮 | EL 原生行为 | 判断 | 计划 |
  |---|---|---|---|
  | 继续 `Continue` | 关闭菜单、`OnResumeGame` | 不需接管 | 保持 EL 原生（已可用） |
  | 返回休息点 `BackToRestpoint` | `GameModeZenith::RespawnPlayerPawn`（EL 内传送 + 读图） | **需接管** | EL 世界是空摄影棚，原生执行会打乱锚定；应改为 EM 的“回到最近休息点 / 复活点”流程，或在莉莉模式下禁用 |
  | 返回标题 `ReturnToTitle` | `WorldLoaderSubsystem::OpenLevelAsset(TitleMap)` | **需接管** | 应触发 EM 返回标题（EL 跟随复位），否则只有 EL 回到标题、桥接断开 |
  | 退出游戏 `QuitGame` | `KismetSystemLibrary::QuitGame` | **需接管** | 只退出 EL 进程会让 Host 失去 Guest；应改为退出 EM（Job Object 会一并结束 EL） |
  | 成就 `Achievements` | 打开 EL `WBP_Achievement` | 不需接管（只读） | 可保留；是否改为显示 EM 成就待定 |
  | 设置 `Settings` | 打开 EL `WBP_Settings` | **部分接管** | 见下 |

  `Settings` 内部细分（按游戏业务判断，未逐页验证 `WBP_Settings` 结构）：
  - 不需接管：EL 的**键位设置**——莉莉的操作键由 Host 按 EM 键位映射成命令位注入，EL 的键位不参与游戏操作；但 EL 菜单键（Enter/Esc/C/A/D）由 Guest 以固定虚拟键转发，改 EL 的 UI 键位会让转发失效，因此建议**隐藏或锁定**这一页，而不是交给 EM。
  - [x] 需接管或屏蔽：**画面设置**（分辨率 / 窗口模式 / 帧率等）——EL 导出窗口尺寸由 Host 推送（EM × `ResScale`），改了会破坏帧导出；画面以 EM 为准。（2026-10-19 已移除）
    - 设置页显示 `1024×768` 是控件兜底：实际 3200×1800 不在受支持分辨率列表里；`ResetToDefault` 才会真的写 1024×768。
    - `WBP_Settings_C` 页签寻址（menuprobe `--funcs` 读字节码）：`SelectTab(i)` 遍历 `TabArray` 设 `SetSelected` 并 `SettingsSwitcher.SetActiveWidgetIndex(i)`；`GoToSideTab` = `(CurrTabIndex ± 1) mod TabArray.Length`。只 Collapse 页签会留空页；只删 switcher 页会让后面的页内容错位一格。
    - 做法照抄 EL 自己的非 PC 分支（`SetupSettings` 里 `IsPCBuild` 为假时）：Construct 后 `GraphicsTab` / `WBP_Settings_Graphics` `RemoveFromParent`，并从 `TabArray` 删掉该项（`g_body.cpp` post-Construct）。实测正反循环 游戏→声音→控制器→键盘 回绕正确，关闭重开幂等。Apply / Reset / 分辨率 / 模式 / 缩放五个处理器仍在 `kBlockedScoped` 里兜底。
    - 遗留：页签的鼠标点击处理器用常量 `SelectTab(3/4)`（控制器 / 键盘），移除后索引应为 2/3。Host 不向 EL 转发鼠标，目前不可触发；以后转发鼠标时要重映射。
  - 需同步：**音量**（EL 的 BGM / 环境音已被静音，莉莉的音效仍走 EL 音量）、**语言**（EL 菜单文字）——建议跟随 EM 的设置，而不是两边各自设置。
  - 待定：伤害数字 / HUD 显示等 EL 游戏性选项（EL HUD 已关闭，大多无效果）。
- [ ] 直开菜单 `MenuKind::Spirits/Relics/Strengthen` 现仅 debug `menu2/3/4` 使用；`MenuKind::Rest` 已改为直接创建，不再依赖 EL 地图里的 `BP_Interactable_RestingPoint_C`。
- [ ] `WBP_MainMenu` 的 A/D 翻页按钮可能仍能翻到 Map 页：目前只把 Map 页签设为 Collapsed，`GoToPage` 循环是否跳过未验证；Record / Collection / Tips 页显示的也是 EL 存档内容，与 EM 世界是否冲突待确认。
- [ ] `SetCanChangeEquipment` 目前开直开菜单时置 true 后没有还原；需要确认关闭菜单后该标记残留是否有副作用（参考 OPMod 的做法）。
- [ ] EM 档案页（Records）仍显示莱拉克的数据。
- [ ] 成长系统没有桥接：EM 的经验 / 掉落给的是莱拉克，莉莉等级和强化材料的来源待设计。
- [ ] EM 与 EL 两边存档各自独立，没有对齐或校验。
- [ ] 发布打包：安装器、Steam 库路径探测、`-dx11` 启动提示；`LilyInEM.ini` 目前由 `deploy.ps1` 生成，指向开发机实例。

### 工程
- [ ] 本仓库根目录的 `AGENTS.md`（规则、命令、事实索引）没有写。
- [ ] Fake Host / Fake Guest 契约桩（参考 `D:\CodeT\QJRef\passthrough-mod-toolkit\stubs`）。
