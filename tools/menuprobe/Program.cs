// Extracts the exact pixels of EL's rest-menu dimmer strips so the host-side ps_menu shader can
// reproduce them instead of using eyeballed smoothstep constants.
//   dotnet run --project tools/menuprobe -c Release -p:CUE4ParseRoot=... -- --paks <Content\Paks> --out <dir>
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Assets.Objects.Properties;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.UE4.Versions;
using CUE4Parse_Conversion.Textures;
using SkiaSharp;

var options = ParseArgs(args);
var paks = options.GetValueOrDefault("paks") ?? throw new ArgumentException("--paks is required");
var outDir = options.GetValueOrDefault("out") ?? throw new ArgumentException("--out is required");
Directory.CreateDirectory(outDir);

var provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, true, new VersionContainer(EGame.GAME_UE4_26));
provider.Initialize();
provider.Mount();

if (options.TryGetValue("find", out var needle))
{
    foreach (var f in provider.Files.Keys)
        if (f.Contains(needle, StringComparison.OrdinalIgnoreCase)) Console.WriteLine(f);
    return 0;
}

// Blueprint function bytecode as JSON: --funcs <package path> [--only Name1,Name2]
if (options.TryGetValue("funcs", out var funcPkg))
{
    provider.ReadScriptData = true;
    var only = options.GetValueOrDefault("only")?.Split(',').ToHashSet();
    var pkg = provider.LoadPackage(funcPkg) ?? throw new InvalidOperationException("pkg");
    foreach (var e in pkg.GetExports())
        if (e is UFunction f && (only == null || only.Contains(f.Name)))
            Console.WriteLine($"==== {f.Name}\n{Newtonsoft.Json.JsonConvert.SerializeObject(f.ScriptBytecode, Newtonsoft.Json.Formatting.Indented)}");
    return 0;
}

// BC7 atlases decode through the bundled Detex native lib (extracted next to the exe)
var detex = Path.Combine(AppContext.BaseDirectory, CUE4Parse_Conversion.Textures.BC.DetexHelper.DLL_NAME);
CUE4Parse_Conversion.Textures.BC.DetexHelper.LoadDll(detex);
CUE4Parse_Conversion.Textures.BC.DetexHelper.Initialize(detex);

