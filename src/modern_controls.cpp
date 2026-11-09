// SPDX-License-Identifier: GPL-3.0-or-later
// Camera vector operations and disc offsets shared with the keyboard-mouse mod.
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
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
    bool enabled, mouse_enabled, pc_layout, pal;
    int mx, my, sx, sy, deadzone, response, controller_level, aim_speed, move_speed;
    bool active = false, held = false, seen = false;
    uint32_t tick = 0;
    float yaw = 0, pitch = 0, tilt = 0;
    openrac::UiPage page, pc_page;
    kbm::Bindings pc_binds;
    kbm::KeyStick pc_move;
    kbm::RingPointer pc_ring;
    kbm::Latch pc_latch;
    kbm::Virtual pc_pad;
    uint32_t pc_tick = UINT32_MAX;
    bool pc_ring_open = false;
    std::filesystem::path preferences;
    bool save_failed = false;
    uint32_t invert_lr = 0, invert_ud = 0;
    bool port_statics = false;
    struct MenuPatch {
        uint32_t items = 0;
        int extra = 0;
        bool hints = false;
        bool pictures = false;
    };
    std::vector<MenuPatch> menu_patches;
};

std::unique_ptr<State> g;

template <class T>
T read(uint32_t p);

template <class T>
void write(uint32_t p, T v);

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

void show_camera() {
    const int level = g->controller_level;
    const bool lr = read<uint8_t>(g->invert_lr) != 0;
    const bool ud = read<uint8_t>(g->invert_ud) != 0;
    openrac::UiContent content;
    content.title = words("Camera", "Caméra", "Kamera", "Cámara", "Telecamera");
    content.items = {
        openrac::UiItem::choice_of(
            words(
                "Movement left/right",
                "Mouvement gauche/droite",
                "Bewegung nach links/rechts",
                "Movimiento izquierda/derecha",
                "Movimento sinistra/destra"
            ),
            {words("Normal", "Normal", "Normal", "Normal", "Normale"),
             words("Inverted", "Inversé", "Umgekehrt", "Invertido", "Invertito")},
            lr ? 1u : 0u
        ),
        openrac::UiItem::choice_of(
            words(
                "Movement up/down",
                "Mouvement haut/bas",
                "Bewegung nach oben/unten",
                "Movimiento arriba/abajo",
                "Movimento su/giù"
            ),
            {words("Normal", "Normal", "Normal", "Normal", "Normale"),
             words("Inverted", "Inversé", "Umgekehrt", "Invertido", "Invertito")},
            ud ? 1u : 0u
        ),
        // Three moving choices give 100 exact positions without exceeding the SDK's
        // 64-choice limit. Keep the selection centered so Left/Right always step by one.
        openrac::UiItem::choice_of(
            words(
                "Camera rotation speed",
                "Vitesse de rotation caméra",
                "Geschwindigk. d. Kameradrehung",
                "Velocidad de giro de cámara",
                "Velocità rotazione camera"
            ),
            {modern::bar(level - 1), modern::bar(level), modern::bar(level + 1)},
            1
        ),
        openrac::UiItem::button(words(
            "PC control layout", "Commandes PC", "PC-Steuerung", "Controles de PC", "Comandi PC"
        ))
    };
    if (g->save_failed) {
        content.footer = {words(
            "Could not save camera speed",
            "Vitesse caméra non enregistrée",
            "Kameratempo nicht gespeichert",
            "No se pudo guardar la velocidad",
            "Velocità camera non salvata"
        )};
    }
    g->page.set(content);
}

