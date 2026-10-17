#pragma once

// Binds data/binary.json (deployed next to the mod DLL) to the running executable:
// module name and SHA-256 must match, otherwise nothing native is armed (fail-closed).

#include "core/binary_manifest.h"

#include <filesystem>
#include <optional>
#include <string>

namespace lie::binary {

std::filesystem::path mod_dir();        // <Mods>/<ModName>
std::string sha256_file(const std::filesystem::path& path);

struct Bound {
    Manifest manifest;
    std::string exe_sha256;
};

// Loads <mod_dir>/data/binary.json and checks it against the current process image.
std::optional<Bound> load_for_current_process(std::string* why);

// Pointer to the function in the running image if its bytes match the manifest prefix.
void* resolve(const Function& fn, std::string* why);

} // namespace lie::binary
