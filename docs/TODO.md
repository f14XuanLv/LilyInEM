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
- [ ] 抠像仍是阈值法（`h_render.cpp` 的 `kKeyHlsl`），深色像素可能被抠掉。可以改成双背景求 alpha。
- [ ] 色彩：EM 色调映射会让莉莉偏灰（`Toggles.quad_gain` 已预留，没有逆色调映射）。
- [ ] 世界层通道不可用时的 Present 兜底合成没有实现（v3 只保留了世界层主通道）。
- [ ] LilyMenu 全屏透传时，EL 画面里的位置和 EM 世界不对齐；标题显示的是 EL 休息点名。

### 菜单 / 系统
- [ ] EL 休息菜单在 EM 长椅场景下的“休眠菜单”（跪坐时 EL 菜单隐藏，选装配时才唤醒）没有实现：目前长椅时只播放跪坐动画，F5 / 重定向会重新打开一个 EL 休息菜单。
- [ ] 打开 EL 菜单依赖当前 EL 地图里有 `BP_Interactable_RestingPoint_C`；地图里没有时会失败（只写日志）。
- [ ] EM 档案页（Records）仍显示莱拉克的数据。
- [ ] 成长系统没有桥接：EM 的经验 / 掉落给的是莱拉克，莉莉等级和强化材料的来源待设计。
- [ ] EM 与 EL 两边存档各自独立，没有对齐或校验。
- [ ] 发布打包：安装器、Steam 库路径探测、`-dx11` 启动提示；`LilyInEM.ini` 目前由 `deploy.ps1` 生成，指向开发机实例。

### 工程
- [ ] 本仓库根目录的 `AGENTS.md`（规则、命令、事实索引）没有写。
- [ ] Fake Host / Fake Guest 契约桩（参考 `D:\CodeT\QJRef\passthrough-mod-toolkit\stubs`）。
- [ ] 代码还没有提交到 git（`D:\CodeT\LilyInEM` 目前没有任何 commit）。
