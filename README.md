# brightness-control

A small Windows tray app that drives an external monitor's brightness over DDC/CI, keeps it in step with the laptop panel, and shows a slider per display in one panel.

## The problem

Windows 11 Quick Settings has exactly one brightness slider, and it only moves the built-in panel. External monitors support DDC/CI, but Windows only exposes that per-display inside *Settings → System → Display*, never in the tray panel — and the Fn brightness keys only ever touch the laptop screen.

So you end up with two brightness controls in two places, and one of them is three clicks deep.

This app does two things about it:

- **Mirrors the laptop panel onto every DDC/CI monitor.** The existing slider — and the Fn keys — now move both screens together.
- **A tray panel** with one slider per display, for when you want to set them independently. It opens from the tray icon or `BrightnessControl.exe --show`, and can optionally take over Win+A (`intercept_wina=1`).

## How it works

| What | API |
|---|---|
| Internal panel, read + write | WMI `root\WMI` — `WmiMonitorBrightness` / `WmiMonitorBrightnessMethods.WmiSetBrightness` |
| External monitor, read + write | DDC/CI via `dxva2.dll` — `GetPhysicalMonitorsFromHMONITOR`, `GetVCPFeatureAndVCPFeatureReply`, `SetVCPFeature`, VCP code `0x10` |

All hardware access runs on one worker thread. The UI thread only posts target values into volatile fields — a DDC/CI write costs 30–100 ms and would otherwise stall slider drags.

Windows exposes no brightness-change event (`WmiMonitorBrightnessEvent` never fires on current builds), so the worker polls the internal panel every 400 ms and drops to 120 ms for 1.5 s after any change, which is enough to track a slider drag smoothly.

Win+A interception is opt-in and off by default. When enabled it uses a `WH_KEYBOARD_LL` hook: only the `A` is swallowed, and an unbound `F24` is injected between the Win down and up so the shell sees a combo rather than a bare Win tap and does not open Start. Modifier key-ups are never swallowed — doing that desyncs the OS modifier state and every later keystroke arrives as Win+key. A `WH_MOUSE_LL` hook closes the panel on the next click outside it.

## Requirements

- Windows 10/11 — uses .NET Framework 4.x, which ships with Windows
- The monitor must support DDC/CI **and** have it enabled in its OSD menu. Most Dell/HP/LG panels ship with it on; some ship with it off and report no brightness capability at all until you enable it.

## Build

No SDK required — the .NET Framework compiler is already on every Windows install:

```bat
build.cmd
```

which is just:

```bat
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /optimize+ ^
  /out:BrightnessControl.exe /r:System.dll /r:System.Management.dll ^
  /r:System.Drawing.dll /r:System.Windows.Forms.dll src\*.cs
```

## Install

Copy `BrightnessControl.exe` anywhere and run it. To start it with Windows, tick **Start with Windows** in the tray menu (it writes a small `.vbs` launcher into `shell:startup`), or make your own shortcut.

Configuration lives in `%LOCALAPPDATA%\BrightnessControl\config.ini`:

```ini
# mirror the laptop panel brightness onto external DDC/CI monitors
linked=1

# Win+A opens this app's panel instead of the Windows Quick Settings
intercept_wina=0
```

Leave `intercept_wina=0` to keep the native Quick Settings on Win+A; the panel stays reachable from the tray icon and from `BrightnessControl.exe --show`, which is handy for binding a different hotkey.

## Source layout

| File | Contents |
|---|---|
| `src/Display.cs` | WMI + DDC/CI interop, monitor enumeration, capability probing |
| `src/Program.cs` | Tray app, worker loop, Win+A and mouse hooks, config |
| `src/QuickPanel.cs` | The Win+A panel |
| `src/Controls.cs` | Slider, toggle switch, footer link (custom drawn) |
| `src/Hooks.cs` | Low-level keyboard/mouse hook host |

## Limitations

- **There is no way to put a slider inside the Quick Settings card.** This was investigated properly, and the results are worth recording so nobody repeats it:
  - The panel is `ControlCenterWindow`, owned by `ShellHost.exe`, created in a DWM compositor band. `EnumWindows`, `FindWindow` and `EnumThreadWindows` cannot see it at all; it can only be found with `WindowFromPoint` plus a class-name check.
  - It exposes no UI Automation provider — `AutomationElement.FromHandle` returns an element with zero descendants — so its internal geometry has to be measured from pixels.
  - `SetWindowBand` is refused with `ACCESS_DENIED` (5) for every band value, so a normal process cannot join or exceed that band.
  - A window placed over the card renders *behind* it: the card's acrylic surface blends the window underneath rather than being covered by it. Anything drawn ends up dimmed behind the glass or below the card.
  - That leaves DLL injection into `ShellHost.exe` and runtime XAML manipulation, which breaks on every feature update and is not worth running on a daily-driver machine. The native slider driving both monitors is the honest alternative.
- Shell flyouts (the media card) render in a higher compositor band than an ordinary topmost window, so the tray panel re-asserts topmost every 500 ms while it is open.
- DDC/CI calls fail transiently while a monitor wakes from sleep. Handles are re-enumerated on failure, but a change made in the first second after wake can be missed.
- External brightness is mapped linearly onto the monitor's VCP range (0–100 on most panels).
- Nothing here survives if the monitor's OSD has DDC/CI disabled — the app will simply find no external display to control.

## Verified on

Windows 11 26H2 (build 26300) · Intel UHD Graphics + NVIDIA MX250 · Dell P2423D over HDMI · laptop panel via WMI. Idle cost is ~40 MB RAM, ~0.2 % of one core, and 2.5 wakeups/s (the worker blocks on an event and only polls the panel on a timer).

## License

MIT
