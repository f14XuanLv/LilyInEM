#pragma once

// Reflection layer shared by both mods (UE 4.26 Guest, UE 5.4 Host).
// - Lookups (classes, functions, properties) are resolved once and cached; objects are held weakly.
// - Every UFunction call goes through Call: the parameter buffer is laid out from the function's
//   FProperty offsets, and vectors / rotators / transforms are written as float (UE4) or double (UE5)
//   depending on the reflected size, so the same code drives both engines.
// - Parameters can be filled positionally (.b/.i/.f/...) or by name (.set(L"Param", value)).

#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RC::Unreal {
class UObject;
class UClass;
class UFunction;
class UStruct;
class FProperty;
} // namespace RC::Unreal

namespace lie::ue {

using RC::Unreal::FName;
using RC::Unreal::FProperty;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
using RC::Unreal::UStruct;

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct Rot {
    double pitch = 0.0, yaw = 0.0, roll = 0.0;
};
struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};
struct RawArray { // TArray / FString layout (identical in 4.26 and 5.4)
    void* data;
    std::int32_t num;
    std::int32_t max;
};

// ---- objects ----
bool alive(const UObject* obj) noexcept;
UObject* find(std::wstring_view path);           // StaticFindObject, cached
UClass* find_class(std::wstring_view path);
// Game thread only: loads the asset's package through the AssetRegistry when it is not resident yet.
// For blueprint classes pass the asset path ("/Game/P/BP_X.BP_X") and look up "..._C" afterwards.
UObject* load_asset(std::wstring_view asset_object_path);
UClass* load_class(std::wstring_view class_path); // "/Game/P/BP_X.BP_X_C"
UObject* first(const wchar_t* short_class_name); // FindFirstOf: low frequency only (FR-ENG-2)
std::wstring class_name(const UObject* obj);
bool class_contains(const UObject* obj, std::wstring_view fragment);
bool is_a(const UObject* obj, UClass* cls);
UObject* world_of(UObject* obj);
FName fname(const wchar_t* text);
std::wstring str(const FName& n);
std::wstring read_fstring(const void* fstring_ptr);

// ---- properties ----
FProperty* prop(UStruct* type, std::wstring_view name);
FProperty* member_of(FProperty* struct_prop, std::wstring_view field); // field of a struct-typed property
FProperty* array_inner(FProperty* array_prop);                         // element property of a TArray
UObject* weak_get(const void* weak_object_ptr);                        // FWeakObjectPtr / TWeakObjectPtr value
void* field_ptr(UObject* obj, std::wstring_view name);
template <class T>
T* field(UObject* obj, std::wstring_view name) { return static_cast<T*>(field_ptr(obj, name)); }
UObject* obj(UObject* owner, std::wstring_view name);

double read_num(FProperty* p, const void* value);
void write_num(FProperty* p, void* value, double v);
V3 read_vec(const void* value, std::int32_t size);
void write_vec(void* value, std::int32_t size, const V3& v);

double num(UObject* o, std::wstring_view name, double fallback = 0.0);
bool set_num(UObject* o, std::wstring_view name, double v);
bool flag(UObject* o, std::wstring_view name, bool fallback = false);
bool set_flag(UObject* o, std::wstring_view name, bool v);
V3 vec(UObject* o, std::wstring_view name);
bool set_vec(UObject* o, std::wstring_view name, const V3& v);

UFunction* function(UObject* obj, std::wstring_view name);

// TMap iteration: fn(key_ptr, value_ptr, key_prop, value_prop) for every valid pair
using MapVisitor = void (*)(void* ctx, void* key, void* value, FProperty* kp, FProperty* vp);
bool map_visit(void* map_ptr, FProperty* map_prop, MapVisitor visit, void* ctx);
template <class F>
bool map_each(void* map_ptr, FProperty* map_prop, F&& fn) {
    return map_visit(map_ptr, map_prop, [](void* c, void* k, void* v, FProperty* kp, FProperty* vp) { (*static_cast<F*>(c))(k, v, kp, vp); }, &fn);
}

// ---- weak handle ----
class Weak {
public:
    Weak() = default;
    explicit Weak(UObject* o) { set(o); }
    UObject* get() const;
    void set(UObject* o);
    void reset() { set(nullptr); }
    explicit operator bool() const { return get() != nullptr; }

private:
    RC::Unreal::FWeakObjectPtr ptr_{};
};

// ---- UFunction call ----
class Call {
public:
    Call(UObject* self, std::wstring_view function_name);
    Call(UObject* self, UFunction* fn);
    ~Call();
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    explicit operator bool() const { return fn_ && buf_; }

