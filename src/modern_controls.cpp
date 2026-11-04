// SPDX-License-Identifier: GPL-3.0-or-later
// Camera vector operations and disc offsets shared with the keyboard-mouse mod.
#include <filesystem>
#include <fstream>
#include <memory>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "controls.hpp"
#include "discs.h"
#include "kbm.hpp"
#include "openrac/games/rac1.hpp"
#include "openrac/mod.hpp"

namespace {
using V = kbm::Vec3;

struct State {
    openrac::Mod* mod;
    const kbm_disc* disc;
    bool enabled, mouse_enabled, invert, pal;
    int mx, my, sx, sy, deadzone, response, controller_level, aim_speed, move_speed;
    bool active = false, held = false, seen = false;
    uint32_t tick = 0;
    float yaw = 0, pitch = 0, tilt = 0;
    openrac::UiPage page;
    std::filesystem::path preferences;
    bool save_failed = false;
};

std::unique_ptr<State> g;

// API 1.11 supplies a private writable data directory. On older runtimes use only
// this mod's library directory, preserving portable installations and avoiding global settings.
std::filesystem::path preferences_path(openrac::Mod& mod) {
    const auto directory = mod.data_dir();
    if (!directory.empty()) {
        return std::filesystem::path(directory) / "controls.ini";
    }
#if defined(_WIN32)
    HMODULE module = nullptr;
    wchar_t path[32768];
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g),
            &module
        )) {
        auto size = GetModuleFileNameW(module, path, 32768);
        if (size > 0 && size < 32768) {
            return std::filesystem::path(std::wstring(path, size)).parent_path() / "controls.ini";
        }
    }
#else
    Dl_info info{};
    if (dladdr(static_cast<const void*>(&g), &info) && info.dli_fname) {
        return std::filesystem::path(info.dli_fname).parent_path() / "controls.ini";
    }
#endif
    return {};
}

const char* words(const char* en, const char* fr, const char* de, const char* es, const char* it) {
    const auto address = g->mod->relocate(g->pal ? 0x15EE88u : 0x15ED88u);
    switch (g->mod->memory().read<int32_t>(address)) {
        case 2:
            return fr;
        case 3:
            return de;
        case 4:
            return es;
        case 5:
            return it;
        default:
            return en;
    }
}

void show_controls() {
    const int level = g->controller_level;
    openrac::UiContent content;
    content.title = words("Controls", "Commandes", "Steuerung", "Controles", "Comandi");
    // Three moving choices give 100 exact positions without exceeding the SDK's
    // 64-choice limit. Keep the selection centered so Left/Right always step by one.
    content.items =
        {openrac::UiItem::choice_of(
             words(
                 "Camera speed",
                 "Vitesse caméra",
                 "Kameratempo",
                 "Velocidad cámara",
                 "Velocità camera"
             ),
             {modern::bar(level - 1), modern::bar(level), modern::bar(level + 1)},
             1
         ),
         openrac::UiItem::button(
             words("Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina")
         )};
    if (g->save_failed) {
        content.footer = {words(
            "Could not save setting",
            "Enregistrement impossible",
            "Einstellung nicht gespeichert",
            "No se pudo guardar",
            "Impossibile salvare"
        )};
    }
    g->page.set(content);
}

void save_controls() {
    g->save_failed = true;
    if (!g->preferences.empty()) {
        std::error_code error;
        std::filesystem::create_directories(g->preferences.parent_path(), error);
        if (!error) {
            std::ofstream out(g->preferences);
            out << "camera_level=" << g->controller_level << '\n';
            out.flush();
            g->save_failed = !out;
        }
    }
    if (g->save_failed) {
        g->mod->warn("camera adjustment could not be saved");
    }
}

void add_controls_page(openrac::Mod& mod) {
    g->preferences = preferences_path(mod);
    if (!g->preferences.empty()) {
        std::ifstream in(g->preferences);
        if (in) {
            g->controller_level = modern::read_level(in, g->controller_level);
        }
    }
    if (!mod.has_ui() || !mod.has_pause_items()) {
        return;
    }
    g->page = mod.ui_page([](const auto&, const openrac::UiEvent& event) {
        bool changed = false;
        if (event.is(OPENRAC_UI_CHANGE) && event.item() == 0 && event.value() >= 0
            && event.value() <= 2) {
            int wanted = std::clamp(g->controller_level + event.value() - 1, 1, 100);
            changed = wanted != g->controller_level;
            g->controller_level = wanted;
        } else if (event.is(OPENRAC_UI_CHOOSE) && event.item() == 1) {
            g->controller_level = 50;
            changed = true;
        }
        if (changed) {
            save_controls();
        }
        if (!event.is(OPENRAC_UI_HIDE)) {
            show_controls();
        }
    });
    show_controls();
    try {
        mod.ui_pause_item("Controls", g->page);
    } catch (const openrac::Error&) {
        // The port permits two native Options entries across all mods. Keep controls
        // working if other mods already occupy them; don't refuse the entire library.
        mod.warn(
            "Controls Options entry unavailable: free a pause Options slot; launcher adjustment "
            "still works"
        );
    }
}

