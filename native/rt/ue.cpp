#include "rt/ue.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FAssetData.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/World.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace lie::ue {

using namespace RC::Unreal;

namespace {

struct Key {
    const void* owner;
    std::wstring name;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept { return std::hash<const void*>{}(k.owner) * 31u ^ std::hash<std::wstring>{}(k.name); }
};

std::unordered_map<Key, FProperty*, KeyHash> g_props;
std::unordered_map<Key, UFunction*, KeyHash> g_fns;
std::unordered_map<std::wstring, Weak> g_paths;

std::int64_t read_int(const void* p, std::int32_t size) {
    switch (size) {
    case 1: return *static_cast<const std::uint8_t*>(p);
    case 2: return *static_cast<const std::int16_t*>(p);
    case 4: return *static_cast<const std::int32_t*>(p);
    case 8: return *static_cast<const std::int64_t*>(p);
    default: return 0;
    }
}

void write_int(void* p, std::int32_t size, std::int64_t v) {
    switch (size) {
    case 1: *static_cast<std::uint8_t*>(p) = static_cast<std::uint8_t>(v); break;
    case 2: *static_cast<std::int16_t*>(p) = static_cast<std::int16_t>(v); break;
    case 4: *static_cast<std::int32_t*>(p) = static_cast<std::int32_t>(v); break;
    case 8: *static_cast<std::int64_t*>(p) = v; break;
    default: break;
    }
}

// Writes `count` components as double when the reflected struct is large enough, float otherwise.
void write_components(void* dst, std::int32_t size, const double* src, int count) {
    if (size >= count * 8) {
        auto* d = static_cast<double*>(dst);
        for (int k = 0; k < count; ++k) d[k] = src[k];
    } else {
        auto* f = static_cast<float*>(dst);
        for (int k = 0; k < count && (k + 1) * 4 <= size; ++k) f[k] = static_cast<float>(src[k]);
    }
}

// FRotator -> FQuat (UE convention, degrees)
void rot_to_quat(const Rot& r, double q[4]) {
    constexpr double d2r = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(r.pitch * d2r * 0.5), cp = std::cos(r.pitch * d2r * 0.5);
    const double sy = std::sin(r.yaw * d2r * 0.5), cy = std::cos(r.yaw * d2r * 0.5);
    const double sr = std::sin(r.roll * d2r * 0.5), cr = std::cos(r.roll * d2r * 0.5);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

} // namespace

// ---------------- objects ----------------

bool alive(const UObject* o) noexcept {
    if (!o) return false;
    auto* m = const_cast<UObject*>(o);
    return !m->IsUnreachable() && !m->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
}

UObject* find(std::wstring_view path) {
    std::wstring key(path);
    if (auto it = g_paths.find(key); it != g_paths.end()) {
        if (UObject* o = it->second.get()) return o;
    }
    UObject* o = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, key.c_str());
    if (o) g_paths[key].set(o);
    return o;
}

UClass* find_class(std::wstring_view path) { return static_cast<UClass*>(find(path)); }

UObject* load_asset(std::wstring_view path) {
    if (UObject* o = find(path)) return o;
    const std::wstring key(path);
    auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
    if (!registry) return nullptr;
    FAssetData data = registry->GetAssetByObjectPath(FName(key.c_str(), FNAME_Add));
    if (!data.PackageName().GetComparisonIndex() && !(Version::IsAtMost(5, 0) && data.ObjectPath().GetComparisonIndex())) return nullptr;
    return UAssetRegistryHelpers::GetAsset(data);
}

UClass* load_class(std::wstring_view class_path) {
    if (UClass* c = find_class(class_path)) return c;
    std::wstring asset(class_path);
    if (asset.size() > 2 && asset.ends_with(L"_C")) asset.resize(asset.size() - 2);
    load_asset(asset);
    return find_class(class_path);
}

UObject* first(const wchar_t* short_class_name) { return UObjectGlobals::FindFirstOf(short_class_name); }

std::wstring class_name(const UObject* o) {
    if (!o) return {};
    UClass* c = const_cast<UObject*>(o)->GetClassPrivate();
    return c ? c->GetName() : std::wstring{};
}

bool class_contains(const UObject* o, std::wstring_view fragment) { return class_name(o).find(fragment) != std::wstring::npos; }

bool is_a(const UObject* o, UClass* cls) { return o && cls && o->IsA(cls); }

UObject* world_of(UObject* o) { return o ? static_cast<UObject*>(o->GetWorld()) : nullptr; }

