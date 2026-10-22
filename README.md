# GTAVR: Enhanced

VR support for Grand Theft Auto V using OpenVR, some reverse engineering of the game's renderer, and Script Hook V.

This is a continuation of [veryjos/GTAV_OpenVR](https://github.com/veryjos/GTAV_OpenVR), the original GTA: Vive by Joseph Delgado. This release is for **GTA V Enhanced** (D3D12). The original D3D11 path for GTA V Legacy is still in the code; a Legacy release will follow separately.

## What's new since the original

- **GTA V Enhanced support**: a D3D12 capture path next to the original D3D11 one.
- **GTAVive.asi source**: the script half was rebuilt from the 2016 alpha binary and checked against it, so the whole mod now builds from source (`GTAVive/`, see `GTAVive/RECONSTRUCTION.md`).
- **Smoother head tracking**:
  - a head pose sampled at the game frame's own time (`[Timing] PoseTiming`);
  - a camera stabilizer with Jitter / Manual / Adaptive presets that you can tune live;
  - a guard against tracking jumps.
- **Room scale**: optional amplified walking for small rooms.
- **Gameplay options** (`gtaovr\gtavive.ini`):
  - move where you look, and a camera locked to the character;
  - stick turning and a soft follow;
  - the view tilting with vehicles and with a falling character;
  - VR height offset;
  - re-centre view, VR off/on in game, and auto-start VR.
- **In-game settings menu** (Ctrl+F6): every gameplay, stabilizer and room scale option, live and saved to the ini files.
- **Rebindable keys**: every keyboard function can be changed in the menu or in the ini files.
- VR mode starts on **Ctrl+L**, not plain L.
- Built with Visual Studio 2022.

## Install

1. Install [Script Hook V](http://www.dev-c.com/gtav/scripthookv/) with an ASI loader.
2. Copy `GTAVive.asi`, `OVRInject.dll`, `openvr_api.dll` and the `gtaovr` folder from the release zip into the GTA V Enhanced folder (next to `GTA5_Enhanced.exe`).
3. Start SteamVR, then the game.
4. In story mode press **Ctrl+L** to enter VR.
5. On GTA V Enhanced, turn HDR off.

**Optional:** `Optional\VR Switch.bat` switches the mod on or off without deleting anything. Copy the `Optional` folder into the game folder (or put the file next to `GTA5_Enhanced.exe`) and run it from there.

## Controls

**Keyboard**

These are the defaults. Every key can be changed in the settings menu (Keys page) or in the ini files:
`[Keys]` in `gtaovr\gtavive.ini` for GTA: Vive's own keys, `[Hotkeys]` in `gtaovr\stabilizer.ini` for the stabilizer and room scale keys. The ini name of each key is in brackets.

| Key | Action |
|---|---|
| Ctrl+F6 | Open / close the settings menu (`OpenMenu`) |
| Ctrl+L | Enter VR; while in VR: re-create the hand pistol (`EnterVR`) |
| Ctrl+Shift+L | VR off / back on (`VROff`) |
| Ctrl+Shift+C | Re-centre the view inside the character (`RecentreView`) |
| 1 | Toggle touchpad fly-move (`FlyMove`) |
| 2 | Toggle left-controller vehicle steering (`LeftHandSteering`) |
| U | Write the melee prop calibration values to `gravovrvalues.txt` (`MeleeCalibrationDump`) |
| Ctrl+Shift+F8 | Stabilizer on/off (`ToggleKey`) |
| Ctrl+Shift+F9 | Reload `stabilizer.ini` (`ReloadKey`) |
| Ctrl+Shift+P | Stabilizer preset: Jitter / Manual / Adaptive (`PresetKey`) |
| Ctrl+Shift+A | Choose what Up/Down adjust (`TargetKey`) |
| Ctrl+Shift+Up / Down | More / less smoothing (`UpKey` / `DownKey`) |
| Ctrl+Shift+S | Save live stabilizer values (`SaveKey`) |
| Ctrl+Shift+Home | Room scale re-anchor (`ReanchorKey`) |

A key bind is optional modifiers and one key joined with `+`, for example `Ctrl+Shift+F8`, `Alt+Home` or `K`. Matching is exact: a bind without modifiers does not fire while Ctrl, Shift or Alt is held. The full list of key names is in `gtavive.ini`.

The game's character switch (F5-F8 and the Alt wheel) is disabled while the mod runs.

**Settings menu**

Press **Ctrl+F6** (`OpenMenu`) to open it, with VR on or off; it is drawn on the game image, so it also shows in the headset. Up/Down select a row, Left/Right change a value (hold to repeat), Enter opens or toggles, Backspace or Esc goes back. While the menu is open these keys, attack and aim do not reach the game. The navigation keys can be rebound too (`MenuUp`, `MenuDown`, `MenuLeft`, `MenuRight`, `MenuSelect`, `MenuBack`); Esc always goes back.

| Page | Contents |
|---|---|
| Gameplay | Every `[Beta]` option in `gtavive.ini` |
| Stabilizer | Stabilizer on/off, preset, position / rotation switches, the active preset's values; room scale on/off, mode and walk gain; pose timing |
| Keys | Every bind above. Select a row, press Enter, then press the new key combination (Esc cancels). Binds used twice are shown in orange. "Reset keys to defaults" restores them all |
| Actions | VR off / on, re-centre view |

Every change applies at once and is saved to `gtavive.ini` or `stabilizer.ini` straight away.

**Right controller**

| Button | Action |
|---|---|
| Trigger | Fire |
| Menu | Switch weapon |
| Grip | Melee; in a vehicle, re-centre the seat view |
| Touchpad press | Aim the teleport arc, release to teleport (aim at a car to take it) |
| Touchpad touch | Fly-move when enabled; steer in a vehicle |

## Settings

- `gtaovr\gtavive.ini`: gameplay options (`[Beta]`) and key binds (`[Keys]`). Every key is commented in the file.
- `gtaovr\stabilizer.ini`: stabilizer, room scale and pose timing. It is created with defaults if missing.

Both files are re-read while the game runs, and the settings menu edits both.

## Building

Visual Studio 2022 (v143 toolset, x64). Put the import libraries `ScriptHookV.lib`, `libMinHook.x64.lib` and `openvr_api.lib` in `ThirdParty\`, then run:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

The output goes to `x64\Release\`: `OVRInject.dll` and `GTAVive.asi`.

## Credits

- Joseph Delgado ([veryjos](https://github.com/veryjos)): original GTA: Vive and OVRInject.
- Alexander Blade: Script Hook V.
- theBmag: [VRcam](https://github.com/theBmag/VRcam), the GTA V camera mod whose ideas helped shape this project.
- Valve: OpenVR. Tsuda Kageyu: MinHook. Cameron Desrochers: readerwriterqueue.