bool valid(uint32_t p) {
    return p >= 0x10000 && p < 0x02000000 - 0x400;
}

template <class T>
T read(uint32_t p) {
    return g->mod->memory().read<T>(p);
}

template <class T>
void write(uint32_t p, T v) {
    g->mod->memory().write<T>(p, v);
}

bool playing() {
    const auto& in = g->mod->input().raw();
    return g->enabled && in.focused && !in.overlay && g->mod->loaded_level() >= 0
           && g->mod->play_status().phase == OPENRAC_PLAY_PLAYING
           && read<int32_t>(g->mod->relocate_level(0, g->disc->level_mode)) == 0;
}

void follow(uint32_t camera) {
    if (!playing() || !g->active || !valid(camera)) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    uint32_t state = read<uint32_t>(camera + KBM_CAMERA_STATE);
    if (!valid(state)) {
        return;
    }
    float yaw = g->yaw, pitch = g->pitch;
    g->yaw = g->pitch = 0;  // one application per game input tick
    if (yaw == 0 && pitch == 0 && !g->held) {
        return;
    }
    const auto locks = read<int32_t>(state + KBM_FOLLOW_LOCKS);
    const auto block = g->mod->relocate_level(0, g->disc->cameras);
    if (!valid(block)) {
        return;
    }
    const auto up = kbm::unit_or(read<V>(block + KBM_CAMERAS_UP), kbm::kWorldUp);
    auto orbit = read<V>(state + KBM_FOLLOW_ORBIT);
    auto smooth = read<V>(state + KBM_FOLLOW_ORBIT_SMOOTH);
    auto target = read<V>(state + KBM_FOLLOW_TARGET);
    auto eye = kbm::minus(read<V>(state + KBM_FOLLOW_EYE), target);
    if (!(locks & 1) && yaw != 0) {
        orbit = kbm::rotate(orbit, up, yaw);
        smooth = kbm::rotate(smooth, up, yaw);
        eye = kbm::rotate(eye, up, yaw);
    }
    if (locks & 2) {
        g->held = false;
    } else if (pitch != 0) {
        if (!g->held) {
            g->tilt = kbm::elevation(orbit, up);
        }
        float now =
            std::clamp(g->tilt + pitch, -kbm::follow_max_elevation(), kbm::follow_max_elevation());
        float moved = now - g->tilt;
        auto tilt = [&](V v) {
            return kbm::with_elevation(
                v, up, std::clamp(kbm::elevation(v, up) + moved, -1.5f, 1.5f)
            );
        };
        orbit = tilt(orbit);
        smooth = tilt(smooth);
        eye = tilt(eye);
        g->tilt = now;
        g->held = true;
    }
    write(state + KBM_FOLLOW_ORBIT, orbit);
    write(state + KBM_FOLLOW_ORBIT_SMOOTH, smooth);
    write(state + KBM_FOLLOW_EYE, kbm::plus(target, eye));
    if (g->held) {
        float w = kbm::follow_target_of(g->tilt);
        write(state + KBM_FOLLOW_TILT, w);
        write(state + KBM_FOLLOW_TILT_RATE, 0.f);
        bool invert = read<int32_t>(g->mod->relocate(g->disc->tilt_invert)) != 0;
        write(g->mod->relocate(g->disc->pad_sticks) + 4, kbm::follow_stick_for(w, invert));
    }
}

