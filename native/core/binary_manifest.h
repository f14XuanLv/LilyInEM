#pragma once

// Runtime model of data/binary/{EL,EM}.json: the version-specific binary facts (EXE SHA-256,
// native function RVAs + byte prefixes, raw struct offsets). Parsing and validation are pure C++;
// the runtime loader (rt/binary.h) reads the file next to the mod DLL and binds it to the running EXE.

#include "core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lie::binary {

struct Function {
    std::string name;
    std::uintptr_t rva = 0;
    std::vector<std::uint8_t> prefix;
};

struct Manifest {
    int schema = 0;
    std::string module;
    std::vector<std::string> sha256; // lower-case hex
    std::map<std::string, Function, std::less<>> functions;
    std::map<std::string, std::int64_t, std::less<>> offsets;

    const Function* fn(std::string_view name) const {
        auto it = functions.find(name);
        return it == functions.end() ? nullptr : &it->second;
    }
    std::optional<std::int64_t> offset(std::string_view name) const {
        auto it = offsets.find(name);
        return it == offsets.end() ? std::nullopt : std::optional<std::int64_t>(it->second);
    }
    bool accepts_sha256(std::string_view hex) const {
        std::string lower(hex);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::ranges::find(sha256, lower) != sha256.end();
    }
};

inline std::optional<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view hex) {
    if (hex.empty() || hex.size() % 2) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

inline std::optional<Manifest> parse(std::string_view text, std::string* err) {
    auto fail = [&](std::string why) -> std::optional<Manifest> {
        if (err) *err = std::move(why);
        return std::nullopt;
    };
    json::Value root;
    std::string jerr;
    if (!json::parse(text, root, &jerr)) return fail("json: " + jerr);
    Manifest m;
    const auto* schema = root.get("schema");
    if (!schema || !schema->is_number() || schema->num != 1) return fail("unsupported schema");
    m.schema = 1;
    const auto* module = root.get("module");
    if (!module || !module->is_string() || module->str.empty()) return fail("missing module");
    m.module = module->str;
    const auto* hashes = root.get("sha256");
    if (!hashes || !hashes->is_array() || hashes->arr.empty()) return fail("missing sha256 list");
    for (const auto& h : hashes->arr) {
        if (!h.is_string() || h.str.size() != 64 || !parse_hex_bytes(h.str)) return fail("bad sha256 entry");
        std::string lower = h.str;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        m.sha256.push_back(std::move(lower));
    }
    if (const auto* fns = root.get("functions")) {
        if (!fns->is_object()) return fail("functions must be an object");
        for (const auto& [name, f] : fns->obj) {
            const auto* rva = f.get("rva");
            const auto* prefix = f.get("prefix");
            if (!rva || !rva->is_string() || !prefix || !prefix->is_string()) return fail(name + ": rva/prefix missing");
            char* end = nullptr;
            const auto value = std::strtoull(rva->str.c_str(), &end, 16);
            if (!end || *end != '\0' || value == 0) return fail(name + ": bad rva");
            auto bytes = parse_hex_bytes(prefix->str);
            if (!bytes || bytes->size() < 8) return fail(name + ": prefix must be >= 8 hex bytes");
            m.functions.emplace(name, Function{name, static_cast<std::uintptr_t>(value), std::move(*bytes)});
        }
    }
    if (const auto* offs = root.get("offsets")) {
        if (!offs->is_object()) return fail("offsets must be an object");
        for (const auto& [name, v] : offs->obj) {
            if (!v.is_number()) return fail(name + ": offset must be a number");
            m.offsets.emplace(name, static_cast<std::int64_t>(v.num));
        }
    }
    return m;
}

} // namespace lie::binary
