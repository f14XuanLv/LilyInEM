#include "rt/binary.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace lie::binary {

std::filesystem::path mod_dir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&mod_dir), &self);
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(self, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().parent_path(); // <Mod>/dlls/main.dll -> <Mod>
}

std::string sha256_file(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return out;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::ifstream f(path, std::ios::binary);
        std::array<char, 1 << 16> buf{};
        bool ok = static_cast<bool>(f);
        while (ok && f) {
            f.read(buf.data(), buf.size());
            if (f.gcount() > 0) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(f.gcount()), 0) >= 0;
        }
        std::array<unsigned char, 32> digest{};
        if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
            static constexpr char hex[] = "0123456789abcdef";
            for (unsigned char b : digest) {
                out += hex[b >> 4];
                out += hex[b & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::optional<Bound> load_for_current_process(std::string* why) {
    auto fail = [&](std::string s) -> std::optional<Bound> {
        if (why) *why = std::move(s);
        return std::nullopt;
    };
    const auto file = mod_dir() / L"data" / L"binary.json";
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("missing " + file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    auto manifest = parse(ss.str(), &err);
    if (!manifest) return fail("binary.json: " + err);

    wchar_t image[MAX_PATH]{};
    GetModuleFileNameW(nullptr, image, MAX_PATH);
    const std::filesystem::path exe(image);
    const std::wstring expected(manifest->module.begin(), manifest->module.end());
    if (_wcsicmp(exe.filename().c_str(), expected.c_str()) != 0) return fail("binary.json is for " + manifest->module);
    Bound b{std::move(*manifest), sha256_file(exe)};
    if (b.exe_sha256.empty()) return fail("cannot hash the game executable");
    if (!b.manifest.accepts_sha256(b.exe_sha256)) return fail("unsupported game build (SHA-256 " + b.exe_sha256 + ")");
    return b;
}

void* resolve(const Function& fn, std::string* why) {
    auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    const auto* at = base + fn.rva;
    for (std::size_t i = 0; i < fn.prefix.size(); ++i) {
        if (at[i] != fn.prefix[i]) {
            if (why) *why = fn.name + ": prefix mismatch at byte " + std::to_string(i);
            return nullptr;
        }
    }
    return const_cast<std::uint8_t*>(at);
}

} // namespace lie::binary
