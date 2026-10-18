// Host render thread (DXGI Present, EM must run with -dx11):
// - input ownership: builds the Lily input frame (single writer) and swallows EM input in LilyMenu / Overlay
//   (WndProc subclass + XInputGetState detour; the mod reads the pad through the trampoline)
// - world layer (FR-CMP-1): the EL frame is keyed into our render target, which EM samples through
//   MI_Spine_Character on a quad attached to Lilac (same sort layer as Lilac, under EM UI / fades / foreground)
// - LilyMenu: EL's native menu (Lily hidden in that frame) is keyed over the EM world
// - ImGui: F6 loadout panel, Lily HUD, F10 oracle panel, toasts

#include "host/host.h"
#include "host/rest_panel_alpha.h"

#include "core/el_items.h"
#include "core/facts.h"
#include "rt/clock.h"
#include "rt/d3d.h"
#include "rt/hook.h"
#include "rt/input.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <Xinput.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace lie::render {

namespace {

// True coverage alpha arrives on a second channel (chars = EL SceneCapture2D, SCS_SceneColorHDR ->
// premult RGB + inverse opacity in A). Over the black studio the back buffer is colour premultiplied
// by that same coverage, so straight colour = bb.rgb / cov and cov = 1 - chars.a exactly reproduces
// EL's composite. When the channel is absent (capture not armed yet) fall back to luminance keying:
// - a pixel whose 4 neighbours are all clearly lit is the interior of a sprite -> opaque (keeps dark detail)
// - everything else keeps alpha = its peak channel, which reproduces additive FX over black exactly when the
//   straight colour rgb/alpha is composited with that alpha
constexpr const char* kKeyHlsl = R"hlsl(
Texture2D frame : register(t0);
Texture2D chars : register(t1);   // SceneCapture RGBA16F: premult rgb, A = 1 - coverage
Texture2D panel_tex : register(t2); // kRestPanelAlpha: row 0 rest-menu BG, row 1 footer strip (design space)
SamplerState samp : register(s0);
cbuffer Key : register(b0) { float2 texel; float gain; float out_linear; float split_on; float panel_u; float menu_alpha; float footer_v; float chars_on; float4 split; };
bool in_split(float2 uv) { return split_on > 0.5 && all(uv >= split.xy) && all(uv <= split.zw); }
float peak(float3 c) { return max(c.r, max(c.g, c.b)); }
float3 px(float2 uv) { return frame.Sample(samp, uv).rgb; }
float4 keyed(float2 uv) {
    float3 c = px(uv);
    float n = min(min(peak(px(uv + float2(texel.x, 0))), peak(px(uv - float2(texel.x, 0)))),
                  min(peak(px(uv + float2(0, texel.y))), peak(px(uv - float2(0, texel.y)))));
    float solid = smoothstep(0.03, 0.09, n);
    float a = saturate(max(solid, peak(c)));
    float3 s = a > 1e-4 ? saturate(c / a) : float3(0, 0, 0);
    return float4(s, a);
}
float4 covered(float2 uv) {       // real alpha path: unpremultiply the back buffer by capture coverage
    float a = 1.0 - chars.Sample(samp, uv).a;
    float3 s = a > 1e-4 ? saturate(px(uv) / a) : float3(0, 0, 0);
    return float4(s, a);
}
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_world(VSOut i) : SV_Target {
    if (split_on > 0.5 && !in_split(i.uv)) return float4(0, 0, 0, 0); // Split: only the rect around Lily is world
    float4 k = chars_on > 0.5 ? covered(i.uv) : keyed(i.uv);
    float3 c = saturate(k.rgb * gain);
    if (out_linear > 0.5) c = pow(c, 2.2);
    return float4(c, k.a);
}
// EL menus over EM, blended ONE / INV_SRC_ALPHA. Every frame is composited on its own (no per-frame state, so
// nothing can flicker). Over the black studio the frame is the premultiplied UI; only its alpha is unknown.
// - full-screen menus (Menu): shown exactly as EL draws them, opaque (their own backdrop covers the screen)
// - rest menu (Split): EL's backdrop is the BG image - a 800 design px wide, full-height panel anchored
//   left whose sprite alpha is 0.8 flat to x=480 then fades to 0 by x=791 - plus a 128 design px tall
//   footer strip along the bottom. Both are black on black in the frame, so their alpha is restored
//   from the extracted sprites (menu_alpha is the menu's own ColorAndOpacity fade); the rest is keyed
float4 ps_menu(VSOut i) : SV_Target {
    if (in_split(i.uv)) discard;
    float3 c = frame.Sample(samp, i.uv).rgb;
    if (split_on < 0.5) return float4(c, 1.0);
    float key = saturate(max(smoothstep(0.03, 0.09, peak(c)), peak(c)));
    float bg = panel_tex.Sample(samp, float2(i.uv.x / panel_u, 0.25)).r;
    float ft = panel_tex.Sample(samp, float2((i.uv.y - footer_v) / max(1.0 - footer_v, 1e-5), 0.75)).r;
    float dim = menu_alpha * (bg + ft * (1.0 - bg)); // the strip is drawn on top of the panel
    float a = key + dim * (1.0 - key);              // both sit under the menu content
    return float4(min(c, a), a);
}
)hlsl";