FName fname(const wchar_t* text) { return FName(text, FNAME_Add); }

std::wstring str(const FName& n) { return const_cast<FName&>(n).ToString(); }

std::wstring read_fstring(const void* v) {
    auto* a = static_cast<const RawArray*>(v);
    if (!a || !a->data || a->num <= 1) return {};
    return std::wstring(static_cast<const wchar_t*>(a->data), static_cast<std::size_t>(a->num - 1));
}

// ---------------- properties ----------------

FProperty* prop(UStruct* type, std::wstring_view name) {
    if (!type) return nullptr;
    Key key{type, std::wstring(name)};
    if (auto it = g_props.find(key); it != g_props.end()) return it->second;
    FProperty* found = nullptr;
    const auto flags = static_cast<EFieldIterationFlags>(static_cast<uint8>(EFieldIterationFlags::IncludeSuper) |
                                                         static_cast<uint8>(EFieldIterationFlags::IncludeDeprecated));
    for (FProperty* p : TFieldRange<FProperty>(type, flags)) {
        if (p && p->GetName() == name) {
            found = p;
            break;
        }
    }
    g_props.emplace(std::move(key), found);
    return found;
}

FProperty* member_of(FProperty* struct_prop, std::wstring_view field) {
    auto* sp = CastField<FStructProperty>(struct_prop);
    UScriptStruct* st = sp ? sp->GetStruct().Get() : nullptr;
    return st ? prop(st, field) : nullptr;
}

FProperty* array_inner(FProperty* array_prop) {
    auto* ap = CastField<FArrayProperty>(array_prop);
    return ap ? ap->GetInner() : nullptr;
}

UObject* weak_get(const void* w) {
    if (!w) return nullptr;
    UObject* o = static_cast<const FWeakObjectPtr*>(w)->Get();
    return alive(o) ? o : nullptr;
}

void* field_ptr(UObject* o, std::wstring_view name) {
    if (!alive(o)) return nullptr;
    FProperty* p = prop(o->GetClassPrivate(), name);
    return p ? reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal() : nullptr;
}

UObject* obj(UObject* owner, std::wstring_view name) {
    auto** slot = field<UObject*>(owner, name);
    return (slot && alive(*slot)) ? *slot : nullptr;
}

double read_num(FProperty* p, const void* v) {
    if (!p || !v) return 0.0;
    if (auto* bp = CastField<FBoolProperty>(p)) return bp->GetPropertyValue(v) ? 1.0 : 0.0;
    if (CastField<FFloatProperty>(p)) return *static_cast<const float*>(v);
    if (CastField<FDoubleProperty>(p)) return *static_cast<const double*>(v);
    if (auto* ep = CastField<FEnumProperty>(p)) {
        FNumericProperty* u = ep->GetUnderlyingProperty();
        return static_cast<double>(read_int(v, u ? u->GetElementSize() : 1));
    }
    return static_cast<double>(read_int(v, p->GetElementSize()));
}

void write_num(FProperty* p, void* v, double value) {
    if (!p || !v) return;
    if (auto* bp = CastField<FBoolProperty>(p)) bp->SetPropertyValue(v, value != 0.0);
    else if (CastField<FFloatProperty>(p)) *static_cast<float*>(v) = static_cast<float>(value);
    else if (CastField<FDoubleProperty>(p)) *static_cast<double*>(v) = value;
    else if (auto* ep = CastField<FEnumProperty>(p)) {
        FNumericProperty* u = ep->GetUnderlyingProperty();
        write_int(v, u ? u->GetElementSize() : 1, static_cast<std::int64_t>(value));
    } else write_int(v, p->GetElementSize(), static_cast<std::int64_t>(std::llround(value)));
}

V3 read_vec(const void* v, std::int32_t size) {
    if (!v) return {};
    if (size >= 24) {
        auto* d = static_cast<const double*>(v);
        return {d[0], d[1], d[2]};
    }
    auto* f = static_cast<const float*>(v);
    return {f[0], f[1], f[2]};
}

void write_vec(void* v, std::int32_t size, const V3& value) {
    const double c[3]{value.x, value.y, value.z};
    if (v) write_components(v, size, c, 3);
}

double num(UObject* o, std::wstring_view name, double fallback) {
    if (!alive(o)) return fallback;
    FProperty* p = prop(o->GetClassPrivate(), name);
    return p ? read_num(p, reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal()) : fallback;
}

