# 世界层真 alpha 通道（SceneCapture2D → 第二共享纹理）

> 2026-10-19。取代 `keyed()` 亮度抠像的主路径：`bb.rgb / (1 - cap.a)` 解出 EL 原生直出颜色与覆盖率。
> 本文同时记录这次的**排查 → 定型 → 实现**方法论，同类"图形学上拿不到的信息"问题可复用。

## 1. 问题与证据链

### 1.1 缺陷定位（只靠读代码 + 量化抓帧）

`h_render.cpp` 旧 `keyed()`：`a = max(smoothstep(0.03, 0.09, 四邻最小亮度), peak(c))`——纯亮度近似。

- 抓导出帧量化：精灵暗色盔甲大面积 `peak < 128` → alpha < 0.5 → 半透明幽灵。
- 数学结论（休息菜单 BG 那次已经证过）：**黑底预乘 RGB 里没有 alpha**——画不画一块纯黑带 alpha 的图，像素逐字节相同。抠像常数怎么调都只是权衡取舍，不可能"修好"。

### 1.2 可行性确认（eval 探针 + SDK 头检索）

需要回答三件事：莉莉能不能被 SceneCapture 拍到、相机数据拿不拿得到、RT 像素能不能读出。

| 问题 | 手段 | 结论 |
|---|---|---|
| Spine 渲染组件是不是普通 primitive | grep EL SDK 头（`D:\CodeT\EnderLilies`） | `SpineSkeletonRendererComponent : UProceduralMeshComponent`——可捕获 |
| `USceneCaptureComponent2D` 在 EL 里可用属性 | eval 逐个读反射属性（pcall 包裹） | `CaptureSource/TextureTarget/FOVAngle/AspectRatio/ProjectionType/OrthoWidth/ShowOnlyActors/HiddenActors/ShowFlags` 全在 |
| 相机 | eval 读 `PlayerCameraManager.CameraCachePrivate.POV` | FMinimalViewInfo 完整：FOV=90、ProjectionMode=0（透视）、OrthoWidth、AspectRatio=1.78 |
| UMG 会不会进 capture | 机制常识 | Slate/UMG 不是场景 primitive，天然排除 |

### 1.3 失败的探针（诚实记录，别再试）

- **Lua `ReadRenderTargetUV` / `ReadRenderTargetRawPixel`**：out-param 调用约定在 bridge 里走不通（`RemoteUnrealParam` 不在 Guest `_G` 里），`UEB.call` 也只是透传 `fn(obj, unpack(args))`。试了三种写法全 `?`。
- **`UKismetRenderingLibrary::ExportRenderTarget`**：能调通但产出 0 字节文件，放弃。
- **结论**：像素读回验证不必在 Lua 里纠缠——实现语言本来就是 C++，`TArray<FColor>&` out 参数在 `ue::Call` 里用 `out()` 槽位直接拿（`rt/ue.h` 已支持 TArray）。

## 2. 方案定型（引擎源码是唯一权威，不搜网）

全部判定依据来自 `C:\Code2\UnrealEngine-4.26.2-release` 的 `Select-String`/读文件，没有一条靠猜：

| 判定点 | 出处 | 结论 |
|---|---|---|
| capture 输出什么 | `EngineTypes.h` `ESceneCaptureSource` 枚举注释 | `SCS_SceneColorHDR` = "SceneColor (HDR) in RGB, **Inv Opacity in A**" |
| A 的确切语义 | `SceneCapturePixelShader.usf` | `OutColor = float4(SceneColorValue.rgb, saturate(a))`，注释 "intentionally keeping alpha inverted … artists invert to get foreground opacity" → **coverage = 1 − a** |
| 空像素 alpha | `BasePassRendering.cpp` | `kSceneColorClearAlpha = 1.0f`，inv-opacity 约定：空=1（全透明），覆盖率乘性累积 |
| RGB 形态 | `SceneCaptureRendering.cpp` 189 行注释 | "Scene capture color is already **pre-multiplied** by alpha" |
| 为什么不用 FinalColor 路径 | `PostProcessTonemap.usf:625` + `PostProcessing.cpp:746` | `OutColor.a = SceneColor.a` 仅当 `POST_PROCESS_ALPHA==1`（`r.PostProcessPropagateAlpha` 或 FXAA）才写出，默认不写 → 不可靠 |
| RGB 从哪来 | 上一条的推论 | backbuffer 已是 EL 最终色调（含 tonemap），**RGB 用 backbuffer、A 用 capture**，不用复刻 EL 的 filmic 曲线 |
| 渲染集怎么圈 | `SceneCaptureComponent.h` PRM_* + 本仓库 `g_stage.cpp::classify` | 摄影棚已把非莉莉/精灵/特效 actor 全部 `SetActorHiddenInGame` → `PRM_RenderScenePrimitives` 默认渲染即与 backbuffer 同集，**不需要任何 ShowOnly/Hidden 列表** |
| 组件怎么建 | `ActorConstruction.cpp` | `AddComponentByClass(cls, attach, xf, deferred=true)` → 设属性 → `FinishAddComponent` 注册 |
| 捕获怎么驱动 | `SceneCaptureComponent.cpp` | `CaptureScene()` = `SendAllEndOfFrameUpdates + UpdateSceneCaptureContents` 立即入队；门槛 `IsVisible()`（所以宿主 actor **不能隐藏**）；`GetCameraView` 显示 aspect 取自 RT 尺寸 → **RT 必须与 backbuffer 同尺寸** |
| RT 怎么建 | `KismetRenderingLibrary.h` + `TextureRenderTarget2D.h` | `CreateRenderTarget2D(world, w, h, RTF_RGBA16f=6, clear, false)` → `PF_FloatRGBA` → DXGI `R16G16B16A16_FLOAT`（format 10） |