struct alignas(16) KeyCB {
    float texel_x, texel_y, gain, out_linear;
    float split_on, panel_u, menu_alpha, footer_v;
    float chars_on, pad[3];
    float split[4];
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateTexFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using XInputFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
CreateTexFn g_create_tex = nullptr;
XInputFn g_xinput = nullptr;
WNDPROC g_wndproc = nullptr;

Toggles g_toggles;
std::atomic<ID3D11Texture2D*> g_world_tex{nullptr};
std::atomic<bool> g_world_active{false};
std::atomic<bool> g_overlay{false};
std::atomic<bool> g_swallow{false};
std::mutex g_equip_mutex;
std::vector<EquipPayload> g_equip;

struct Renderer {
    std::mutex m;
    bool ready = false, failed = false;
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* back_rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps_world = nullptr;
    ID3D11PixelShader* ps_menu = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* no_depth = nullptr;
    ID3D11BlendState* alpha_blend = nullptr;
    ID3D11RenderTargetView* world_rtv = nullptr;
    bool world_linear = false;
    std::uint64_t opened = 0;
    ID3D11Texture2D* guest_tex = nullptr;
    ID3D11ShaderResourceView* guest_srv = nullptr;
    std::uint64_t opened_chars = 0;
    ID3D11Texture2D* chars_tex = nullptr;
    ID3D11ShaderResourceView* chars_srv = nullptr;
    ID3D11Texture2D* panel_tex = nullptr;
    ID3D11ShaderResourceView* panel_srv = nullptr;
    UINT gw = 0, gh = 0;
    ImGuiContext* imgui = nullptr;
    input::FrameBuilder input;
    InputFrame frame{};
} g_r;

struct Toast {
    std::mutex m;
    std::string text;
    double until = 0.0;
} g_toast;

HRESULT STDMETHODCALLTYPE on_create_tex(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = g_create_tex(dev, d, init, out);
    if (SUCCEEDED(hr) && d && out && *out && d->Width == facts::kWorldRtWidth && d->Height == facts::kWorldRtHeight && (d->BindFlags & D3D11_BIND_RENDER_TARGET)) {
        (*out)->AddRef();
        if (ID3D11Texture2D* old = g_world_tex.exchange(*out)) old->Release();
        log::info(L"world-layer RT captured (format %d)", static_cast<int>(d->Format));
    }
    return hr;
}

DWORD WINAPI on_xinput(DWORD i, XINPUT_STATE* st) {
    if (g_swallow.load() && st) {
        std::memset(st, 0, sizeof(*st));
        return i == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    }
    return g_xinput(i, st);
}

void arm_xinput() {
    if (hook::attached("XInputGetState")) return;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        HMODULE m = GetModuleHandleW(dll);
        void* fn = m ? reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")) : nullptr;
        if (!fn) continue;
        if (hook::attach_at("XInputGetState", fn, reinterpret_cast<void*>(&on_xinput), reinterpret_cast<void**>(&g_xinput)))
            input::set_xinput(reinterpret_cast<input::XInputGetStateFn>(g_xinput));
        return;
    }
}

