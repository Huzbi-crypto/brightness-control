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
server, which we do not have.

**RTTI does not work here** (tried, `tools/rtti_probe.cpp`): the class has no
RTTI type descriptor in the DLL, so there is no vtable to walk. The
`ControlCenterView` string at `0x35B2F8` is a source path
(`...\controlcenter\dll\ControlCenterView.cpp`), not a descriptor, and
searching for the decorated form `.?AVControlCenterView` finds nothing - RTTI
is simply not emitted for this component.

So the route is symbols, the same way Windhawk does it - and it works
(`tools/symtool.cpp`):

```
PDB   : ControlCenter.pdb
GUID  : 69549732A6CF1C43FDF7ED1FD2227E421 (age 1)
```

Both names resolve, and the addresses are confirmed independently: with the
per-monitor-brightness mod enabled, both functions already start with
`E9 .. .. .. ..` (a 5-byte jump) plus `CC` padding, which is Windhawk's own
detour sitting on exactly these RVAs.

| Target | RVA | Original prologue |
|---|---|---|
| `ControlCenterView::OnGotFocus` | `0xC0950` | `40 53` push rbx; `48 83 EC 40` sub rsp,40; `48 8B 05 E3 36 2F 00` mov rax,[rip+2F36E3]; `48 33 C4` xor rax,rsp |
| `ControlCenterView::Connect` | `0xA5490` | `48 83 EC 48` sub rsp,48; `48 8B 05 A5 EB 30 00` mov rax,[rip+30EBA5]; `48 33 C4` xor rax,rsp |

Notes that shape the detour:

- `OnGotFocus` gives a clean 5-byte steal (`push rbx` + `sub rsp,0x40`), so a
  plain `E9 rel32` patch needs no relocation.
- `Connect` starts with a 4-byte `sub rsp,0x48`, so 5 bytes is not an
  instruction boundary; it needs an 11-byte steal that includes the
  RIP-relative stack-cookie load. That one instruction has to be relocated
  (`disp32 -= trampoline_addr - original_addr`); everything else in the stolen
  span is position independent.
- `Connect` is ICF-folded with the `Connect` of several other classes in the
  same DLL (same address), so the hook must decide whether the object is a
  ControlCenterView instead of trusting the address.

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
