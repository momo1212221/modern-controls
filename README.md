# Modern Controls (experimental)

A separate RAC1 PAL / NTSC-U mod for a quicker, more adjustable camera and movement response.

Adjust camera speed live under **Pause → Options → Controls**. The page is translated into the five PAL languages, including German **Steuerung**. Its native Options entry is registered as “Controls” before the game initializes its language. Use Left/Right for exact one-step changes from **1 to 100**; the bar shows the current number and Reset returns to 50. Changes affect the next gameplay input tick and are saved for the next launch. The original Slow/Medium/Fast camera setting does not set the speed of this direct camera.

The remaining settings are under **Launcher → Mods → Modern Controls → Settings** and take effect after a game restart.

| Setting | Default | Range |
| --- | --- | --- |
| Mouse horizontal / vertical sensitivity | 150% each | 10–1000% |
| Controller camera fine adjustment | 50 | 1–100 |
| Controller first-person aim speed | 75% | 25–200% |
| Right stick horizontal base speed | 240 degrees/s | 30–720 |
| Right stick vertical base speed | 150 degrees/s | 30–360 |
| Right stick radial deadzone | 10% | 0–40% |
| Invert vertical look | Off | On / Off |
| Movement speed | 100% | 50–200% |
| Movement acceleration and braking | 175% | 100–300% |

Mouse, Steam Input gyro-to-mouse and trackpad-to-mouse turn the camera directly, without stick emulation, acceleration or smoothing. Stick rotation is linear outside a circular deadzone. Horizontal and vertical settings are independent. Mouse counts depend on the operating system / Steam Input sensitivity too. The mod uses its own vertical invert option for both input types.

The follow camera keeps its pitch when the stick or mouse stops, and its original camera-volume locks and pitch limits still apply. First-person aiming is also direct. Other camera modes, menus, cutscenes and the Triangle weapon ring keep the game's controls. The left stick retains its analog movement input. Movement speed independently scales the on-foot target speed; 100% keeps the original maximum run speed. Acceleration and braking have their own multiplier. Vehicle controls are unchanged. Jump and animation speeds are not scaled directly. Set movement response to 100% to retain the original acceleration/braking. Disable Modern Controls to return entirely to the game's controls.

## Controllers

Works with Xbox, PlayStation, Steam Deck and other gamepads that OpenRAC maps to its pad input; it does not depend on controller branding. **Controller camera fine adjustment (1–100)** is the main right-stick speed control. 1 gives 25% of the base rate, 50 gives 100%, 75 gives 200% and 100 gives 300%; all intermediate levels are distinct. **Controller first-person aiming speed (%)** is an additional multiplier for precision while aiming. For example, level 75 camera (200%) and 75% aiming gives 150% of the base speed in first person. These sliders leave mouse/gyro/trackpad sensitivity unchanged. Aiming affects the first-person camera, not every weapon's third-person fire mode.

**Movement speed (%)** separately changes how fast Ratchet moves on foot, including analog walking. Start at 100%; 120% increases the target speed by 20%. It applies to on-foot movement regardless of whether its input came from a controller or keyboard. Movement speed, acceleration/braking and camera speed are independent. Other speed mods multiply with this setting, so keep one of the speed multipliers at 100% when combining them.

The in-game camera slider takes effect immediately. Changes to other launcher settings take effect after restarting the game. No mouse is required for the controller improvements. Disable the mouse-camera setting for a controller-only configuration if you want the mouse left free.

## Steam Deck Pack

Keep the Steam Deck Pack enabled for its icons, but switch **Gyro / trackpad mouse camera** off in that pack. Modern Controls becomes the one mouse-camera provider. Its own mouse setting can be disabled for a stick-only setup. Do not combine it with Keyboard & Mouse; both control the camera.

The Controls page uses one native pause Options slot. The port currently permits two slots across all mods. If two other mods already occupy them, Modern Controls logs a warning and continues with launcher controls; free a slot to expose the live slider. With Multiplayer and the old Steam Deck mouse-only pack, disable that already-unused mouse pack and keep the separate icon mod. A current Steam Deck Pack can stay enabled if a slot is free; its own mouse camera must remain off.

The fine adjustment is saved in `controls.ini` in the SDK's per-mod data directory on interface 1.11+, or beside this library on older runtimes (including 1.10). Only this mod's own preference is written. A read-only mod directory still allows live adjustment, but the page reports when it cannot save. The saved in-game camera level takes precedence over the launcher default; remove that file to use the launcher's level again. Existing 0.2.0 launcher camera percentages should be migrated to the closest new level (100% → 50, 200% → 75, 300% → 100).

## Development and verification

Uses the SDK's stable `rac1.camera.follow.apply_overrides` and `rac1.player.approach_speed` hooks. Camera math and disc layouts are reused from `keyboard-mouse/src/kbm.hpp` and `discs.h`, under the repository's GPL-3.0-or-later license. Build through the root CMake project with SDK 1.9 or newer.

Tests cover radial deadzone boundaries, diagonal magnitude, mouse scaling, 60 Hz game-tick normalization (both OpenRAC disc builds), and camera vector length. A native-library host test exercises both discs on synthetic game RAM with interface 1.10: camera and first-person movement, duplicate polls, pitch bounds, movement speed/restoration, controller-only turn and aim scaling, vehicle passthrough, movement response, menus, focus loss, weapon selection and special-camera passthrough. Tests also exercise all 100 slider positions, endpoint clamping, reset, live rate changes, language selection, preference reload on interface 1.10, and graceful handling of occupied Options slots. No game assets are included.

This is a first implementation. Automated tests do not establish how it feels on hardware. Actual Mac / Steam Deck gameplay, unusual camera volumes, gravity boots and multiplayer need playtesting before proposing an upstream release. Other mods changing the same camera state may interact even when their hooks chain correctly.