LRESULT CALLBACK on_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_swallow.load()) {
        switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR: case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
            return 0;
        case WM_INPUT: return DefWindowProcW(h, msg, wp, lp);
        default: break;
        }
    }
    return CallWindowProcW(g_wndproc, h, msg, wp, lp);
}

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    D3DCompile(kKeyHlsl, std::strlen(kKeyHlsl), "LilyInEM", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (err) {
        log::error(L"shader %hs: %hs", entry, static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    return blob;
}

bool create_back_rtv(IDXGISwapChain* swap) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const bool ok = SUCCEEDED(g_r.dev->CreateRenderTargetView(back, nullptr, &g_r.back_rtv));
    back->Release();
    return ok;
}

bool init(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    swap->GetDesc(&desc);
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_r.dev))) || !g_r.dev) {
        log::error(L"swap chain is not D3D11: launch EM with -dx11");
        return false;
    }
    g_r.dev->GetImmediateContext(&g_r.ctx);
    g_r.hwnd = desc.OutputWindow;
    void** vt = *reinterpret_cast<void***>(g_r.dev);
    hook::attach_at("D3D11.CreateTexture2D", vt[5], reinterpret_cast<void*>(&on_create_tex), reinterpret_cast<void**>(&g_create_tex));

    if (ID3DBlob* b = compile("vs_main", "vs_4_0")) { g_r.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_r.vs); b->Release(); }
    if (ID3DBlob* b = compile("ps_world", "ps_4_0")) { g_r.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_r.ps_world); b->Release(); }
    if (ID3DBlob* b = compile("ps_menu", "ps_4_0")) { g_r.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_r.ps_menu); b->Release(); }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(KeyCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_r.dev->CreateBuffer(&bd, nullptr, &g_r.cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_r.dev->CreateSamplerState(&sd, &g_r.sampler);
    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = kRestPanelAlphaW;
    pd.Height = 2;
    pd.MipLevels = pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_IMMUTABLE;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA pinit[2] = {{kRestPanelAlpha[0], kRestPanelAlphaW, 0}, {kRestPanelAlpha[1], kRestPanelAlphaW, 0}};
    if (SUCCEEDED(g_r.dev->CreateTexture2D(&pd, pinit, &g_r.panel_tex)))
        g_r.dev->CreateShaderResourceView(g_r.panel_tex, nullptr, &g_r.panel_srv);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    g_r.dev->CreateRasterizerState(&rd, &g_r.raster);
    D3D11_DEPTH_STENCIL_DESC dd{};
    g_r.dev->CreateDepthStencilState(&dd, &g_r.no_depth);
    D3D11_BLEND_DESC bl{};
    auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE; // ps_menu outputs premultiplied colour (the frame over the black studio)
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_r.dev->CreateBlendState(&bl, &g_r.alpha_blend);

    IMGUI_CHECKVERSION();
    g_r.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_r.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    else
        io.Fonts->AddFontDefault();
    if (!ImGui_ImplWin32_Init(g_r.hwnd) || !ImGui_ImplDX11_Init(g_r.dev, g_r.ctx) || !create_back_rtv(swap)) return false;
    g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_r.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&on_wndproc)));
    g_r.ready = true;
    log::info(L"renderer ready (%ux%u)", desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

ID3D11ShaderResourceView* guest_srv(const FrameMeta& fm) {
    if (!fm.shared_handle) return nullptr;
    if (fm.shared_handle != g_r.opened || !g_r.guest_srv) {
        d3d::release(g_r.guest_srv);
        d3d::release(g_r.guest_tex);
        g_r.opened = 0;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_r.dev->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(fm.shared_handle)), __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&tex))) || !tex)
            return nullptr;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        if (FAILED(g_r.dev->CreateShaderResourceView(tex, nullptr, &g_r.guest_srv))) {
            tex->Release();
            return nullptr;
        }
        g_r.guest_tex = tex;
        g_r.gw = td.Width;
        g_r.gh = td.Height;
        g_r.opened = fm.shared_handle;
    }
    return g_r.guest_srv;
}

ID3D11ShaderResourceView* chars_srv(const FrameMeta& fm) {
    if (!fm.chars_handle) return nullptr;
    if (fm.chars_handle != g_r.opened_chars || !g_r.chars_srv) {
        d3d::release(g_r.chars_srv);
        d3d::release(g_r.chars_tex);
        g_r.opened_chars = 0;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_r.dev->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(fm.chars_handle)), __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&tex))) || !tex)
            return nullptr;
        if (FAILED(g_r.dev->CreateShaderResourceView(tex, nullptr, &g_r.chars_srv))) {
            tex->Release();
            return nullptr;
        }
        g_r.chars_tex = tex;
        g_r.opened_chars = fm.chars_handle;
        log::info(L"scene-capture alpha channel open");
    }
    return g_r.chars_srv;
}

