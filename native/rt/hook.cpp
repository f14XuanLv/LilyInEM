#include "rt/hook.h"

#include "rt/binary.h"
#include "rt/log.h"

#include <polyhook2/Detour/x64Detour.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace lie::hook {

namespace {

std::mutex g_mutex;
std::map<std::string, std::unique_ptr<PLH::x64Detour>, std::less<>> g_detours;

bool arm(std::string_view name, void* address, void* detour, void** original) {
    std::scoped_lock lk(g_mutex);
    if (g_detours.contains(name)) return true;
    auto d = std::make_unique<PLH::x64Detour>(reinterpret_cast<std::uint64_t>(address), reinterpret_cast<std::uint64_t>(detour),
                                              reinterpret_cast<std::uint64_t*>(original));
    if (!d->hook()) {
        log::error(L"detour failed: %hs", std::string(name).c_str());
        return false;
    }
    g_detours.emplace(std::string(name), std::move(d));
    log::info(L"detour armed: %hs @ %p", std::string(name).c_str(), address);
    return true;
}

} // namespace

bool attach(const binary::Manifest& m, std::string_view function, void* detour, void** original) {
    const binary::Function* fn = m.fn(function);
    if (!fn) {
        log::error(L"binary.json has no function '%hs'", std::string(function).c_str());
        return false;
    }
    std::string why;
    void* address = binary::resolve(*fn, &why);
    if (!address) {
        log::error(L"refusing to hook: %hs", why.c_str());
        return false;
    }
    return arm(function, address, detour, original);
}

bool attach_at(std::string_view name, void* address, void* detour, void** original) {
    return address && arm(name, address, detour, original);
}

bool attached(std::string_view name) {
    std::scoped_lock lk(g_mutex);
    return g_detours.contains(name);
}

void detach_all() {
    std::scoped_lock lk(g_mutex);
    for (auto& [name, d] : g_detours) d->unHook();
    g_detours.clear();
}

} // namespace lie::hook
