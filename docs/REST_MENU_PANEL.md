# EL 休息菜单背板：为什么这一块不走图形学透传

> 结论：菜单的**内容**（文字/图标/特效）照旧走正常的帧透传抠像；只有**背板**（左侧 `BG` 面板 + 底部页脚横条）是纯黑 alpha 面，在导出帧里信息为零、无法抠出，所以改为 **Host 侧按 EL 原始资产数据重放**。
> 相关工具：`tools/menuprobe`（离线探针，构建与运行均不依赖）。
> 数据载体：`native/host/rest_panel_alpha.h`（已入库的生成物，编译进 MagnoliaHost）。

## 1. 正常透传路径（回顾）

Guest 在无头"黑摄影棚"里渲染 EL，Present 时把 backbuffer 拷进 DXGI 共享纹理；Host `ps_menu`/`ps_world` 对每个像素按亮度抠 alpha，再以 `ONE / INV_SRC_ALPHA` 预乘混合到 EM 世界上。

这个管线成立的前提：一个像素只要有可见贡献，它的预乘 RGB 就非零，alpha 可由亮度近似。

## 2. 背板为什么透传不出来

EL `WBP_RestMenu` 的 `BG` 是一个 `Image`（PaperSprite `T_Menu_RestPointTitle_Bg`）：**RGB 恒为黑，只有 alpha 渐变**。它的预乘贡献是 `黑 × alpha = 0`，叠在同样纯黑的摄影棚上——**画不画，导出帧逐字节相同**。backbuffer 的 alpha 通道也不可用：UI 已经在 EL 帧缓冲内混合完，"这一片被盖了 0.8"这个信息在拷贝时已经不存在。

同理，页脚 `WBP_MainMenu_Footer` 的 `FooterBG`（`T_Menu_Footer_Bg`）也是纯黑 + 纵向 alpha 渐变。

旧 TODO 里的备选方案"保留 BG 可见 + 换背景双帧求 alpha"没选：需要两帧差分、还要把面板/横条/内容三层 alpha 分离，脆且复杂；直接从 sprite 取 alpha 是精确值。

## 3. 重放方案（`native/host/h_render.cpp` `ps_menu`）

### 提取到的事实（CUE4Parse + 实测）

| 项 | 值 |
|---|---|
| `BG` 槽位 | `CanvasPanelSlot_4`，Anchors (0,0)→(0,1)，Offsets R=800，AutoSize → **左缘起、800 设计像素宽、全屏高** |
| `BG` sprite | `T_Menu_RestPointTitle_Bg`（Atlas-Game，源区域 800×8），alpha：**0.8（204/255）平台到 x=480，渐隐至 x=791 为 0** |
| 绘制顺序 | MouseBlocker → **BG** → VerticalBox 内容 → Footer：**面板在内容之下** |
| 页脚 | `WidgetSwitcher_BG` 索引 0 `FooterBG`（`T_Menu_Footer_Bg`，8×128），alpha 0→0.30 纵向渐变；锚定**底部 128 设计像素高、全宽**（Overlay autosize） |
| UI 缩放 | `ScaleToFit`（`DefaultEngine.ini`）：`min(w/1920, h/1080)`，1280×720 时为 2/3 → 面板物理宽 **533px**、横条高 **85px** |
| 动画 | `Show`/`Close`/`HideUI`/`HideTransition` 的 `MovieSceneColorTrack` 绑**根控件** `ColorAndOpacity`：整个菜单（含背板）随 `.A` 淡入淡出 |

### 实现

- `rest_panel_alpha.h`：800×2 的 8bit alpha LUT（行 0 = BG 横向曲线，行 1 = 页脚渐变重采样到 800）。启动时上传为 `R8_UNORM` 纹理（`t1`）。
- `ps_menu`（仅 `FrameKind::Split` 分支）：
  - `panel_u = 800 × min(fm.w/1920, fm.h/1080) / fm.w`，`footer_v = 1 - 128×scale/fm.h`——随实际帧尺寸自适应；
  - 背板 `dim = menu_alpha * (bg + ft*(1-bg))`（横条 over 面板），再 `a = key + dim*(1-key)`（背板在抠出的内容之下）——还原 EL 的图层栈而非 max 糊叠；
  - Split 世界矩形照旧 `discard`，面板不盖莉莉/英灵。
- `FrameMeta::menu_alpha`：Guest 每帧读可见菜单 `ColorAndOpacity.A` 发布（`g_body.cpp::publish_frame_kind` → `g_frame.cpp::stage_menu`），Host 用它乘背板 alpha——`Show` 淡入、`Close` 淡出、`C` 隐藏 UI 全程同步。
- Guest 侧 `BG` **保持 Collapsed**：反正黑叠黑无贡献，隐藏它让帧里的内容更干净。

### 为什么这样做是"对齐 EL"而不是近似

面板的几何、alpha 曲线、缩放规则、绘制顺序全部来自 EL 资产与实测，重放使用的也是 EL 自己的合成语义（预乘黑 → 只抬 alpha）。等效于"把拷贝时丢失的 alpha 通道用资产原值补回来"。

## 4. 相关工具 `tools/menuprobe`（离线，不进构建/运行）

用 CUE4Parse 直接读 EL pak，三种用法：

```powershell
# 贴图区域取像素（默认）：PaperSprite → 图集 → BC7 解码 → 裁区域 → PNG + 逐列/行 RGBA CSV
.\tools\menuprobe\run.ps1
# 控件树 / 动画绑定 dump（--tree 需配值，pair 解析）
dotnet run --project tools\menuprobe -c Release -p:CUE4ParseRoot=<CUE4Parse源码目录> -- --paks <EL>\Content\Paks --out tools\menuprobe\out --tree x
# 搜 pak 内文件路径
dotnet run ... --find RestMenu
```

- 配置：`devconfig.json` 的 `cue4parse`（CUE4Parse 源码路径，csproj 以 `-p:CUE4ParseRoot` 引用其工程）。
- 输出 `tools\menuprobe\out\` 已 gitignore（含游戏 ini，不入库）；`Detex.dll` 为运行时自解压的 BC 解码库。
- **再生成 LUT**：跑取数后把 `bg_restpanel.csv` 第 4 列 / `bg_footer.csv` 第 4 列（重采样到 800）写进 `rest_panel_alpha.h` 即可；数据只变过才会变。