bool set_num(UObject* o, std::wstring_view name, double v) {
    if (!alive(o)) return false;
    FProperty* p = prop(o->GetClassPrivate(), name);
    if (!p) return false;
    write_num(p, reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal(), v);
    return true;
}

bool flag(UObject* o, std::wstring_view name, bool fallback) { return num(o, name, fallback ? 1.0 : 0.0) != 0.0; }
bool set_flag(UObject* o, std::wstring_view name, bool v) { return set_num(o, name, v ? 1.0 : 0.0); }

V3 vec(UObject* o, std::wstring_view name) {
    if (!alive(o)) return {};
    FProperty* p = prop(o->GetClassPrivate(), name);
    return p ? read_vec(reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal(), p->GetSize()) : V3{};
}

bool set_vec(UObject* o, std::wstring_view name, const V3& v) {
    if (!alive(o)) return false;
    FProperty* p = prop(o->GetClassPrivate(), name);
    if (!p) return false;
    write_vec(reinterpret_cast<std::uint8_t*>(o) + p->GetOffset_Internal(), p->GetSize(), v);
    return true;
}

UFunction* function(UObject* o, std::wstring_view name) {
    if (!alive(o)) return nullptr;
    UClass* cls = o->GetClassPrivate();
    Key key{cls, std::wstring(name)};
    if (auto it = g_fns.find(key); it != g_fns.end()) return it->second;
    UFunction* f = o->GetFunctionByNameInChain(key.name.c_str());
    g_fns.emplace(std::move(key), f);
    return f;
}

bool map_visit(void* map_ptr, FProperty* map_prop, MapVisitor visit, void* ctx) {
    auto* mp = CastField<FMapProperty>(map_prop);
    if (!mp || !map_ptr) return false;
    auto* map = static_cast<FScriptMap*>(map_ptr);
    const FScriptMapLayout& layout = mp->GetMapLayout();
    for (int32 i = 0, n = map->GetMaxIndex(); i < n; ++i) {
        if (!map->IsValidIndex(i)) continue;
        auto* pair = static_cast<std::uint8_t*>(map->GetData(i, layout));
        visit(ctx, pair, pair + layout.ValueOffset, mp->GetKeyProp(), mp->GetValueProp());
    }
    return true;
}

UObject* Weak::get() const {
    UObject* o = ptr_.Get();
    return alive(o) ? o : nullptr;
}

void Weak::set(UObject* o) { ptr_ = FWeakObjectPtr(o); }

// ---------------- Call ----------------

Call::Call(UObject* self, std::wstring_view function_name) : self_(self), fn_(function(self, function_name)) { init(); }
Call::Call(UObject* self, UFunction* fn) : self_(self), fn_(alive(self) ? fn : nullptr) { init(); }

void Call::init() {
    if (!fn_) return;
    const std::int32_t size = std::max<std::int32_t>({fn_->GetPropertiesSize(), fn_->GetParmsSize(), 16});
    buf_ = static_cast<std::uint8_t*>(FMemory::Malloc(static_cast<SIZE_T>(size), 16));
    std::memset(buf_, 0, static_cast<std::size_t>(size));
    for (FProperty* p : TFieldRange<FProperty>(fn_, EFieldIterationFlags::None)) {
        if (!p || !p->HasAnyPropertyFlags(CPF_Parm)) continue;
        p->InitializeValue(buf_ + p->GetOffset_Internal());
        if (p->HasAnyPropertyFlags(CPF_ReturnParm)) ret_ = p;
        else params_.push_back(p);
    }
}

Call::~Call() {
    if (!buf_) return;
    for (FProperty* p : params_) p->DestroyValue(buf_ + p->GetOffset_Internal());
    if (ret_) ret_->DestroyValue(buf_ + ret_->GetOffset_Internal());
    FMemory::Free(buf_);
}

void* Call::slot(FProperty* p) const { return (p && buf_) ? buf_ + p->GetOffset_Internal() : nullptr; }

FProperty* Call::param(const wchar_t* name) const {
    for (FProperty* p : params_) {
        if (p->GetName() == name) return p;
    }
    return (ret_ && ret_->GetName() == name) ? ret_ : nullptr;
}

