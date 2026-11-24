// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/console_model.hpp>

#include "d3d12_xs/console.hpp"
#include "d3d12_xs/dxgi.hpp"
#include "statistics.hpp"
#include "xsystem.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

namespace dxs = g6shim::d3d12_xs;
namespace xgr = g6shim::xgameruntime;
namespace xs = g6shim::xgameruntime::xsystem;
namespace xmem = g6shim::xmem;

using g6shim::ConsoleModel;

struct ConsoleRestore {
    ~ConsoleRestore() {
        g6shim::set_console_model(ConsoleModel::kSeriesX);
        xmem::reset_title_memory();
    }
};

void use_console(ConsoleModel model) {
    g6shim::set_console_model(model);
    xmem::reset_title_memory();
}

using Describe = HRESULT (*)(dxs::Object*, dxs::dxgi::AdapterDescription*);
using Analytics = xs::AnalyticsInfo* (*)(xgr::Object*, xs::AnalyticsInfo*);

}  // namespace

TEST(console_model_parses_its_values) {
    CHECK(g6shim::parse_console_model("series-x") == ConsoleModel::kSeriesX);
    CHECK(g6shim::parse_console_model("series-s") == ConsoleModel::kSeriesS);
    CHECK(g6shim::parse_console_model("SERIES-X") == ConsoleModel::kSeriesX);
    CHECK(g6shim::parse_console_model("Series-S") == ConsoleModel::kSeriesS);
    CHECK(!g6shim::parse_console_model("series").has_value());
    CHECK(!g6shim::parse_console_model("series-x2").has_value());
    CHECK(!g6shim::parse_console_model("").has_value());
    CHECK(std::strcmp(g6shim::console_model_name(ConsoleModel::kSeriesX), "Xbox Series X") == 0);
    CHECK(std::strcmp(g6shim::console_model_name(ConsoleModel::kSeriesS), "Xbox Series S") == 0);
}

TEST(console_model_setter_reports_the_chosen_model) {
    ConsoleRestore restore;
    g6shim::set_console_model(ConsoleModel::kSeriesS);
    CHECK(g6shim::console_model() == ConsoleModel::kSeriesS);
    g6shim::set_console_model(ConsoleModel::kSeriesX);
    CHECK(g6shim::console_model() == ConsoleModel::kSeriesX);
}

TEST(console_model_drives_the_graphics_adapter_memory) {
    ConsoleRestore restore;
    dxs::Object* adapter = dxs::dxgi::adapter_object();
    const Describe describe = dxs::slot<Describe>(adapter, 0x40);

    use_console(ConsoleModel::kSeriesS);
    CHECK(dxs::current_console() == dxs::Console::kSeriesS);
    dxs::dxgi::AdapterDescription small{};
    CHECK(describe(adapter, &small) == S_OK);
    CHECK(small.dedicated_video_memory == 0);
    CHECK(small.shared_system_memory == std::size_t{7776} << 20);

    use_console(ConsoleModel::kSeriesX);
    CHECK(dxs::current_console() == dxs::Console::kSeriesX);
    dxs::dxgi::AdapterDescription large{};
    CHECK(describe(adapter, &large) == S_OK);
    CHECK(large.dedicated_video_memory == std::size_t{10} << 30);
    CHECK(large.shared_system_memory == (std::size_t{7} << 30) / 2);
}

TEST(console_model_drives_the_title_memory_budget) {
    ConsoleRestore restore;

    use_console(ConsoleModel::kSeriesS);
    const xmem::TitleMemory small = xmem::title_memory();
    CHECK(small.gpu_optimal == 0);
    CHECK(small.standard == std::size_t{7776} << 20);

    xmem::WorkingSet statistics{};
    CHECK(xmem::get_working_set_statistics(0, &statistics) == 1);
    CHECK(statistics.gpu_optimal_limit == 0);
    CHECK(statistics.title_limit <= small.standard);

    use_console(ConsoleModel::kSeriesX);
    const xmem::TitleMemory large = xmem::title_memory();
    CHECK(large.gpu_optimal == std::size_t{10} << 30);
    CHECK(large.standard == (std::size_t{7} << 30) / 2);
}

TEST(console_model_drives_the_runtime_analytics_form) {
    ConsoleRestore restore;
    const auto get = xgr::method<Analytics>(xs::kAnalyticsObject, 0x18);
    xgr::Object* object = const_cast<xgr::Object*>(&xs::kAnalyticsObject);

    use_console(ConsoleModel::kSeriesS);
    xs::AnalyticsInfo small{};
    CHECK(get(object, &small) == &small);
    CHECK(std::strcmp(small.form.data(), "Xbox Series S") == 0);

    use_console(ConsoleModel::kSeriesX);
    xs::AnalyticsInfo large{};
    CHECK(get(object, &large) == &large);
    CHECK(std::strcmp(large.form.data(), "Xbox Series X") == 0);
}