    // positional
    Call& b(bool v) { return put_b(next(), v); }
    Call& i(std::int64_t v) { return put_n(next(), static_cast<double>(v)); }
    Call& f(double v) { return put_n(next(), v); }
    Call& o(const UObject* v) { return put_o(next(), v); }
    Call& n(FName v) { return put_name(next(), v); }
    Call& n(const wchar_t* v) { return put_name(next(), fname(v)); }
    Call& s(const wchar_t* v) { return put_s(next(), v); }
    Call& v(const V3& v) { return put_v(next(), v); }
    Call& r(const Rot& v) { return put_r(next(), v); }
    Call& c(const Color& v) { return put_c(next(), v); }
    Call& xf(const V3& loc, const Rot& rot = {}, const V3& scale = {1.0, 1.0, 1.0}) { return put_xf(next(), loc, rot, scale); }
    Call& bytes(const void* data, std::size_t size) { return put_bytes(next(), data, size); }
    Call& copy(const void* src) { return put_copy(next(), src); } // deep copy (TArrays etc.) of a value of the param's type
    template <class T>
    Call& arr(const std::vector<T>& items) { return put_arr(next(), items.data(), sizeof(T), static_cast<std::int32_t>(items.size())); }
    Call& skip() { next(); return *this; }

    // named
    Call& set_b(const wchar_t* p, bool v) { return put_b(param(p), v); }
    Call& set_n(const wchar_t* p, double v) { return put_n(param(p), v); }
    Call& set_o(const wchar_t* p, const UObject* v) { return put_o(param(p), v); }
    Call& set_name(const wchar_t* p, FName v) { return put_name(param(p), v); }
    Call& set_v(const wchar_t* p, const V3& v) { return put_v(param(p), v); }
    Call& set_c(const wchar_t* p, const Color& v) { return put_c(param(p), v); }
    template <class T>
    Call& set_arr(const wchar_t* p, const std::vector<T>& items) { return put_arr(param(p), items.data(), sizeof(T), static_cast<std::int32_t>(items.size())); }

    bool go();

    bool rb() const;
    std::int64_t ri() const;
    double rf() const;
    UObject* ro() const;
    V3 rv() const;
    std::wstring rs() const;
    FName rn() const;

    FProperty* param(const wchar_t* name) const;
    FProperty* ret_prop() const { return ret_; }
    void* slot(FProperty* p) const;
    void* out(const wchar_t* name) const { return slot(param(name)); }

private:
    void init();
    FProperty* next() { return cursor_ < params_.size() ? params_[cursor_++] : nullptr; }
    Call& put_b(FProperty* p, bool v);
    Call& put_n(FProperty* p, double v);
    Call& put_o(FProperty* p, const UObject* v);
    Call& put_name(FProperty* p, FName v);
    Call& put_s(FProperty* p, const wchar_t* v);
    Call& put_v(FProperty* p, const V3& v);
    Call& put_r(FProperty* p, const Rot& v);
    Call& put_c(FProperty* p, const Color& v);
    Call& put_xf(FProperty* p, const V3& loc, const Rot& rot, const V3& scale);
    Call& put_bytes(FProperty* p, const void* data, std::size_t size);
    Call& put_copy(FProperty* p, const void* src);
    Call& put_arr(FProperty* p, const void* items, std::size_t elem, std::int32_t count);

    UObject* self_ = nullptr;
    UFunction* fn_ = nullptr;
    std::uint8_t* buf_ = nullptr;
    std::vector<FProperty*> params_;
    FProperty* ret_ = nullptr;
    std::size_t cursor_ = 0;
};

inline bool call(UObject* o, std::wstring_view fn) {
    Call c(o, fn);
    return c && c.go();
}
inline bool call_bool(UObject* o, std::wstring_view fn, bool fallback = false) {
    Call c(o, fn);
    return c.go() ? c.rb() : fallback;
}
inline double call_num(UObject* o, std::wstring_view fn, double fallback = 0.0) {
    Call c(o, fn);
    return c.go() ? c.rf() : fallback;
}
inline UObject* call_obj(UObject* o, std::wstring_view fn) {
    Call c(o, fn);
    return c.go() ? c.ro() : nullptr;
}

// Common engine helpers (both engines expose the same UFunctions)
UObject* gameplay_statics();
UObject* system_library();
V3 actor_location(UObject* actor);
void set_actor_location(UObject* actor, const V3& v, bool sweep = false, bool teleport = true);
void console(UObject* world_context, const wchar_t* cmd);

} // namespace lie::ue
