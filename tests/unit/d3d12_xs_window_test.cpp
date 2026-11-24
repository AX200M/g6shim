// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/window_geometry.hpp"

#include <windows.h>

using namespace g6shim::d3d12_xs::window;

namespace {

const Rect kWorkArea{0, 0, 1920, 1040};

void unset(const char* name) { ::SetEnvironmentVariableA(name, nullptr); }

}

TEST(d3d12_xs_window_shrinks_to_the_work_area) {
    CHECK(fit_into(kWorkArea, 3840, 2160) == (Size{1848, 1040}));
    CHECK(fit_into(kWorkArea, 1280, 720) == (Size{1280, 720}));
    CHECK(fit_into(kWorkArea, 1920, 1040) == (Size{1920, 1040}));
    CHECK(fit_into(kWorkArea, 3840, 2000) == (Size{1920, 1000}));
    CHECK(fit_into(kWorkArea, kUsedDefault, 0) == (Size{kUsedDefault, 0}));
    CHECK(fit_into(kWorkArea, 0, 2160) == (Size{0, 2160}));
    CHECK(fit_into(Rect{0, 0, 0, 0}, 3840, 2160) == (Size{3840, 2160}));
    CHECK(fit_into(Rect{0, 0, 1920, 0}, 3840, 2160) == (Size{3840, 2160}));
}

TEST(d3d12_xs_window_keeps_its_aspect_ratio) {
    const Size wide = fit_into(Rect{0, 0, 1000, 1000}, 1920, 1080);
    CHECK(wide.width == 1000);
    CHECK(wide.height == 562);
    const Size tall = fit_into(Rect{0, 0, 1000, 1000}, 1080, 1920);
    CHECK(tall.width == 562);
    CHECK(tall.height == 1000);
    const Size square = fit_into(Rect{0, 0, 500, 500}, 1000, 1000);
    CHECK(square == (Size{500, 500}));
    const Size no_stretch = fit_into(Rect{0, 0, 1920, 1200}, 1280, 720);
    CHECK(no_stretch == (Size{1280, 720}));
}

TEST(d3d12_xs_window_is_centred_when_fitted) {
    const Placement fitted = fit(kWorkArea, 100, 200, 3840, 2160);
    CHECK(fitted.client == (Size{1848, 1040}));
    CHECK(fitted.x == 36);
    CHECK(fitted.y == 0);
    const Placement left_alone = fit(kWorkArea, 100, 200, 1280, 720);
    CHECK(left_alone.client == (Size{1280, 720}));
    CHECK(left_alone.x == 100);
    CHECK(left_alone.y == 200);
    const Placement second_monitor = fit(Rect{1920, 0, 3840, 1040}, 0, 0, 3840, 2160);
    CHECK(second_monitor.x == 1956);
    CHECK(second_monitor.y == 0);
    const Placement above = fit(Rect{0, -1040, 1920, 0}, 0, 0, 3840, 2160);
    CHECK(above.x == 36);
    CHECK(above.y == -1040);
}

TEST(d3d12_xs_window_size_text_parses) {
    CHECK(parse_size("1280x720") == (Size{1280, 720}));
    CHECK(parse_size("1920X1080") == (Size{1920, 1080}));
    CHECK(parse_size("  1920 x 1080  ") == (Size{1920, 1080}));
    CHECK(parse_size("32767x1") == (Size{32767, 1}));
    CHECK(!parse_size("").has_value());
    CHECK(!parse_size("    ").has_value());
    CHECK(!parse_size("1920").has_value());
    CHECK(!parse_size("1920x").has_value());
    CHECK(!parse_size("x1080").has_value());
    CHECK(!parse_size("0x1080").has_value());
    CHECK(!parse_size("1920x0").has_value());
    CHECK(!parse_size("-1920x1080").has_value());
    CHECK(!parse_size("1920x1080x2").has_value());
    CHECK(!parse_size("widexhigh").has_value());
    CHECK(!parse_size("32768x1080").has_value());
    CHECK(!parse_size("1920x1080 pixels").has_value());
}

TEST(d3d12_xs_window_forced_size_comes_from_the_environment) {
    unset("G6SHIM_WINDOW");
    CHECK(!forced_size().has_value());
    CHECK(requested_size(Size{3840, 2160}) == (Size{3840, 2160}));
    ::SetEnvironmentVariableA("G6SHIM_WINDOW", "800x600");
    CHECK(forced_size() == (std::optional<Size>(Size{800, 600})));
    CHECK(requested_size(Size{3840, 2160}) == (Size{800, 600}));
    ::SetEnvironmentVariableA("G6SHIM_WINDOW", "800x");
    CHECK(requested_size(Size{320, 200}) == (Size{320, 200}));
    unset("G6SHIM_WINDOW");
    CHECK(requested_size(Size{320, 200}) == (Size{320, 200}));
}

TEST(d3d12_xs_window_fullscreen_covers_the_monitor) {
    unset("G6SHIM_FULLSCREEN");
    CHECK(!fullscreen_requested());
    ::SetEnvironmentVariableA("G6SHIM_FULLSCREEN", "1");
    CHECK(fullscreen_requested());
    const Placement full = fullscreen_placement(Rect{1920, -120, 3840, 960});
    CHECK(full.x == 1920);
    CHECK(full.y == -120);
    CHECK(full.client == (Size{1920, 1080}));
    ::SetEnvironmentVariableA("G6SHIM_FULLSCREEN", "0");
    CHECK(!fullscreen_requested());
    ::SetEnvironmentVariableA("G6SHIM_FULLSCREEN", "");
    CHECK(!fullscreen_requested());
    unset("G6SHIM_FULLSCREEN");
    CHECK(!fullscreen_requested());
}
