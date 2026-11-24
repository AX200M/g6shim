// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace g6shim::shader::emit {

class Validator {
public:
    Validator() = default;
    ~Validator();

    Validator(const Validator&) = delete;
    Validator& operator=(const Validator&) = delete;
    Validator(Validator&& other) noexcept;
    Validator& operator=(Validator&& other) noexcept;

    static std::optional<std::filesystem::path> default_path();
    static Validator load(const std::filesystem::path* path = nullptr);

    void validate(std::span<std::uint8_t> container);

private:
    explicit Validator(void* object) : object_(object) {}

    void* object_ = nullptr;
};

}  // namespace g6shim::shader::emit