struct StateBackup {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4]{};
    UINT mask = 0, sref = 0;
    ID3D11DepthStencilState* ds = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    explicit StateBackup(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(1, &rtv, &dsv);
        c->RSGetViewports(&nvp, vp);
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &mask);
        c->OMGetDepthStencilState(&ds, &sref);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &smp);
        c->PSGetConstantBuffers(0, 1, &cb);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
    }
    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(1, &rtv, dsv);
        c->RSSetViewports(nvp, vp);
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, mask);
        c->OMSetDepthStencilState(ds, sref);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &smp);
        c->PSSetConstantBuffers(0, 1, &cb);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        for (IUnknown* u : std::initializer_list<IUnknown*>{rtv, dsv, rs, bs, ds, vs, ps, gs, srv, smp, cb, il}) if (u) u->Release();
    }
};

// Full-screen triangle with one of our pixel shaders into `target`
void blit(ID3D11RenderTargetView* target, UINT w, UINT h, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, ID3D11ShaderResourceView* chars,
          const KeyCB& cb, bool blend) {
    auto* c = g_r.ctx;
    StateBackup bk(c);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    c->OMSetRenderTargets(1, &target, nullptr);
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_r.raster);
    c->OMSetBlendState(blend ? g_r.alpha_blend : nullptr, nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(g_r.no_depth, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->UpdateSubresource(g_r.cb, 0, nullptr, &cb, 0, 0);
    c->VSSetShader(g_r.vs, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_r.cb);
    c->PSSetShaderResources(0, 1, &src);
    c->PSSetShaderResources(1, 1, &chars);
    c->PSSetShaderResources(2, 1, &g_r.panel_srv);
    c->PSSetSamplers(0, 1, &g_r.sampler);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    bk.restore(c);
}

bool ensure_world_rtv() {
    ID3D11Texture2D* tex = g_world_tex.load();
    if (!tex) return false;
    if (g_r.world_rtv) return true;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    switch (td.Format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break; // RTF_RGBA8_SRGB storage: write encoded values
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rd.Format = td.Format; g_r.world_linear = true; break;
    default: rd.Format = td.Format; break;
    }
    return SUCCEEDED(g_r.dev->CreateRenderTargetView(tex, &rd, &g_r.world_rtv));
}

bool edge(int vk, bool& was) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool e = down && !was;
    was = down;
    return e;
}

Ctx host_ctx(const Control* c) {
    HostFlags f{};
    return c->host_flags.read(f) ? static_cast<Ctx>(f.ctx) : Ctx::Inactive;
}

void poll_input(Control* c, bool focused) {
    static bool f5 = false, f6 = false, f7 = false, f8 = false, f10 = false, esc = false;
    const Ctx ctx = host_ctx(c);
    if (focused) {
        if (edge(VK_F5, f5) && ctx == Ctx::Gameplay) ++g_toggles.pause_requests;
        if (edge(VK_F6, f6) && (ctx == Ctx::Gameplay || g_overlay.load())) g_overlay = !g_overlay.load();
        if (edge(VK_F7, f7)) g_toggles.bridge_on = !g_toggles.bridge_on.load();
        if (edge(VK_F8, f8)) g_toggles.show_lilac = !g_toggles.show_lilac.load();
        if (edge(VK_F10, f10)) g_toggles.debug_hud = !g_toggles.debug_hud.load();
        if (edge(VK_ESCAPE, esc) && g_overlay.load()) g_overlay = false;
    }
    const bool to_lily = ctx == Ctx::Gameplay || ctx == Ctx::LilyMenu;
    g_r.input.build(g_r.frame, focused && to_lily);
    if (ctx == Ctx::LilyMenu && focused) {
        // menu keys that are not Lily actions: Esc / Backspace cancel, Enter confirm, arrows navigate
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_ESCAPE) || down(VK_BACK)) g_r.frame.raw |= 1u << cmd::Dodge;
        if (down(VK_RETURN)) g_r.frame.raw |= 1u << cmd::Jump;
        if (down(VK_UP)) g_r.frame.y = 1.0f;
        if (down(VK_DOWN)) g_r.frame.y = -1.0f;
        if (down(VK_LEFT)) g_r.frame.x = -1.0f;
        if (down(VK_RIGHT)) g_r.frame.x = 1.0f;
    }
    c->input.store(g_r.frame);
    g_swallow = ctx == Ctx::LilyMenu || ctx == Ctx::Overlay || g_overlay.load();
}

