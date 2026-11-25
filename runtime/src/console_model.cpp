// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/console_model.hpp"
#include <g6shim/settings.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace g6shim {

namespace {

constexpr char kConsoleVariable[] = "G6SHIM_CONSOLE";
constexpr char kSeriesXValue[] = "series-x";
constexpr char kSeriesSValue[] = "series-s";
constexpr char kSeriesXName[] = "Xbox Series X";
constexpr char kSeriesSName[] = "Xbox Series S";
constexpr int kUnset = -1;

std::atomic<int> g_model{kUnset};

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (lower(left[i]) != lower(right[i])) return false;
    return true;
}

ConsoleModel model_from_setting() {
    const std::optional<std::string> value = settings::get(kConsoleVariable);
    if (!value || value->empty()) return ConsoleModel::kSeriesX;
    if (const std::optional<ConsoleModel> parsed = parse_console_model(*value)) return *parsed;
    G6SHIM_WARN("console model: unknown %s value \"%s\"; using series-x", kConsoleVariable, value->c_str());
    return ConsoleModel::kSeriesX;
}

}  // namespace

ConsoleModel console_model() {
    int model = g_model.load(std::memory_order_relaxed);
    if (model == kUnset) {
        model = static_cast<int>(model_from_setting());
        int expected = kUnset;
        if (!g_model.compare_exchange_strong(expected, model, std::memory_order_relaxed))
            model = expected;
    }
    return static_cast<ConsoleModel>(model);
}

void set_console_model(ConsoleModel model) {
    g_model.store(static_cast<int>(model), std::memory_order_relaxed);
}

std::optional<ConsoleModel> parse_console_model(std::string_view value) {
    if (same_text(value, kSeriesXValue)) return ConsoleModel::kSeriesX;
    if (same_text(value, kSeriesSValue)) return ConsoleModel::kSeriesS;
    return std::nullopt;
}

const char* console_model_name(ConsoleModel model) {
    return model == ConsoleModel::kSeriesS ? kSeriesSName : kSeriesXName;
}

}  // namespace g6shim
