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
| Injector loads a DLL into `ShellHost.exe` | **done** - verified, logs from pid 10432 |
| DLL reports from inside the host | **done** |
| Hook the view lifecycle to get the XAML root | next |
| Insert the brightness row (WinRT XAML) | not started |
| Wire the row to DDC/CI | not started |

### What the next step has to hook

The panel's root comes from `ControlCenterView` inside `ControlCenter.dll`, via
two of its **virtual** functions (not exports):

- `...::produce<ControlCenterView, IControlOverrides>::OnGotFocus(void*)`
- `...::produce<ControlCenterView, IComponentConnector>::Connect(int, void*)`

Both receive the implementation object, from which the XAML tree is reachable.
The reference resolves these by decorated name through Windhawk's symbol
server, which we do not have. The RTTI type descriptor for `ControlCenterView`
is present in the DLL (offset `0x35B2F8`), so the plan is to find the vtable
through RTTI and identify the two slots empirically - hook the candidates,
open the panel, see which fire with that signature.

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