void queue_equip(EquipOp op, int set, int slot, const char* id) {
    EquipPayload e;
    e.op = op;
    e.set = set;
    e.slot = slot;
    std::snprintf(e.id, sizeof(e.id), "%s", id ? id : "");
    std::scoped_lock lk(g_equip_mutex);
    g_equip.push_back(e);
}

const char* spirit_name(const char* id) {
    if (!id || !id[0]) return "(空 Empty)";
    for (const auto& s : el_items::kSpirits) {
        if (std::strcmp(s.id, id) == 0) return s.name;
    }
    return id;
}

void draw_panel(const Vitals& v, const ImVec2& display) {
    static int sel_set = 1, sel_slot = 0;
    constexpr int kSlotCmd[3] = {cmd::Attack, cmd::Sub1, cmd::Sub2};
    constexpr const char* kSlotName[3] = {"主技能 H/X", "副技能1 J/Y", "副技能2 K/B"};
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.0f, display.x - 40.0f), std::min(620.0f, display.y - 40.0f)), ImGuiCond_Always);
    bool open = true;
    if (ImGui::Begin("莉莉 精灵与遗物 Lily Spirits & Relics  [F6 / Esc]###lie_panel", &open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::Button("解锁全部并回满 Unlock all & restore")) queue_equip(EquipOp::UnlockAll, 1, 0, "");
        ImGui::SameLine();
        if (ImGui::Button("切换精灵组 Switch set")) queue_equip(EquipOp::SwitchSet, v.summon_set == 1 ? 2 : 1, 0, "");
        ImGui::Separator();
        ImGui::Columns(2, "cols", true);
        for (int set = 1; set <= 2; ++set) {
            ImGui::Text("精灵组 Set %c%s", set == 1 ? 'A' : 'B', v.summon_set == set ? "  (当前 current)" : "");
            for (int s = 0; s < 3; ++s) {
                char label[160];
                std::snprintf(label, sizeof(label), "%s %s: %s###s%d%d", (sel_set == set && sel_slot == s) ? ">>" : "  ", kSlotName[s], spirit_name(v.spirits[set - 1][s]), set, s);
                if (ImGui::Button(label, ImVec2(-80.0f, 0.0f))) sel_set = set, sel_slot = s;
                ImGui::SameLine();
                char un[32];
                std::snprintf(un, sizeof(un), "卸下###u%d%d", set, s);
                if (ImGui::Button(un, ImVec2(70.0f, 0.0f))) queue_equip(EquipOp::UnequipSpirit, set, kSlotCmd[s], "");
            }
        }
        ImGui::BeginChild("spirits", ImVec2(0, 0), true);
        for (const auto& s : el_items::kSpirits) {
            char item[160];
            std::snprintf(item, sizeof(item), "[%s] %s###sp%s", s.main ? "主" : "副", s.name, s.id);
            if (ImGui::Selectable(item)) queue_equip(EquipOp::EquipSpirit, sel_set, kSlotCmd[sel_slot], s.id);
        }
        ImGui::EndChild();
        ImGui::NextColumn();
        ImGui::Text("遗物 Relics %d / %d", v.passive_used, v.passive_max);
        ImGui::BeginChild("relics", ImVec2(0, 0), true);
        for (const auto& p : el_items::kPassives) {
            const bool on = (v.passives_mask >> p.bit) & 1ull;
            char item[160];
            std::snprintf(item, sizeof(item), "%s %s###p%s", on ? "[ON ]" : "[OFF]", p.name, p.id);
            if (ImGui::Selectable(item, on)) queue_equip(EquipOp::TogglePassive, 0, 0, p.id);
        }
        ImGui::EndChild();
        ImGui::Columns(1);
    }
    ImGui::End();
    if (!open) g_overlay = false;
}

