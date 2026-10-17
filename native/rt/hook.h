#pragma once

// Native detours (PolyHook2 bundled with UE4SS). Manifest functions are prefix-checked first.

#include "core/binary_manifest.h"

#include <string_view>

namespace lie::hook {

// Arms a detour on a manifest function. Returns false (and logs) on prefix mismatch / failure.
bool attach(const binary::Manifest& m, std::string_view function, void* detour, void** original);
// Arms a detour at a known address (system DLL exports, COM vtable slots).
bool attach_at(std::string_view name, void* address, void* detour, void** original);
bool attached(std::string_view name);
void detach_all();

} // namespace lie::hook
