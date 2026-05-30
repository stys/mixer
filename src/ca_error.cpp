#include "ca_error.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

std::string fourCC(OSStatus s) {
    const uint32_t v = static_cast<uint32_t>(s);
    const char c[4] = {
        static_cast<char>((v >> 24) & 0xff), static_cast<char>((v >> 16) & 0xff),
        static_cast<char>((v >> 8) & 0xff),  static_cast<char>(v & 0xff),
    };
    const bool printable = std::all_of(std::begin(c), std::end(c),
        [](char ch) { return ch >= 0x20 && ch < 0x7f; });
    return printable ? std::string(c, 4) : std::to_string(s);
}

void check(OSStatus status, const char* op) {
    if (status != noErr) {
        throw std::runtime_error(std::string(op) + " failed (OSStatus " +
                                 std::to_string(status) + " / '" + fourCC(status) + "')");
    }
}