void draw_hud(const Vitals& v, const ImVec2& display) {
    auto* dl = ImGui::GetBackgroundDrawList();
    const float s = display.y / 720.0f;
    const ImVec2 o(40.0f * s, 40.0f * s);
    const float w = 220.0f * s, h = 12.0f * s;
    const float hp = v.max_hp > 0 ? std::clamp(float(v.hp) / float(v.max_hp), 0.0f, 1.0f) : 0.0f;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(20, 18, 22, 190), 3.0f);
    dl->AddRectFilled(o, ImVec2(o.x + w * hp, o.y + h), IM_COL32(222, 216, 200, 240), 3.0f);
    char txt[48];
    std::snprintf(txt, sizeof(txt), "%d / %d", v.hp, v.max_hp);
    dl->AddText(ImVec2(o.x + w + 8.0f * s, o.y - 4.0f * s), IM_COL32(230, 224, 210, 240), txt);
    for (int i = 0; i < std::min(v.max_heals, 12); ++i) { // prayers
        const ImVec2 c(o.x + 8.0f * s + i * 18.0f * s, o.y + h + 14.0f * s);
        dl->AddNgonFilled(c, 6.5f * s, i < v.heals ? IM_COL32(236, 214, 150, 235) : IM_COL32(70, 66, 60, 170), 4);
    }
    if (v.special_unlocked && v.max_mp > 0) { // special gauge (two segments with Ricorus' Ring)
        const ImVec2 b(o.x, o.y + h + 28.0f * s);
        const float r = std::clamp(float(v.mp) / float(v.max_mp), 0.0f, 1.0f);
        dl->AddRectFilled(b, ImVec2(b.x + w * 0.7f, b.y + 5.0f * s), IM_COL32(15, 15, 20, 170), 2.0f);
        dl->AddRectFilled(b, ImVec2(b.x + w * 0.7f * r, b.y + 5.0f * s), IM_COL32(150, 110, 230, 235), 2.0f);
    }
    const ImVec2 sp(40.0f * s, display.y - 70.0f * s); // spirit set (bottom left)
    for (int k = 0; k < 3; ++k) {
        const char* id = v.spirits[std::clamp(v.summon_set, 1, 2) - 1][k];
        std::snprintf(txt, sizeof(txt), "%s", id[0] ? id : "-");
        dl->AddRectFilled(ImVec2(sp.x + k * 64.0f * s, sp.y), ImVec2(sp.x + k * 64.0f * s + 56.0f * s, sp.y + 28.0f * s), IM_COL32(18, 16, 20, 170), 4.0f);
        dl->AddText(ImVec2(sp.x + k * 64.0f * s + 6.0f * s, sp.y + 5.0f * s), IM_COL32(230, 220, 200, 230), txt);
    }
}