void pad(int port, uint8_t* report, int size) {
    if (port != 0 || !report || size < 6) {
        return;
    }
    g->active = playing();
    if (g->mouse_enabled) {
        g->mod->capture_mouse(g->active);
    }
    const auto& in = g->mod->input().raw();
    if (!g->active) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    const auto block = g->mod->relocate_level(0, g->disc->cameras);
    if (!valid(block)) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    auto camera = read<uint32_t>(block + KBM_CAMERAS_CURRENT);
    if (!valid(camera)) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    const auto kind = read<int16_t>(camera + KBM_CAMERA_ID);
    // Keep special cameras, quick-select and vehicles under the game's control.
    if (kind != 0 && kind != KBM_CAMERA_FIRST_PERSON) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    // Triangle opens the weapon ring: preserve the complete report there.
    if ((report[1] & 0x10) == 0) {
        g->held = false;
        g->yaw = g->pitch = 0;
        return;
    }
    const auto axis = modern::stick(report[2], report[3], g->deadzone / 100.f);
    report[2] = report[3] = 128;  // no additional original stick easing
    if (g->seen && g->tick == in.tick) {
        return;
    }
    g->seen = true;
    g->tick = in.tick;
    const int aim = kind == KBM_CAMERA_FIRST_PERSON ? g->aim_speed : 100;
    float yaw = -modern::controller_rate(
        axis.x, g->sx, modern::controller_percent(g->controller_level), aim
    );
    float pitch = modern::controller_rate(
        axis.y, g->sy, modern::controller_percent(g->controller_level), aim
    );
    if (g->mouse_enabled && in.captured) {
        yaw -= modern::mouse(in.mouse_dx, g->mx);
        pitch += modern::mouse(in.mouse_dy, g->my);
    }
    if (g->invert) {
        pitch = -pitch;
    }
    if (kind == KBM_CAMERA_FIRST_PERSON) {
        g->held = false;
        auto state = read<uint32_t>(camera + KBM_CAMERA_STATE);
        if (!valid(state)) {
            return;
        }
        auto look = read<V>(camera);
        const auto hero = g->mod->relocate(g->disc->hero);
        const auto moby = valid(hero) ? read<uint32_t>(hero + KBM_HERO_MOBY) : 0;
        const auto up = valid(moby)
                            ? kbm::unit_or(read<V>(moby + 0xE0u), kbm::kWorldUp)
                            : kbm::kWorldUp;
        look = kbm::rotate(look, up, yaw);
        float old = read<float>(state + KBM_FIRST_PERSON_TILT);
        if (std::isfinite(old)) {
            const float wanted = old + pitch;
            const float now = std::clamp(
                wanted, -kbm::kFirstPersonTiltLimit, kbm::kFirstPersonTiltLimit
            );
            const float moved = std::clamp(
                old, -kbm::kFirstPersonTiltLimit, kbm::kFirstPersonTiltLimit
            );
            look = kbm::with_elevation(look, up, kbm::elevation(look, up) - (now - moved));
            write(state + KBM_FIRST_PERSON_TILT, now);
        }
        write(camera, look);
    } else {
        g->yaw = yaw;
        g->pitch = pitch;
    }
}

void load(openrac::Mod& mod) {
    if (mod.game() != "rac1" || (mod.version() != "pal" && mod.version() != "ntsc")) {
        throw openrac::Error("Modern Controls needs RAC1 PAL or NTSC-U");
    }
    g = std::make_unique<State>();
    g->mod = &mod;
    g->pal = mod.version() == "pal";
    g->disc = &k_kbm_discs[g->pal ? 0 : 1];
    g->enabled = mod.setting<bool>("enabled");
    g->mouse_enabled = mod.setting<bool>("mouse_enabled");
    g->invert = mod.setting<bool>("invert_y");
    auto setting = [&](const char* key, int lo, int hi) {
        return std::clamp(mod.setting<int64_t>(key), int64_t(lo), int64_t(hi));
    };
    g->mx = setting("mouse_x", 10, 1000);
    g->my = setting("mouse_y", 10, 1000);
    g->sx = setting("stick_x", 30, 720);
    g->sy = setting("stick_y", 30, 360);
    g->deadzone = setting("deadzone", 0, 40);
    g->response = setting("response", 100, 300);
    g->controller_level = setting("controller_level", 1, 100);
    g->aim_speed = setting("aim_speed", 25, 200);
    g->move_speed = setting("move_speed", 50, 200);
    add_controls_page(mod);
    if (g->mouse_enabled) {
        mod.take_input(OPENRAC_INPUT_MOUSE);
    }
    mod.on_pad(pad);
    mod.on_frame([] {
        bool active = playing();
        if (g->mouse_enabled) {
            g->mod->capture_mouse(active);
        }
        if (!active) {
            g->held = false;
            g->yaw = g->pitch = 0;
        }
    });
    mod.hook<openrac::rac1::FollowCamera_ApplyOverrides>([](auto original, uint32_t camera) {
        original(camera);
        follow(camera);
    });
    mod.hook<openrac::rac1::Hero_ApproachSpeed>([](auto original, float up, float down) {
        // Same target-speed hook used by difficulty-mod. Scale only during the call:
        // never keep a multiplier in the hero's target, so repeated ticks do not compound it.
        const auto hero = g->mod->relocate(g->disc->hero);
        if (!playing() || !valid(hero) || read<uint8_t>(hero + KBM_HERO_VEHICLE) != 0) {
            original(up, down);
            return;
        }
        const float gain = g->response / 100.f;
        const float speed = g->move_speed / 100.f;
        const auto address = hero + 0x190u;
        const float target = read<float>(address);
        if (g->move_speed == 100 || !std::isfinite(target)) {
            original(up * gain, down * gain);
            return;
        }
        const float scaled = target * speed;
        write(address, scaled);
        original(up * gain * speed, down * gain * speed);
        // Respect a target explicitly changed by the original or another chained mod.
        if (read<float>(address) == scaled) {
            write(address, target);
        }
    });
}
}  // namespace
OPENRAC_MOD_CPP("modern-controls", "0.3.0", load)

extern "C" OPENRAC_MOD_EXPORT void openrac_mod_unload() {
    if (g) {
        if (g->mouse_enabled) {
            g->mod->capture_mouse(false);
        }
        g.reset();
    }
}
