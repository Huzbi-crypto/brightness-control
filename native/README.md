# native

The in-process implementation: a DLL that lives inside `ShellHost.exe` and adds a
brightness slider for the external monitor to the Quick Settings panel itself.

No service, no tray app, no polling process. The code runs in the address space
that is already there, so the marginal cost is a few hundred KB and no CPU when
the panel is closed.

## Status

Work in progress. Milestone 1 is the injector plus a DLL that proves it is
running inside `ShellHost`; the XAML work comes after that.

| Step | State |
|---|---|
| Injector loads a DLL into `ShellHost.exe` | in progress |
| DLL reports from inside the host | in progress |
| Catch `ControlCenter.dll`, hook the view lifecycle | not started |
| Insert the brightness row (WinRT XAML) | not started |
| Wire the row to DDC/CI | not started |

## Layout

| File | Purpose |
|---|---|
| `src/injector.cpp` | `bcinject.exe` — finds `ShellHost.exe`, injects the DLL, exits |
| `src/bcdll.cpp` | the DLL that gets injected |
| `build.cmd` | builds both with MSVC (locates it via `vswhere`) |

## Build

```bat
build.cmd
```

Needs MSVC Build Tools with the C++ workload (`Microsoft.VisualStudio.Workload.VCTools`).

## How it works, and why

Windows exposes no extension point for Quick Settings, and a normal process
cannot draw inside the panel: the Control Center is created in a higher DWM
compositor band (`SetWindowBand` returns `ACCESS_DENIED`) and a window placed
over it renders *behind* its acrylic surface. The panel's contents are a XAML
tree owned by `ShellHost.exe`.

So the only way to be genuinely inside it is to run in that process. The
approach, learned from the GPL-3.0 `per-monitor-brightness` Windhawk mod but
implemented independently here:

1. hook `kernelbase!LoadLibraryExW` to catch `ControlCenter.dll` loading into
   the host (it is not statically imported, so it arrives late);
2. hook the control center view's lifecycle to obtain its XAML element;
3. construct a `Windows.UI.Xaml.Controls.Slider` with C++/WinRT and insert it
   into the panel's tree;
4. drive the monitor over DDC/CI (`dxva2`, VCP `0x10`) — the same calls the C#
   app in `../src` already uses and has verified.

No code from that mod is copied; only the technique is shared.
