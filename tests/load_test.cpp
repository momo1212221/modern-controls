// SPDX-License-Identifier: GPL-3.0-or-later
// Load the real binary into a synthetic host: no game assets or running save needed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "openrac/openrac_mod.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
std::vector<uint8_t> memory(0x2000000);
openrac_input input{};
openrac_pad_fn pad;
void* pad_user;
openrac_frame_fn frame;
void* frame_user;
void (*camera_hook)(uint32_t);
void (*speed_hook)(float, float);
int phase = OPENRAC_PLAY_PLAYING;
float speed_up, speed_down, target_seen;
uint32_t hero;
int movement_setting = 100;
bool replace_target = false;
std::string failure, shown, title;
openrac_ui_fn ui_callback;
void* ui_user;
bool reject_pause = false;

void ui_event(uint32_t kind, int item = -1, int value = 0) {
    openrac_ui_event event{};
    event.size = sizeof event;
    event.kind = kind;
    event.item = item;
    event.value = value;
    ui_callback(ui_user, 0, &event);
}

template <class T>
T& at(uint32_t p) {
    return *reinterpret_cast<T*>(memory.data() + p);
}

struct V {
    float x, y, z;
};

void camera_original(uint32_t) {}

void speed_original(float up, float down) {
    speed_up = up;
    speed_down = down;
    target_seen = at<float>(hero + 0x190);
    if (replace_target) {
        at<float>(hero + 0x190) = 7;
    }
}