void show_pc_layout() {
    openrac::UiContent content;
    content.title = words(
        "PC Controls", "Commandes PC", "PC-Steuerung", "Controles de PC", "Comandi PC"
    );
    content.items = {
        openrac::UiItem::text(words("Move        W A S D", "Déplacer     W A S D", "Bewegen      W A S D", "Mover        W A S D", "Movimento    W A S D")),
        openrac::UiItem::text(words("Camera      Mouse", "Caméra       Souris", "Kamera       Maus", "Cámara       Ratón", "Telecamera   Mouse")),
        openrac::UiItem::text(words("Jump        Space", "Sauter       Espace", "Springen     Leertaste", "Saltar       Espacio", "Salta        Spazio")),
        openrac::UiItem::text(words("Fire        Left Mouse", "Tirer        Clic gauche", "Feuern       Linke Maus", "Disparar     Clic izq.", "Fuoco        Mouse sin.")),
        openrac::UiItem::text(words("Quick Select Right Mouse", "Sélection    Clic droit", "Schnellausw. Rechte Maus", "Selección    Clic der.", "Selezione    Mouse dest.")),
        openrac::UiItem::text(words("Wrench      F / Middle Mouse", "Clé          F / Clic milieu", "Schlüssel    F / Mittlere Maus", "Llave        F / Clic central", "Chiave       F / Mouse centro")),
        openrac::UiItem::text(words("Crouch      Ctrl / C", "Accroupir    Ctrl / C", "Ducken       Strg / C", "Agacharse    Ctrl / C", "Accovaccia   Ctrl / C")),
        openrac::UiItem::text(words("Strafe      Shift", "Pas latéral  Maj", "Seitwärts    Umschalt", "Lateral      Mayús", "Laterale     Maiusc")),
        openrac::UiItem::text(words("First person V / Mouse 4", "1re personne V / Souris 4", "Ego-Ansicht  V / Maus 4", "1a persona   V / Ratón 4", "Prima pers.  V / Mouse 4")),
        openrac::UiItem::text(words("Map         Tab / M", "Carte        Tab / M", "Karte        Tab / M", "Mapa         Tab / M", "Mappa        Tab / M")),
        openrac::UiItem::text(words("Weapons     1-8 / Wheel", "Armes        1-8 / Molette", "Waffen       1-8 / Mausrad", "Armas        1-8 / Rueda", "Armi         1-8 / Rotella")),
        openrac::UiItem::text(words("Pause / Back Esc / P", "Pause/Retour Échap / P", "Pause/Zurück Esc / P", "Pausa/Atrás  Esc / P", "Pausa/Indietro Esc / P"))
    };
    content.footer = {
        g->pc_layout
            ? words("PC layout active; gamepad stays active", "Disposition PC active; manette active", "PC-Layout aktiv; Gamepad bleibt aktiv", "Diseño PC activo; mando activo", "Layout PC attivo; gamepad attivo")
            : words("Enable PC layout in the mod settings", "Activer la disposition PC dans le mod", "PC-Layout in den Mod-Einstellungen aktivieren", "Activa el diseño PC en los ajustes", "Attiva il layout PC nelle impostazioni")
    };
    g->pc_page.set(content);
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

bool port_statics_mapped(const openrac::Memory& memory) {
#if defined(__APPLE__)
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.base());
    auto protection = [](uintptr_t target, vm_prot_t want, vm_prot_t reject) {
        mach_vm_address_t address = static_cast<mach_vm_address_t>(target);
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const kern_return_t result = mach_vm_region(
            mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object
        );
        if (object != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object);
        }
        return result == KERN_SUCCESS && address <= target
               && target < static_cast<uintptr_t>(address + size)
               && (info.protection & want) == want && (info.protection & reject) == 0;
    };
    // OpenRAC reserves a contiguous 4 GiB guest window. Its port region is RW while large gaps
    // around it remain PROT_NONE. A normal unit-test heap has no such layout.
    return protection(base + 0xE0200000u, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE)
           && protection(base + 0x40000000u, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
           && protection(base + 0xF0000000u, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
#else
    return false;
#endif
}

void add_camera_page(openrac::Mod& mod) {
    g->preferences = preferences_path(mod);
    g->port_statics = port_statics_mapped(mod.memory());
    g->invert_lr = mod.relocate(g->pal ? 0x15EEE0u : 0x15EDE0u);
    g->invert_ud = mod.relocate(g->pal ? 0x15EEDCu : 0x15EDDCu);
    if (!g->preferences.empty()) {
        std::ifstream in(g->preferences);
        if (in) {
            g->controller_level = modern::read_level(in, g->controller_level);
        }
    }
    if (!mod.has_ui() || !mod.has_pause_items()) {
        return;
    }
    g->pc_page = mod.ui_page([](const auto&, const openrac::UiEvent& event) {
        if (!event.is(OPENRAC_UI_HIDE)) {
            show_pc_layout();
        }
    });
    show_pc_layout();
    g->page = mod.ui_page([](const auto&, const openrac::UiEvent& event) {
        bool changed = false;
        if (event.is(OPENRAC_UI_CHANGE) && event.item() == 0 && event.value() >= 0
            && event.value() <= 1) {
            write<uint8_t>(g->invert_lr, event.value() != 0 ? 1 : 0);
        } else if (event.is(OPENRAC_UI_CHANGE) && event.item() == 1 && event.value() >= 0
                   && event.value() <= 1) {
            write<uint8_t>(g->invert_ud, event.value() != 0 ? 1 : 0);
        } else if (event.is(OPENRAC_UI_CHANGE) && event.item() == 2 && event.value() >= 0
                   && event.value() <= 2) {
            const int wanted = std::clamp(g->controller_level + event.value() - 1, 1, 100);
            changed = wanted != g->controller_level;
            g->controller_level = wanted;
        } else if (event.is(OPENRAC_UI_CHOOSE) && event.item() == 3) {
            g->pc_page.open();
        }
        if (changed) {
            save_controls();
        }
        if (!event.is(OPENRAC_UI_HIDE)) {
            show_camera();
        }
    });
    show_camera();
    try {
        // Keep exactly one visible camera entry in the German PAL menu. The native RAC1 Kamera
        // entry is removed below and this page takes its place with the 1-100 control.
        mod.ui_pause_item("Kamera", g->page);
    } catch (const openrac::Error&) {
        // The original Camera page stays available only when all pause Options slots are occupied.
        mod.warn(
            "Camera Options integration unavailable: free a pause Options slot; launcher adjustment "
            "still works"
        );
        g->port_statics = false;
    }
}

// OpenRAC builds the level Options list in its guest-static region. It must keep the game's Camera
// item while constructing the mod pages because that page is their native template. Once the list
// exists, remove only that original Camera item: our mod page then occupies Camera's role without a
// duplicate Camera/Kamera entry. The companion hint and picture arrays are shifted by the same
// one item so Subtitles and Quit keep their own description and picture.
constexpr uint32_t kPortStatics = 0xE0200000u;
constexpr uint32_t kPortStaticsEnd = 0xE0300000u;
constexpr uint32_t kMenuItemBytes = 0xCu;

constexpr int kPauseModMax = 4;

bool pause_mod_text(uint16_t text) {
    return text >= 0x7D00u && text < 0x7D00u + kPauseModMax;
}

bool plausible_picture(uint32_t at) {
    const uint32_t sector = read<uint32_t>(at);
    const uint32_t count = read<uint32_t>(at + 4);
    return sector != 0 && count != 0 && count <= 0x800u;
}

void patch_aux(State::MenuPatch& patch) {
    const uint32_t begin = patch.items;
    const uint32_t end = std::min(kPortStaticsEnd, begin + 0x20000u);
    if (!patch.hints) {
        for (uint32_t at = begin; at + 32 < end; at += 4) {
            if (read<uint32_t>(at) != 0x4F35u || read<uint32_t>(at + 4) != 0x7E0Au) {
                continue;
            }
            uint32_t p = at + 8;
            for (int i = 0; i < patch.extra - 1 && pause_mod_text(read<uint16_t>(p)); ++i, p += 4) {
            }
            if (read<uint32_t>(p) == 0x4F36u && read<uint32_t>(p + 4) == 0x4F37u
                && read<uint32_t>(p + 8) == 0x4F39u && read<uint32_t>(p + 12) == 0) {
                write<uint32_t>(p, read<uint32_t>(p + 4));
                write<uint32_t>(p + 4, read<uint32_t>(p + 8));
                write<uint32_t>(p + 8, 0u);
                patch.hints = true;
                break;
            }
        }
    }
    if (!patch.pictures) {
        for (uint32_t at = begin; at + 8u * static_cast<uint32_t>(patch.extra + 4) < end; at += 8) {
            if (!plausible_picture(at)) {
                continue;
            }
            uint32_t p = at + 8;
            bool zero = true;
            for (int i = 0; i < patch.extra; ++i, p += 8) {
                zero = zero && read<uint32_t>(p) == 0 && read<uint32_t>(p + 4) == 0;
            }
            if (!zero || !plausible_picture(p) || !plausible_picture(p + 8)
                || !plausible_picture(p + 16)) {
                continue;
            }
            // p is Camera; replace it with Subtitles, then Quit. The stale final slot is ignored
            // because the Options item list is now one entry shorter.
            write<uint32_t>(p, read<uint32_t>(p + 8));
            write<uint32_t>(p + 4, read<uint32_t>(p + 12));
            write<uint32_t>(p + 8, read<uint32_t>(p + 16));
            write<uint32_t>(p + 12, read<uint32_t>(p + 20));
            patch.pictures = true;
            break;
        }
    }
}

void integrate_camera_menu() {
    if (!g->port_statics) {
        return;
    }
    for (auto& patch : g->menu_patches) {
        if (!patch.hints || !patch.pictures) {
            patch_aux(patch);
        }
    }
    // The generated list is unique here: Sound, Video, up to four mod items, Camera, Subtitles,
    // Quit, end. Scan only the port's 1 MiB guest-static arena, not the game's data.
    for (uint32_t sound = kPortStatics;
         sound + static_cast<uint32_t>(kPauseModMax + 5) * kMenuItemBytes < kPortStaticsEnd;
         sound += 4) {
        if (read<uint16_t>(sound) != 0x4F22u || read<uint16_t>(sound + kMenuItemBytes) != 0x7E08u) {
            continue;
        }
        uint32_t camera = sound + 2 * kMenuItemBytes;
        int mods = 0;
        while (mods < kPauseModMax && pause_mod_text(read<uint16_t>(camera))) {
            ++mods;
            camera += kMenuItemBytes;
        }
        if (mods == 0 || read<uint16_t>(camera) != 0x4F23u
            || read<uint16_t>(camera + kMenuItemBytes) != 0x4F24u
            || read<uint16_t>(camera + 2 * kMenuItemBytes) != 0x4F26u
            || read<uint16_t>(camera + 3 * kMenuItemBytes) != 0) {
            continue;
        }
        const uint32_t items = sound - 3 * kMenuItemBytes;
        bool known = false;
        for (const auto& patch : g->menu_patches) {
            known = known || patch.items == items;
        }
        if (known) {
            continue;
        }
        // Copy Subtitles, Quit and the terminator over the original Camera item.
        for (int i = 0; i < 3; ++i) {
            for (uint32_t word = 0; word < kMenuItemBytes; word += 4) {
                write<uint32_t>(
                    camera + static_cast<uint32_t>(i) * kMenuItemBytes + word,
                    read<uint32_t>(camera + static_cast<uint32_t>(i + 1) * kMenuItemBytes + word)
                );
            }
        }
        g->menu_patches.push_back({items, 1 + mods, false, false});
        patch_aux(g->menu_patches.back());
        g->mod->info(
            "Modern Controls: native Kamera entry replaced by 1-100 page ("
            + std::to_string(mods) + " mod entries present)"
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

kbm::Context pc_context(int* dialog_kind) {
    const int level = g->mod->loaded_level();
    const int level_mode =
        level >= 0 ? read<int32_t>(g->mod->relocate_level(0, g->disc->level_mode)) : -1;
    const int boot_mode = level < 0 ? read<int32_t>(g->disc->boot_mode) : -1;
    const auto context = kbm::context_of(level, level_mode, boot_mode);
    *dialog_kind = context == kbm::Context::Dialog
        ? read<int32_t>(g->mod->relocate(g->disc->dialog))
        : -1;
    return context;
}

bool pc_action(const openrac_input& in, kbm::Context context, kbm::Action action, bool pressed) {
    if (!g->pc_layout || !in.focused || in.overlay) {
        return false;
    }
    const auto& info = kbm::kActions[static_cast<std::size_t>(action)];
    if (info.group == kbm::Group::Menu && context == kbm::Context::Play) {
        return false;
    }
    for (int bind : g->pc_binds.of(action)) {
        if (bind == 0) {
            continue;
        }
        if (kbm::is_mouse(bind)) {
            if (context == kbm::Context::Play && !in.captured) {
                continue;
            }
            if (context != kbm::Context::Play && !in.pointer_inside) {
                continue;
            }
        }
        if (pressed ? openrac_bind_pressed(&in, bind)
                    : (openrac_bind_held(&in, bind) || openrac_bind_pressed(&in, bind))) {
            return true;
        }
    }
    return false;
}

void pc_take_item(int item) {
    const uint32_t hero = g->mod->relocate(g->disc->hero);
    if (item != 0 && valid(hero) && item != read<int32_t>(hero + KBM_HERO_ITEM)) {
        write<int32_t>(hero + KBM_HERO_PENDING, item);
    }
}

void pc_weapon_hotkeys(const openrac_input& in, kbm::Context context, bool ring_open) {
    if (context != kbm::Context::Play || ring_open) {
        return;
    }
    const uint32_t hero = g->mod->relocate(g->disc->hero);
    if (!valid(hero) || read<uint8_t>(hero + KBM_HERO_VEHICLE) != 0) {
        return;
    }
    std::array<int32_t, kbm::kQuickSelectSlots> slots{};
    for (int i = 0; i < kbm::kQuickSelectSlots; ++i) {
        slots[static_cast<std::size_t>(i)] =
            read<int32_t>(g->disc->quick_select + static_cast<uint32_t>(i * 4));
        const auto action =
            static_cast<kbm::Action>(static_cast<int>(kbm::Action::Weapon1) + i);
        if (pc_action(in, context, action, true)) {
            pc_take_item(slots[static_cast<std::size_t>(i)]);
            return;
        }
    }
    const int step = pc_action(in, context, kbm::Action::WeaponNext, true) ? 1
                   : pc_action(in, context, kbm::Action::WeaponPrev, true) ? -1 : 0;
    if (step != 0) {
        const int32_t pending = read<int32_t>(hero + KBM_HERO_PENDING);
        const int32_t current = pending != 0 ? pending : read<int32_t>(hero + KBM_HERO_ITEM);
        pc_take_item(kbm::cycle_item(slots, current, step));
    }
}

void pc_layout_pad(uint8_t* report, int size, const openrac_input& in) {
    if (!g->pc_layout || !g->enabled || !in.focused || in.overlay) {
        g->pc_move = {};
        g->pc_ring.reset();
        g->pc_ring_open = false;
        return;
    }

    if (g->pc_tick != in.tick) {
        g->pc_tick = in.tick;
        int dialog_kind = -1;
        const auto context = pc_context(&dialog_kind);
        kbm::ActionSet held = 0;
        for (int i = 0; i < kbm::kActionCount; ++i) {
            const auto action = static_cast<kbm::Action>(i);
            if (pc_action(in, context, action, false)) {
                held |= kbm::bit(action);
            }
        }
        held = g->pc_latch.filter(context, held);
        const auto on = [&](kbm::Action action) {
            return (held & kbm::bit(action)) != 0;
        };

        g->pc_pad = {};
        g->pc_pad.held = kbm::buttons_for(context, held, dialog_kind);

        if (context == kbm::Context::Play) {
            g->pc_move.step(
                on(kbm::Action::MoveForward), on(kbm::Action::MoveBack),
                on(kbm::Action::MoveLeft), on(kbm::Action::MoveRight),
                on(kbm::Action::Walk), {0.5f, 4.0f}
            );
            g->pc_pad.left_x = g->pc_move.x;
            g->pc_pad.left_y = g->pc_move.y;

            const bool ring_open = on(kbm::Action::QuickSelect);
            if (ring_open) {
                g->pc_pad.held = static_cast<uint16_t>(g->pc_pad.held | kbm::kTriangle);
                if (!g->pc_ring_open) {
                    g->pc_ring.reset();
                }
                if (in.captured) {
                    g->pc_ring.add(in.mouse_dx, in.mouse_dy, 30.0f);
                    const auto stick = g->pc_ring.stick(30.0f);
                    g->pc_pad.left_x = stick.first;
                    g->pc_pad.left_y = stick.second;
                }
            } else if (g->pc_ring_open) {
                g->pc_ring.reset();
            }
            g->pc_ring_open = ring_open;
            pc_weapon_hotkeys(in, context, ring_open);
        } else {
            g->pc_move = {};
            g->pc_ring.reset();
            g->pc_ring_open = false;
            if (on(kbm::Action::MoveForward) || on(kbm::Action::MenuUp)) {
                g->pc_pad.held = static_cast<uint16_t>(g->pc_pad.held | kbm::kUp);
            }
            if (on(kbm::Action::MoveBack) || on(kbm::Action::MenuDown)) {
                g->pc_pad.held = static_cast<uint16_t>(g->pc_pad.held | kbm::kDown);
            }
            if (on(kbm::Action::MoveLeft) || on(kbm::Action::MenuLeft)) {
                g->pc_pad.held = static_cast<uint16_t>(g->pc_pad.held | kbm::kLeft);
            }
            if (on(kbm::Action::MoveRight) || on(kbm::Action::MenuRight)) {
                g->pc_pad.held = static_cast<uint16_t>(g->pc_pad.held | kbm::kRight);
            }
        }
    }

    kbm::merge(g->pc_pad, report, size);
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
    // KBM_FOLLOW_EYE is the previous collision-resolved camera position. Leave it to the
    // game's collision solver; changing it here can make the next trace start behind geometry.
    if (!(locks & 1) && yaw != 0) {
        orbit = kbm::rotate(orbit, up, yaw);
        smooth = kbm::rotate(smooth, up, yaw);
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
        g->tilt = now;
        g->held = true;
    }
    write(state + KBM_FOLLOW_ORBIT, orbit);
    write(state + KBM_FOLLOW_ORBIT_SMOOTH, smooth);
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
    if (g->mouse_enabled || g->pc_layout) {
        g->mod->capture_mouse(g->active);
    }
    const auto& in = g->mod->input().raw();
    pc_layout_pad(report, size, in);
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
    // Mirror the two native Camera options. The direct camera bypasses the original stick
    // reader, so their signs have to be applied here as well.
    if (read<uint8_t>(g->invert_lr) != 0) {
        yaw = -yaw;
    }
    if (read<uint8_t>(g->invert_ud) != 0) {
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
    g->pc_layout = mod.setting<bool>("pc_layout");
    for (int i = 0; i < kbm::kActionCount; ++i) {
        kbm::parse_binds(
            kbm::kActions[static_cast<std::size_t>(i)].fallback,
            g->pc_binds.binds[static_cast<std::size_t>(i)]
        );
    }
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
    add_camera_page(mod);
    if (g->enabled && g->pc_layout) {
        mod.take_input(OPENRAC_INPUT_KEYBOARD | OPENRAC_INPUT_MOUSE);
    } else if (g->enabled && g->mouse_enabled) {
        mod.take_input(OPENRAC_INPUT_MOUSE);
    }
    mod.on_pad(pad);
    mod.on_frame([] {
        bool active = playing();
        if (g->mouse_enabled || g->pc_layout) {
            g->mod->capture_mouse(active);
        }
        if (!active) {
            g->held = false;
            g->yaw = g->pitch = 0;
        }
        integrate_camera_menu();
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
OPENRAC_MOD_CPP("modern-controls", "0.4.0", load)

extern "C" OPENRAC_MOD_EXPORT void openrac_mod_unload() {
    if (g) {
        if (g->mouse_enabled || g->pc_layout) {
            g->mod->capture_mouse(false);
        }
        g.reset();
    }
}
