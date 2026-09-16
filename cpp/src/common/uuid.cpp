#include "uuid.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <random>

namespace at {

namespace {
std::mt19937_64& engine() {
    static std::mt19937_64 eng{[] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64{seq};
    }()};
    return eng;
}
std::mutex& engine_mutex() {
    static std::mutex m;
    return m;
}
constexpr char kHex[] = "0123456789abcdef";
} // namespace

std::string uuid_v4() {
    std::array<std::uint8_t, 16> b{};
    {
        std::lock_guard<std::mutex> lock(engine_mutex());
        std::uint64_t hi = engine()();
        std::uint64_t lo = engine()();
        for (int i = 0; i < 8; ++i) {
            b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(hi >> (8 * i));
            b[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(lo >> (8 * i));
        }
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0f) | 0x40); // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3f) | 0x80); // variant 10xx
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[b[i] >> 4];
        out += kHex[b[i] & 0x0f];
    }
    return out;
}

bool is_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!hex(s[i])) {
            return false;
        }
    }
    char ver = s[14];
    char var = s[19];
    return ver >= '1' && ver <= '8' && (var == '8' || var == '9' || var == 'a' || var == 'b');
}

} // namespace at