void draw_debug(Control* c, const ImVec2& display, bool guest_fresh) {
    HostCounters hk{};
    GuestCounters gk{};
    GuestStatus gs{};
    c->host_counters.read(hk);
    c->guest_counters.read(gk);
    c->guest_status.read(gs);
    ImGui::SetNextWindowPos(ImVec2(display.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    if (ImGui::Begin("LilyInEM", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav)) {
        static const char* kCtx[] = {"Gameplay", "EmMenu", "LilyMenu", "Overlay", "Inactive"};
        ImGui::Text("F5 莉莉菜单 | F6 装配 | F7 桥接 %s | F8 莱拉克 | F10 调试", g_toggles.bridge_on ? "ON" : "OFF");
        ImGui::Text("ctx=%s guest=%s ready=%u layer=%s", kCtx[std::min<std::uint32_t>(static_cast<std::uint32_t>(host_ctx(c)), 4)], guest_fresh ? "OK" : "WAIT", gs.ready,
                    g_world_active ? "WORLD" : "off");
        ImGui::Text("puppet frames=%u err_max=%.4f | terrain r%u rects=%u p99=%.3fms | EL boxes=%u", hk.puppet_frames, hk.puppet_err_max, hk.terrain_revision,
                    hk.terrain_rects, hk.terrain_ms_p99, gk.terrain_boxes);
        ImGui::Text("hurt: EM intercepted %u / kb %u | EL received %u replayed %u kb %u | lethal %u | HP fixes %u", hk.dmg_intercepted, hk.kb_intercepted,
                    gk.hurt_received, gk.hurt_replayed, gk.knockback_replayed, gk.lethal_sent, hk.lilac_hp_corrections);
        ImGui::Text("hits: EL proxy %u events %u | EM applied %u damage %u | falling frames %u rebases %u", gk.proxy_hits, gk.hit_events, hk.hit_events_applied,
                    hk.enemy_damage_calls, gk.falling_frames, gk.rebases);
    }
    ImGui::End();
}

void render_frame(IDXGISwapChain* swap) {
    if (g_r.failed) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return;
    if (g_r.ready ? desc.OutputWindow != g_r.hwnd : !d3d::is_game_window(desc.OutputWindow)) return;
    if (!g_r.ready && !init(swap)) {
        g_r.failed = true;
        return;
    }
    if (!g_r.back_rtv && !create_back_rtv(swap)) return;
    arm_xinput();
    Control* c = shm::control();
    if (!c) return;
    const bool focused = GetForegroundWindow() == g_r.hwnd;
    poll_input(c, focused);
    const Ctx ctx = host_ctx(c);

    FrameMeta fm{};
    const bool guest_fresh = c->frame.read(fm) && heartbeat_alive(fm.frame_ms, clock::now_ms(), facts::kHeartbeatTimeoutMs);
    ID3D11ShaderResourceView* srv = (guest_fresh && g_toggles.bridge_on) ? guest_srv(fm) : nullptr;
    ID3D11ShaderResourceView* cap = (guest_fresh && g_toggles.bridge_on) ? chars_srv(fm) : nullptr;
    KeyCB cb{};
    if (srv) {
        cb.texel_x = 1.0f / static_cast<float>(g_r.gw);
        cb.texel_y = 1.0f / static_cast<float>(g_r.gh);
        cb.gain = g_toggles.quad_gain.load() > 0.05f ? g_toggles.quad_gain.load() : 1.0f;
        cb.chars_on = cap ? 1.0f : 0.0f;
        // FrameKind::Menu has Lily hidden: the world layer keeps its last Lily and the menu goes on top. Split keeps
        // her live (the rect around her feeds the world layer, the rest is the menu). After a Menu frame the world
        // layer waits one more frame, in case EL's content trails its flag (no empty Lily).
        const auto kind = static_cast<FrameKind>(fm.menu_frame);
        static std::uint64_t last_seq = 0;
        static int since_menu = 2;
        if (fm.frame_seq != last_seq) {
            last_seq = fm.frame_seq;
            since_menu = kind == FrameKind::Menu ? 0 : std::min(since_menu + 1, 2);
        }
        if (kind == FrameKind::Split) {
            cb.split_on = 1.0f;
            std::memcpy(cb.split, fm.split, sizeof(cb.split));
            // EL UI is ScaleToFit on a 1920x1080 design: BG is 800 design px wide, the footer strip 128 tall
            const float ui = std::min(fm.width / 1920.0f, fm.height / 1080.0f);
            cb.panel_u = fm.width > 0 ? 800.0f * ui / fm.width : 0.0f;
            cb.menu_alpha = std::clamp(fm.menu_alpha, 0.0f, 1.0f);
            cb.footer_v = fm.height > 0 ? 1.0f - 128.0f * ui / fm.height : 1.0f;
        }
        if (g_world_active && since_menu >= 2 && g_r.ps_world && ensure_world_rtv()) {
            cb.out_linear = g_r.world_linear ? 1.0f : 0.0f;
            blit(g_r.world_rtv, facts::kWorldRtWidth, facts::kWorldRtHeight, g_r.ps_world, srv, cap, cb, false);
        }
        if (kind != FrameKind::World && g_r.ps_menu) blit(g_r.back_rtv, desc.BufferDesc.Width, desc.BufferDesc.Height, g_r.ps_menu, srv, cap, cb, true);
    }

    ImGui::SetCurrentContext(g_r.imgui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    const bool overlay = g_overlay.load();
    if (overlay) {
        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(g_r.hwnd, &pt)) io.MousePos = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
        io.MouseDown[0] = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }
    io.MouseDrawCursor = overlay;
    ImGui::NewFrame();
    Vitals v{};
    c->vitals.read(v);
    if (overlay) draw_panel(v, io.DisplaySize);
    if (ctx == Ctx::Gameplay && g_world_active) draw_hud(v, io.DisplaySize);
    {
        std::scoped_lock lk(g_toast.m);
        if (!g_toast.text.empty() && clock::now_s() < g_toast.until) {
            const ImVec2 sz = ImGui::CalcTextSize(g_toast.text.c_str());
            const ImVec2 p((io.DisplaySize.x - sz.x) * 0.5f, io.DisplaySize.y * 0.12f);
            auto* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(p.x - 16, p.y - 8), ImVec2(p.x + sz.x + 16, p.y + sz.y + 8), IM_COL32(10, 10, 14, 170), 6.0f);
            fg->AddText(p, IM_COL32(236, 226, 205, 255), g_toast.text.c_str());
        }
    }
    if (g_toggles.debug_hud) draw_debug(c, io.DisplaySize, guest_fresh);
    ImGui::Render();
    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_r.ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g_r.ctx->OMSetRenderTargets(1, &g_r.back_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_r.ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    d3d::release(old_rtv);
    d3d::release(old_dsv);
}

HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    {
        std::scoped_lock lk(g_r.m);
        try {
            render_frame(swap);
        } catch (...) {
            g_r.failed = true;
        }
    }
    return g_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE on_resize(IDXGISwapChain* swap, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    {
        std::scoped_lock lk(g_r.m);
        d3d::release(g_r.back_rtv);
    }
    return g_resize(swap, n, w, h, f, flags);
}

} // namespace

Toggles& toggles() { return g_toggles; }

std::vector<EquipPayload> take_equip_commands() {
    std::scoped_lock lk(g_equip_mutex);
    std::vector<EquipPayload> out;
    out.swap(g_equip);
    return out;
}

void install() {
    if (hook::attached("DXGI.Present")) return;
    void* present = nullptr;
    void* resize = nullptr;
    if (!d3d::swapchain_entries(present, resize)) return;
    hook::attach_at("DXGI.Present", present, reinterpret_cast<void*>(&on_present), reinterpret_cast<void**>(&g_present));
    hook::attach_at("DXGI.ResizeBuffers", resize, reinterpret_cast<void*>(&on_resize), reinterpret_cast<void**>(&g_resize));
}

void shutdown() {
    std::scoped_lock lk(g_r.m);
    if (g_wndproc && g_r.hwnd) SetWindowLongPtrW(g_r.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_wndproc));
    if (g_r.ready) {
        ImGui::SetCurrentContext(g_r.imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_r.imgui);
        g_r.ready = false;
    }
    if (ID3D11Texture2D* t = g_world_tex.exchange(nullptr)) t->Release();
    for (IUnknown** p : std::initializer_list<IUnknown**>{
             reinterpret_cast<IUnknown**>(&g_r.world_rtv), reinterpret_cast<IUnknown**>(&g_r.back_rtv), reinterpret_cast<IUnknown**>(&g_r.guest_srv),
             reinterpret_cast<IUnknown**>(&g_r.guest_tex), reinterpret_cast<IUnknown**>(&g_r.chars_srv), reinterpret_cast<IUnknown**>(&g_r.chars_tex),
             reinterpret_cast<IUnknown**>(&g_r.panel_srv), reinterpret_cast<IUnknown**>(&g_r.panel_tex),
             reinterpret_cast<IUnknown**>(&g_r.vs), reinterpret_cast<IUnknown**>(&g_r.ps_world),
             reinterpret_cast<IUnknown**>(&g_r.ps_menu), reinterpret_cast<IUnknown**>(&g_r.cb), reinterpret_cast<IUnknown**>(&g_r.sampler),
             reinterpret_cast<IUnknown**>(&g_r.raster), reinterpret_cast<IUnknown**>(&g_r.no_depth), reinterpret_cast<IUnknown**>(&g_r.alpha_blend),
             reinterpret_cast<IUnknown**>(&g_r.ctx), reinterpret_cast<IUnknown**>(&g_r.dev)}) {
        if (*p) {
            (*p)->Release();
            *p = nullptr;
        }
    }
}

bool world_rt_captured() { return g_world_tex.load() != nullptr; }
void set_world_layer(bool active) { g_world_active = active; }
bool overlay_open() { return g_overlay.load(); }

void toast(const char* text) {
    std::scoped_lock lk(g_toast.m);
    g_toast.text = text ? text : "";
    g_toast.until = clock::now_s() + 2.5;
}

} // namespace lie::render