// Widget tree of WBP_RestMenu: dump every export (name/class/outer) plus the layout fields we care about
// (CanvasPanelSlot Offsets/Anchors/Alignment/AutoSize/ZOrder, widget Visibility, Image brush resource).
if (options.ContainsKey("tree"))
{
    var pkgPath = options.GetValueOrDefault("tree") is { Length: > 1 } t ? t : "/Game/_Zenith/UI/Rest/WBP_RestMenu";
    var pkg = provider.LoadPackage(pkgPath) ?? throw new InvalidOperationException("pkg");
    for (var i = 0; i < pkg.ExportMapLength; ++i)
    {
        UObject? obj = null;
        try { obj = pkg.ExportsLazy[i].Value; } catch { }
        var cls = obj?.ExportType ?? "?";
        if (options.ContainsKey("props") && obj != null)
        {
            Console.WriteLine($"[{i}] {obj.Name} : {cls}");
            foreach (var p in obj.Properties)
                Console.WriteLine($"    {p.Name.Text} : {p.GetType().Name} tag={p.Tag}");
            continue;
        }
        var extra = "";
        if (obj != null)
        {
            var sb = new System.Text.StringBuilder();
            void Add(string k, object? v) { if (v != null) sb.Append($" {k}={v}"); }
            Add("vis", obj.GetOrDefault<string>("Visibility"));
            IPropertyHolder? layout = obj.GetOrDefault<FStructFallback?>("LayoutData", null);
            layout ??= obj;
            var anchors = layout.GetOrDefault<FStructFallback?>("Anchors", null);
            if (anchors != null)
                Add("anchors", $"{anchors.GetOrDefault<FVector2D>("Minimum")}->{anchors.GetOrDefault<FVector2D>("Maximum")}");
            var offs = layout.GetOrDefault<FStructFallback?>("Offsets", null);
            if (offs != null)
                Add("offs", $"L{offs.GetOrDefault<float>("Left")} T{offs.GetOrDefault<float>("Top")} R{offs.GetOrDefault<float>("Right")} B{offs.GetOrDefault<float>("Bottom")}");
            var align = layout.GetOrDefault<FVector2D?>("Alignment", null);
            Add("align", align.HasValue ? $"{align.Value.X},{align.Value.Y}" : null);
            Add("auto", obj.GetOrDefault<bool?>("bAutoSize", null));
            Add("z", obj.GetOrDefault<int?>("ZOrder", null));
            var brush = obj.GetOrDefault<FStructFallback?>("Brush", null);
            if (brush != null)
            {
                var res = brush.GetOrDefault<FPackageIndex?>("ResourceObject", null);
                var size = brush.GetOrDefault<FVector2D>("ImageSize");
                Add("brush", $"{res} drawAs={brush.GetOrDefault<byte>("DrawAs")} size={size.X}x{size.Y}");
            }
            var slotIdx = obj.GetOrDefault<FPackageIndex?>("Slot", null);
            if (slotIdx != null) Add("slot", $"#{slotIdx.Index - 1}");
            var content = obj.GetOrDefault<FPackageIndex?>("Content", null);
            if (content != null) Add("content", $"#{content.Index - 1}");
            // WidgetAnimation: which widget/property the track drives
            var alpha = obj.GetOrDefault<FScriptStruct?>("AlphaCurve", null);
            if (alpha?.StructType is CUE4Parse.UE4.Objects.MovieScene.FMovieSceneChannel<float> ch)
            {
                var times = string.Join(",", ch.Times.Select(t => t.Value.ToString()));
                var vals = string.Join(",", ch.Values.Select(v => v.Value.ToString("0.###")));
                Add("alphaCurve", $"t[{times}] v[{vals}]");
            }
            var pb = obj.GetOrDefault<FStructFallback?>("PropertyBinding", null);
            if (pb != null)
                Add("propbind", $"{pb.GetOrDefault<FName>("PropertyName").Text} path={pb.GetOrDefault<FName>("PropertyPath").Text}");
            var slots = obj.GetOrDefault<UScriptArray?>("Slots", null);
            if (slots != null)
                Add("slots", string.Join(",", slots.Properties.Select(p => (p as ObjectProperty)?.Value?.ResolvedObject?.Name.Text ?? p.ToString())));
            var binds = obj.GetOrDefault<UScriptArray?>("AnimationBindings", null);
            if (binds != null)
                foreach (var e in binds.Properties)
                {
                    var fb = (e as StructProperty)?.Value?.StructType as FStructFallback;
                    if (fb != null)
                        Add("bind", $"{fb.GetOrDefault<FName>("WidgetName").Text}.{fb.GetOrDefault<FName>("PropertyName").Text}");
                }
            extra = sb.ToString();
        }
        var outerName = obj?.Outer?.Name.Text ?? "?";
        Console.WriteLine($"[{i}] {obj?.Name} : {cls} outer={outerName}{extra}");
    }
    return 0;
}

// Packaged engine config: DPI scaling rule + design resolution
foreach (var cfg in new[] { "EnderLilies/Config/DefaultEngine.ini", "EnderLilies/Config/DefaultGame.ini" })
{
    if (!provider.Files.TryGetValue(cfg, out var file)) { Console.WriteLine($"missing {cfg}"); continue; }
    var text = System.Text.Encoding.UTF8.GetString(file.Read());
    File.WriteAllText(Path.Combine(outDir, Path.GetFileName(cfg)), text);
    foreach (var line in text.Split('\n'))
    {
        if (line.Contains("UIScale") || line.Contains("DPI") || line.Contains("DesignScreenSize") || line.Contains("UserInterface"))
            Console.WriteLine($"{Path.GetFileName(cfg)}: {line.Trim()}");
    }
}