### 合成数学（为什么 `bb.rgb / cov` 严格成立）

黑摄影棚上 EL 的每像素合成：`bb.rgb = Σ color_i·a_i`（预乘），capture 的 `cap.a = 1 − Π(1−a_i)`。同机位同帧、可见集一致 ⇒ `s = bb.rgb / cov` 是预乘→直出的精确还原，`s·a ≡ bb`。Host 写入世界层 RT 仍是 (直出, alpha) 契约，材质侧语义不变。

### 传输通道（不引入第二条机制）

- RT 的 `ID3D11Texture2D*` 拿不到（UE4SS 只暴露反射层）→ **Guest 侧 hook `ID3D11Device::CreateTexture2D`**（Host 已有同款 vtable hook 先例）：`capture_expect_rt` 置 want-flag 后再 `CreateRenderTarget2D`，首个匹配 desc 即抓。
- desc 唯一性：`RTF_RGBA16f` 全分辨率 + 仅 `RT|SRV`（引擎内其它全分辨率 FloatRGBA 池化目标都带 `TexCreate_UAV` → `BIND_UNORDERED_ACCESS` 不同）+ want-flag 时间窗。
- 兜底验证：抓到后首个非 Menu 帧起，30 帧后做一次 staging 回读——真 capture 必有 `a < 1.0` 的像素（Lily 在屏时）；全 1 → 抓错，丢弃并继续匹配。静默错抓变自愈。
- `FrameMeta` 加 `chars_handle`（协议 bump 到 v2），Present 时 `CopyResource` 进第二块共享纹理，零拷贝。

## 3. 实现清单

| 文件 | 内容 |
|---|---|
| `native/core/protocol.h` | `FrameMeta::chars_handle`；`kProtocol` 1→2 |
| `native/guest/g_capture.cpp`（新） | 生成 actor + `AddComponentByClass(SceneCaptureComponent2D, deferred)`，设 `CaptureSource/PrimitiveRenderMode/bCaptureEveryFrame=false/bCaptureOnMovement=false`，`capture_expect_rt` 后 `CreateRenderTarget2D`，写 `TextureTarget` 字段，`FinishAddComponent`；每 tick 读 `CameraCachePrivate.POV` → `K2_SetWorldLocationAndRotation` + `FOVAngle/ProjectionType/OrthoWidth` → `CaptureScene()` |
| `native/guest/g_frame.cpp` | `CreateTexture2D` 钩子（want+匹配+AddRef）、staging 回读验证、第二块共享纹理 + `meta.chars_handle`、resize/shutdown 释放 |
| `native/guest/g_body.cpp` | `publish_projection` 里调 `capture_tick(pc)`（同一相机源）；`reset_anchor` 里 `capture_reset()` |
| `native/host/h_render.cpp` | `chars` 纹理 t1（panel 移 t2）、`chars_on` 开关、`covered()`：`a = max(cov, peak(bb))`、`s = bb.rgb/a`（第 6 节）；`chars_handle=0` 时回退 `keyed()` |
| 坑 | `ensure` 撞 UE 宏 → `ensure_capture`；`ue::FProperty` 完整定义需 `<Unreal/CoreUObject/UObject/UnrealType.hpp>` |

## 4. EL 导出分辨率跟随 EM（2026-10-19 追加）

整条管线本来就是归一化 UV + UU 世界尺寸，所以分辨率是自由参数：

- `Control::guest_res_w/h`（Host 写，原子量）：`render_frame` 量到 EM backbuffer 尺寸就推 `EM × guest_res_scale`（默认 **5/3**，`LilyInEM.ini` `[Guest] ResScale` 可调，上不封顶）。
- Guest `tick` 轮询到变化 → `request_frame_size` 直接 `SetWindowPos` 那个**移出屏幕的窗口**。两层桌面钳制都要绕：`r.setres`/GameUserSettings 钳到逻辑桌面（本机 2560×1600@150% = 1707×1067），UE4 自己的 `WM_GETMINMAXINFO` 也钳（ptMaxTrackSize=工作区）——所以 `g_frame` 先 `SetWindowLongPtr(GWLP_WNDPROC)` 子类化窗口、把 max track size 提到 32000，再 SetWindowPos。WM_SIZE → swap chain ResizeBuffers → 共享纹理 + capture RT 全链路自愈（实测真实到达 3200×1800）。
- 收益：世界层和 EL 菜单都以更高分辨率渲染再经 Host 双线性降采样 = mild SSAA；EM 1920×1080 → EL 3200×1800（2.78× 像素）。