int hook(openrac_mod*, const char* id, const char*, void* fn, void** original) {
    if (std::strcmp(id, "rac1.camera.follow.apply_overrides") == 0) {
        camera_hook = reinterpret_cast<decltype(camera_hook)>(fn);
        *original = reinterpret_cast<void*>(camera_original);
    } else if (std::strcmp(id, "rac1.player.approach_speed") == 0) {
        speed_hook = reinterpret_cast<decltype(speed_hook)>(fn);
        *original = reinterpret_cast<void*>(speed_original);
    } else {
        return OPENRAC_HOOK_UNKNOWN;
    }
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    char directory[] = "/tmp/openrac-modern-controls-XXXXXX";
    CHECK(mkdtemp(directory));
    auto library = std::filesystem::path(directory) / std::filesystem::path(argv[1]).filename();
    std::filesystem::copy_file(argv[1], library);
    auto lib = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK(lib);
    auto load =
        reinterpret_cast<int (*)(const openrac_api*, openrac_mod*)>(dlsym(lib, "openrac_mod_load"));
    auto unload = reinterpret_cast<void (*)()>(dlsym(lib, "openrac_mod_unload"));
    CHECK(load && unload);
    for (bool pal : {true, false}) {
        for (int movement : {100, 150}) {
            movement_setting = movement;
            hero = pal ? 0x13F450 : 0x13F350;
            std::fill(memory.begin(), memory.end(), 0);
            failure.clear();
            input = {};
            input.focused = 1;
            input.captured = 1;
            openrac_api api{};
            api.size = offsetof(openrac_api, data_dir);
            api.api_major = 1;
            api.api_minor = 10;
            api.game = "rac1";
            api.version = pal ? "pal" : "ntsc";
            api.memory = memory.data();
            api.log = [](openrac_mod*, int, const char*) {
            };
            api.fail = [](openrac_mod*, const char* msg) {
                failure = msg;
            };
            api.setting_bool = [](openrac_mod*, const char* key) -> int {
                return std::strcmp(key, "invert_y") != 0;
            };
            api.setting_int = [](openrac_mod*, const char* key) -> int64_t {
                if (!std::strcmp(key, "mouse_x") || !std::strcmp(key, "mouse_y")) {
                    return 150;
                }
                if (!std::strcmp(key, "stick_x")) {
                    return 240;
                }
                if (!std::strcmp(key, "stick_y")) {
                    return 150;
                }
                if (!std::strcmp(key, "deadzone")) {
                    return 10;
                }
                if (!std::strcmp(key, "controller_level")) {
                    return 75;
                }
                if (!std::strcmp(key, "aim_speed")) {
                    return 75;
                }
                if (!std::strcmp(key, "move_speed")) {
                    return movement_setting;
                }
                return 175;
            };
            api.hook = hook;
            api.relocate_data = [](uint32_t p) {
                if (p == 0x15EE88u || p == 0x15ED88u) {
                    return p + 0x1000u;
                }
                return p;
            };
            api.relocate_level_data = [](int, uint32_t p) {
                return p;
            };
            api.loaded_level = []() {
                return 0;
            };
            api.input = []() -> const openrac_input* {
                return &input;
            };
            api.take_input = [](openrac_mod*, uint32_t) {
                return 0;
            };
            api.capture_mouse = [](openrac_mod*, int value) {
                input.captured = value;
            };
            api.on_pad = [](openrac_mod*, openrac_pad_fn f, void* u) {
                pad = f;
                pad_user = u;
                return 0;
            };
            api.on_frame = [](openrac_mod*, openrac_frame_fn f, void* u) {
                frame = f;
                frame_user = u;
                return 0;
            };
            api.play_status = [](openrac_play_status* s) {
                s->phase = phase;
                s->level = 0;
                return 0;
            };
            api.ui_page = [](openrac_mod*, uint32_t, openrac_ui_fn fn, void* user) {
                ui_callback = fn;
                ui_user = user;
                return 0;
            };
            api.ui_set = [](openrac_mod*, int, const openrac_ui_content* c) {
                CHECK(c->item_count == 3);
                CHECK(c->items[0].choice_count == 2);
                CHECK(c->items[1].choice_count == 2);
                CHECK(c->items[2].choice_count == 3);
                CHECK(c->items[2].choice == 1);
                title = c->title;
                shown = c->items[2].choices[1];
                return 0;
            };
            api.ui_pause_item = [](openrac_mod*, const char* label, int) {
                CHECK(std::string(label) == "Camera" || std::string(label) == "Kamera");
                return reject_pause ? OPENRAC_HOOK_UNKNOWN : 0;
            };
            CHECK(load(&api, reinterpret_cast<openrac_mod*>(1)) == 0);
            CHECK(failure.empty());
            const uint32_t cameras = pal ? 0x166D80 : 0x166C80;
            constexpr uint32_t camera = 0x100000, state = 0x110000;
            at<uint32_t>(cameras + 0x180) = camera;
            at<V>(cameras + 0x1b0) = {0, 0, 1};
            at<uint32_t>(camera + 0x70) = state;
            at<V>(state + 0x130) = {4, 0, 0};
            at<V>(state + 0x140) = {4, 0, 0};
            at<V>(state) = {4, 0, 0};
            uint8_t report[6] = {255, 255, 255, 128, 128, 128};
            input.tick = 1;
            pad(pad_user, 0, report, 6);
            CHECK(report[2] == 128);
            camera_hook(camera);
            // Physical right stick: 200% speed, with no mouse delta at all.
            CHECK(
                std::abs(at<V>(state + 0x130).y - 4 * std::sin(-240 * .01745329252f / 60 * 2))
                < .0001f
            );
            CHECK(report[4] == 128 && report[5] == 128);
            CHECK(
                std::abs(
                    at<V>(state + 0x130).x * at<V>(state + 0x130).x
                    + at<V>(state + 0x130).y * at<V>(state + 0x130).y - 16
                )
                < .001f
            );
            const float once = at<V>(state + 0x130).y;
            camera_hook(camera);
            CHECK(at<V>(state + 0x130).y == once);
            // Mouse deltas are proportional, consumed once even when the pad is polled twice.
            input.tick++;
            input.mouse_dx = 100;
            pad(pad_user, 0, report, 6);
            pad(pad_user, 0, report, 6);
            camera_hook(camera);
            CHECK(at<V>(state + 0x130).y < once);
            input.tick++;
            input.mouse_dx = 0;
            input.mouse_dy = 30;
            pad(pad_user, 0, report, 6);
            camera_hook(camera);
            CHECK(at<V>(state + 0x130).z > 0);
            at<float>(hero + 0x190) = 4;
            const float movement_gain = movement / 100.f;
            for (int n = 0; n < 10; ++n) {
                speed_hook(.2f, .3f);
                CHECK(std::abs(speed_up - .35f * movement_gain) < .0001f);
                CHECK(std::abs(speed_down - .525f * movement_gain) < .0001f);
                CHECK(target_seen == 4 * movement_gain);
                CHECK(at<float>(hero + 0x190) == 4);  // no accumulation across ticks
            }
            replace_target = true;
            speed_hook(.2f, .3f);
            replace_target = false;
            CHECK(
                at<float>(hero + 0x190) == 7
            );  // don't undo another hook's intentional target change
            at<float>(hero + 0x190) = 4;
            at<uint8_t>(hero + 0x20A4) = 1;
            speed_hook(.2f, .3f);
            CHECK(target_seen == 4 && speed_up == .2f && speed_down == .3f);
            at<uint8_t>(hero + 0x20A4) = 0;
            // Camera-volume locks suppress both axes, even with large mouse motion.
            at<int32_t>(state + 0x1b8) = 3;
            input.tick++;
            input.mouse_dx = 100;
            input.mouse_dy = 100;
            const V locked = at<V>(state + 0x130);
            pad(pad_user, 0, report, 6);
            camera_hook(camera);
            CHECK(
                at<V>(state + 0x130).x == locked.x && at<V>(state + 0x130).y == locked.y
                && at<V>(state + 0x130).z == locked.z
            );
            at<int32_t>(state + 0x1b8) = 0;
            // Menus and lost focus leave physical reports and acceleration unchanged.
            phase = OPENRAC_PLAY_MENU;
            input.tick++;
            report[2] = 255;
            pad(pad_user, 0, report, 6);
            CHECK(report[2] == 255);
            speed_hook(.2f, .3f);
            CHECK(speed_up == .2f && target_seen == 4);
            frame(frame_user);
            CHECK(!input.captured);
            phase = OPENRAC_PLAY_PLAYING;
            input.focused = 0;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(report[2] == 255);
            input.focused = 1;
            // Weapon selection and special camera modes retain their stick input.
            report[1] = 0xef;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(report[2] == 255);
            report[1] = 255;
            at<int16_t>(camera + 0x86) = 7;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(report[2] == 255);
            // First-person tilt, with bounded pitch, uses the direct path too.
            at<int16_t>(camera + 0x86) = 4;
            at<V>(camera) = {1, 0, 0};
            constexpr uint32_t moby = 0x120000;
            at<uint32_t>(hero + 0x2080) = moby;
            at<V>(moby + 0xE0) = {0, 1, 0};
            input.captured = 1;
            input.mouse_dx = 0;
            input.mouse_dy = 0;
            report[2] = 255;
            report[3] = 128;
            input.tick++;
            pad(pad_user, 0, report, 6);
            // Gravity-boots/rotated hero frames use Ratchet's moby up axis, not the follow camera up.
            CHECK(std::abs(at<V>(camera).y) < .0001f);
            CHECK(std::abs(at<V>(camera).z) > .0001f);

            // Clamp the stored tilt before computing the view delta: zero new pitch must not snap.
            at<V>(moby + 0xE0) = {0, 0, 1};
            at<V>(camera) = {1, 0, 0};
            at<float>(state + 0x10) = 1.2f;
            report[2] = report[3] = 128;
            input.mouse_dx = input.mouse_dy = 0;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(std::abs(at<V>(camera).x - 1) < .0001f && std::abs(at<V>(camera).y) < .0001f
                  && std::abs(at<V>(camera).z) < .0001f);

            report[2] = 128;
            input.mouse_dy = 100000;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(at<float>(state + 0x10) < 1.118f && at<float>(state + 0x10) > 1.116f);
            // Native settings: real 1..100 positions through three moving choices.
            at<int32_t>((pal ? 0x15EE88 : 0x15ED88) + 0x1000) = 3;
            ui_event(OPENRAC_UI_SHOW);
            CHECK(title == "Kamera");
            CHECK(shown.find("75/100") != std::string::npos);
            ui_event(OPENRAC_UI_CHANGE, 2, 2);
            CHECK(shown.find("76/100") != std::string::npos);
            input.mouse_dy = 0;
            input.mouse_dx = 0;
            at<V>(camera) = {1, 0, 0};
            report[2] = 255;
            input.tick++;
            pad(pad_user, 0, report, 6);
            CHECK(
                std::abs(at<V>(camera).y - std::sin(-240 * .01745329252f / 60 * 2.04f * .75f))
                < .0001f
            );
            for (int n = 0; n < 110; ++n) {
                ui_event(OPENRAC_UI_CHANGE, 2, 2);
            }
            CHECK(shown.find("100/100") != std::string::npos);
            for (int n = 0; n < 110; ++n) {
                ui_event(OPENRAC_UI_CHANGE, 2, 0);
            }
            CHECK(shown.find("1/100") != std::string::npos);
            // No separate Reset row: speed lives directly in the native Camera page.
            for (int n = 0; n < 49; ++n) {
                ui_event(OPENRAC_UI_CHANGE, 2, 2);
            }
            CHECK(shown.find("50/100") != std::string::npos);
            ui_event(OPENRAC_UI_CHANGE, 2, 2);
            CHECK(shown.find("51/100") != std::string::npos);
            unload();
            // On interface 1.10 the saved file beside the library survives a reload.
            CHECK(load(&api, reinterpret_cast<openrac_mod*>(1)) == 0);
            CHECK(shown.find("51/100") != std::string::npos);
            unload();
            reject_pause = true;
            CHECK(
                load(&api, reinterpret_cast<openrac_mod*>(1)) == 0
            );  // full Options slots don't unload the mod
            unload();
            reject_pause = false;
            std::filesystem::remove(std::filesystem::path(directory) / "controls.ini");
            phase = OPENRAC_PLAY_PLAYING;
        }
    }
    dlclose(lib);
    std::filesystem::remove_all(directory);
}
