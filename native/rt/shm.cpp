#include "rt/shm.h"

#include "rt/clock.h"

#include <new>

namespace lie::shm {

namespace {

struct Mapping {
    HANDLE handle = nullptr;
    void* view = nullptr;
};

Mapping g_control, g_world;

template <class T>
T* map_block(Mapping& m, const wchar_t* name) {
    if (m.view) return static_cast<T*>(m.view);
    m.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(T)), name);
    if (!m.handle) return nullptr;
    m.view = MapViewOfFile(m.handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(T));
    if (!m.view) {
        CloseHandle(m.handle);
        m.handle = nullptr;
        return nullptr;
    }
    auto* block = static_cast<T*>(m.view);
    // Fresh mappings are zero-filled; a stale layout (other protocol) is rebuilt in place.
    if (block->magic != kMagic || block->protocol != kProtocol) {
        new (block) T();
        block->protocol = kProtocol;
        block->magic = kMagic;
    }
    return block;
}

void unmap(Mapping& m) {
    if (m.view) UnmapViewOfFile(m.view);
    if (m.handle) CloseHandle(m.handle);
    m = {};
}

} // namespace

bool open() {
    // Serialise first-time initialisation between the two processes.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\LilyInEM_ShmInit_v1");
    if (mutex) WaitForSingleObject(mutex, 5000);
    const bool ok = map_block<Control>(g_control, kControlShmName) && map_block<World>(g_world, kWorldShmName);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return ok;
}

void close() {
    unmap(g_world);
    unmap(g_control);
}

Control* control() noexcept { return static_cast<Control*>(g_control.view); }
World* world() noexcept { return static_cast<World*>(g_world.view); }

} // namespace lie::shm