## 5. 验证

1. 日志三连（全绿才算通）：guest `scene capture armed (<WxH>)`、`capture RT texture grabbed (format 10)`、host `scene-capture alpha channel open`。默认档 EM 1920×1080 × `ResScale` 5/3 → `armed (3200x1800)`；尺寸随 `guest_res_w/h` 变。
2. `verify_capture_rt` 无 drop 日志。
3. 截图：暗部实心（s5000 黑骑士盔甲、莉莉裙摆阴影）、特效正常叠加、Split/开合菜单无残留。
4. 加法光：技能光晕 / 发光粒子在空白处可见、叠在半透明特效上的亮部不被削平（见第 6 节）。

## 6. 特效补漏：capture 没有覆盖率的光（2026-10-19 追加）

从 ParticleSystemManager（Cascade 资产编辑 Mod）的遍历路径 `Emitters → LODLevels → RequiredModule.Material / TypeDataModule` 往下追：

| 现象 | 出处 | 结论 |
|---|---|---|
| 加法粒子不写覆盖率 | `BasePassPixelShader.usf:1418` `half4(Color*Opacity, 0)` + `BasePassRendering.cpp:206` alpha `BF_Zero, BF_InverseSourceAlpha` | Additive 在 capture 里 cov 不变；EL 母材质 `M_RGBA_Suv1` / `M_Grayscale_Suv1` / `M_Aura` 均为 Additive |
| Bloom 不进 capture | `SceneCaptureRendering.cpp:503` PostProcessing=0；`DeferredShadingRenderer.cpp:2485` 在后处理前拷出 | 光晕在 bb 有、cap 无 |
| HDR 半透明 | tonemap 后 `peak(bb)` 可超 cov | 旧 `saturate(bb/cov)` 截断 |

修复（`h_render.cpp::covered`）：`a = max(cov, peak(bb))`，`s = bb/a`。普通混合恒有 `peak(bb) ≤ cov` → 不变；超出部分 `s·a ≡ bb` 光全保留，代价仅是这些像素 EM 背景被压暗 `peak − cov`。

已知缺口：`M_Yugami100`（`bUsesDistortion`，S5000 用）的扭曲只作用在黑摄影棚，EM 背景不会被扭曲，alpha 通道无法承载。

**排除项（别再查）**：
- Separate Translucency：PostProcessing=0 → `AllowTranslucencyAfterDOF()` false，全部半透明画进 SceneColor。
- "alpha 晚一帧"：曾推断 EngineTickPost 的 capture 排在 Present 之后——错。游戏模式下 Present 在 Slate `DrawWindows`（`LaunchEngineLoop.cpp:4944`，`GEngine->Tick` 之后），capture N 先于 Present N。另：Guest 里 `HUD:ReceiveDrawHUD` 实测从不触发，不能当挂点。

## 7. 可复用的检索路径速查

- **语义判定**（alpha/混合/渲染流程）：直接 grep `C:\Code2\UnrealEngine-4.26.2-release\Engine\{Source,Shaders}`——枚举注释、usf 写出、C++ clear/blend 三连看，比任何二手资料可靠。
- **类/属性存在性**（只读）：`.\tools\eval.ps1 -Role guest -Code "local ok,v = pcall(function() return obj.Prop end); return tostring(ok)..':'..tostring(v)"`，逐属性 pcall，不要链式取值（曾把 Guest 探崩）。
- **控件/贴图离线事实**：`tools\menuprobe`（控件树 + 动画绑定 + sprite 逐像素 alpha）。
- **类继承**：SDK 工程头文件 grep（`D:\CodeT\EnderLilies`、`D:\CodeT\EnderMagnolia`）。
- **拿不到的东西就 hook**：反射层只到 UObject；D3D11/RHI 对象用 vtable 钩子（本工程两处先例：swapchain Present、device CreateTexture2D）。
- **截图验证**：`tools\capture.ps1 -Name x -Role host|guest` → `<UserDir>\Saved\LIE\shots\`。
- **特效/材质资产事实**（离线）：cue4parse MCP `init_provider(EL Paks, GAME_UE4_26)` → `get_exports(PS_*)` 列发射器 → `get_property_path(...:ParticleModuleRequired_N, "Material")` → MI 的 `Parent` + `BasePropertyOverrides.BlendMode`、母材质 `BlendMode / bUsesDistortion`。MI 可覆盖混合模式，母材质不能代表实例。
- **帧时序推断**：必须追到 `FEngineLoop::Tick`（`LaunchEngineLoop.cpp`），只看 `UGameEngine::Tick` 会误判——游戏模式 Present 在 Slate 阶段。
- **新挂点先实测**：打算挂某个 UFunction 前，用 bridge `hook_function` + `poll_events` 确认它真的在触发，再写代码。