Call& Call::put_b(FProperty* p, bool v) {
    if (p) write_num(p, slot(p), v ? 1.0 : 0.0);
    return *this;
}
Call& Call::put_n(FProperty* p, double v) {
    if (p) write_num(p, slot(p), v);
    return *this;
}
Call& Call::put_o(FProperty* p, const UObject* v) {
    if (p) *static_cast<const UObject**>(slot(p)) = v;
    return *this;
}
Call& Call::put_name(FProperty* p, FName v) {
    if (p) *static_cast<FName*>(slot(p)) = v;
    return *this;
}
Call& Call::put_s(FProperty* p, const wchar_t* v) {
    if (!p) return *this;
    auto* a = static_cast<RawArray*>(slot(p));
    const wchar_t* text = v ? v : L"";
    const auto len = static_cast<std::int32_t>(std::wcslen(text)) + 1;
    a->data = FMemory::Malloc(static_cast<SIZE_T>(len) * sizeof(wchar_t), alignof(wchar_t));
    std::memcpy(a->data, text, static_cast<std::size_t>(len) * sizeof(wchar_t));
    a->num = a->max = len;
    return *this;
}
Call& Call::put_v(FProperty* p, const V3& v) {
    if (p) write_vec(slot(p), p->GetSize(), v);
    return *this;
}
Call& Call::put_r(FProperty* p, const Rot& v) {
    const double c[3]{v.pitch, v.yaw, v.roll};
    if (p) write_components(slot(p), p->GetSize(), c, 3);
    return *this;
}
Call& Call::put_c(FProperty* p, const Color& v) {
    if (p) {
        auto* d = static_cast<float*>(slot(p));
        d[0] = v.r, d[1] = v.g, d[2] = v.b, d[3] = v.a;
    }
    return *this;
}
Call& Call::put_xf(FProperty* p, const V3& loc, const Rot& rot, const V3& scale) {
    if (!p) return *this;
    double q[4];
    rot_to_quat(rot, q);
    const double data[12]{q[0], q[1], q[2], q[3], loc.x, loc.y, loc.z, 0.0, scale.x, scale.y, scale.z, 0.0};
    write_components(slot(p), p->GetSize() >= 96 ? 96 : 48, data, 12); // FTransform: quat, translation(+pad), scale(+pad)
    return *this;
}
Call& Call::put_bytes(FProperty* p, const void* data, std::size_t size) {
    if (p) std::memcpy(slot(p), data, std::min<std::size_t>(size, static_cast<std::size_t>(p->GetSize())));
    return *this;
}
Call& Call::put_copy(FProperty* p, const void* src) {
    if (p && src) p->CopySingleValue(slot(p), src);
    return *this;
}
Call& Call::put_arr(FProperty* p, const void* items, std::size_t elem, std::int32_t count) {
    if (!p) return *this;
    auto* a = static_cast<RawArray*>(slot(p));
    if (count > 0) {
        a->data = FMemory::Malloc(elem * static_cast<std::size_t>(count), 16);
        std::memcpy(a->data, items, elem * static_cast<std::size_t>(count));
    }
    a->num = a->max = count;
    return *this;
}

bool Call::go() {
    if (!*this || !alive(self_)) return false;
    self_->ProcessEvent(fn_, buf_);
    return true;
}

bool Call::rb() const { return ret_ && read_num(ret_, slot(ret_)) != 0.0; }
std::int64_t Call::ri() const { return ret_ ? static_cast<std::int64_t>(read_num(ret_, slot(ret_))) : 0; }
double Call::rf() const { return ret_ ? read_num(ret_, slot(ret_)) : 0.0; }
UObject* Call::ro() const {
    if (!ret_) return nullptr;
    UObject* o = *static_cast<UObject**>(slot(ret_));
    return alive(o) ? o : nullptr;
}
V3 Call::rv() const { return ret_ ? read_vec(slot(ret_), ret_->GetSize()) : V3{}; }
std::wstring Call::rs() const { return ret_ ? read_fstring(slot(ret_)) : std::wstring{}; }
FName Call::rn() const { return ret_ ? *static_cast<const FName*>(slot(ret_)) : FName{}; }

// ---------------- common helpers ----------------

UObject* gameplay_statics() { return find(L"/Script/Engine.Default__GameplayStatics"); }
UObject* system_library() { return find(L"/Script/Engine.Default__KismetSystemLibrary"); }

V3 actor_location(UObject* a) {
    Call c(a, L"K2_GetActorLocation");
    return c.go() ? c.rv() : V3{};
}

void set_actor_location(UObject* a, const V3& v, bool sweep, bool teleport) {
    Call c(a, L"K2_SetActorLocation");
    c.v(v).b(sweep).skip().b(teleport).go();
}

void console(UObject* ctx, const wchar_t* cmd) {
    Call(system_library(), L"ExecuteConsoleCommand").o(world_of(ctx)).s(cmd).o(nullptr).go();
}

} // namespace lie::ue