// Sprite regions: rest-menu left panel + footer strips (both variants)
(string file, string spritePath)[] sprites =
[
    ("bg_restpanel", "/Game/_Zenith/UI/Textures/Atlas-Game/T_Menu_RestPointTitle_Bg.T_Menu_RestPointTitle_Bg"),
    ("bg_footer", "/Game/_Zenith/UI/Textures/Atlas-Common/T_Menu_Footer_Bg.T_Menu_Footer_Bg"),
    ("bg_footer_white", "/Game/_Zenith/UI/Textures/Atlas-Common/T_Menu_Footer_Bg_White.T_Menu_Footer_Bg_White"),
    ("header_underline", "/Game/_Zenith/UI/Textures/Atlas-Game/T_Menu_Title_Underline.T_Menu_Title_Underline"),
];
var atlases = new Dictionary<string, SKBitmap>();
foreach (var (name, path) in sprites)
{
    try
    {
        var sprite = provider.LoadPackageObject(path);
        var atlasRef = sprite.GetOrDefault<FPackageIndex>("BakedSourceTexture") ?? throw new InvalidOperationException("no atlas");
        var uv = sprite.GetOrDefault<FVector2D>("BakedSourceUV");
        var dim = sprite.GetOrDefault<FVector2D>("BakedSourceDimension");
        var key = atlasRef.ToString();
        if (!atlases.TryGetValue(key, out var atlas))
        {
            var tex = atlasRef.Load<UTexture2D>() ?? throw new InvalidOperationException("atlas not found");
            var decoded = tex.Decode() ?? throw new InvalidOperationException("atlas decode failed");
            atlas = decoded.ToSkBitmap();
            atlases[key] = atlas;
            Console.WriteLine($"atlas {key}: {atlas.Width}x{atlas.Height}");
        }
        var rect = SKRectI.Create((int)uv.X, (int)uv.Y, (int)dim.X, (int)dim.Y);
        using var crop = new SKBitmap(rect.Width, rect.Height);
        if (!atlas.ExtractSubset(crop, rect)) throw new InvalidOperationException($"bad rect {rect}");
        using (var image = SKImage.FromBitmap(crop))
        using (var data = image.Encode(SKEncodedImageFormat.Png, 100))
            File.WriteAllBytes(Path.Combine(outDir, $"{name}.png"), data.ToArray());
        Console.WriteLine($"{name}: region ({uv.X},{uv.Y}) {dim.X}x{dim.Y} -> {name}.png");

        // per-column (wide strips) or per-row (tall strips) RGBA dump
        var lines = new List<string>();
        if (rect.Width >= rect.Height)
        {
            for (var x = 0; x < rect.Width; ++x)
            {
                var p = crop.GetPixel(x, rect.Height / 2);
                lines.Add($"{x},{p.Red},{p.Green},{p.Blue},{p.Alpha}");
            }
        }
        else
        {
            for (var y = 0; y < rect.Height; ++y)
            {
                var p = crop.GetPixel(rect.Width / 2, y);
                lines.Add($"{y},{p.Red},{p.Green},{p.Blue},{p.Alpha}");
            }
        }
        File.WriteAllLines(Path.Combine(outDir, $"{name}.csv"), lines);

        // row-variance check for wide strips: is the middle column row-uniform?
        if (rect.Width >= rect.Height && rect.Height > 1)
        {
            var maxd = 0;
            for (var y = 0; y < rect.Height; ++y)
            for (var x = 0; x < rect.Width; ++x)
            {
                var a = crop.GetPixel(x, y);
                var b = crop.GetPixel(x, rect.Height / 2);
                maxd = Math.Max(maxd, Math.Abs(a.Alpha - b.Alpha));
            }
            Console.WriteLine($"{name}: max per-column alpha deviation vs mid row = {maxd}");
        }
    }
    catch (Exception e)
    {
        Console.WriteLine($"{name}: {e.Message}");
    }
}
return 0;

static Dictionary<string, string> ParseArgs(string[] args)
{
    var result = new Dictionary<string, string>();
    for (var i = 0; i + 1 < args.Length; i += 2)
    {
        if (!args[i].StartsWith("--")) throw new ArgumentException($"unexpected argument {args[i]}");
        result[args[i][2..]] = args[i + 1];
    }
    return result;
}
