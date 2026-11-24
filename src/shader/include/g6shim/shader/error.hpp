// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace g6shim::shader {

enum class ErrorKind : std::uint8_t {
    NotDxbc,
    Malformed,
    Isa,
    Unsupported,
};

class ParseError : public std::runtime_error {
public:
    ParseError(ErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

    ErrorKind kind() const noexcept { return kind_; }

private:
    ErrorKind kind_;
};

std::string hex(std::uint64_t value);

namespace detail {

[[noreturn]] inline void fail_not_dxbc() {
    throw ParseError(ErrorKind::NotDxbc, "not a DXBC container");
}

[[noreturn]] inline void fail_malformed(const char* reason) {
    throw ParseError(ErrorKind::Malformed, std::string("malformed shader: ") + reason);
}

[[noreturn]] inline void fail_malformed(const std::string& reason) {
    throw ParseError(ErrorKind::Malformed, "malformed shader: " + reason);
}

[[noreturn]] inline void fail_isa(std::size_t offset, const char* reason) {
    throw ParseError(ErrorKind::Isa, "GFX10 code at " + hex(offset) + ": " + reason);
}

[[noreturn]] inline void fail_unsupported(std::size_t offset, const char* what) {
    throw ParseError(ErrorKind::Unsupported, "GFX10 code at " + hex(offset) + ": " + std::string(what) + " is not supported");
}

}  // namespace detail

}  // namespace g6shim::shader
