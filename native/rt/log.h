#pragma once

// printf-style logging into UE4SS.log with a per-mod tag (set once by each mod entry point).

#include <DynamicOutput/DynamicOutput.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace lie::log {

inline const wchar_t*& tag() {
    static const wchar_t* t = L"LilyInEM";
    return t;
}

inline std::wstring vformat(const wchar_t* fmt, va_list ap) {
    wchar_t buf[2048];
    std::vswprintf(buf, std::size(buf), fmt, ap);
    return buf;
}

inline void info(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::wstring msg = vformat(fmt, ap);
    va_end(ap);
    RC::Output::send(STR("[{}] {}\n"), tag(), msg);
}

inline void warn(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::wstring msg = vformat(fmt, ap);
    va_end(ap);
    RC::Output::send<RC::LogLevel::Warning>(STR("[{}] {}\n"), tag(), msg);
}

inline void error(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::wstring msg = vformat(fmt, ap);
    va_end(ap);
    RC::Output::send<RC::LogLevel::Error>(STR("[{}] {}\n"), tag(), msg);
}

inline std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace lie::log
