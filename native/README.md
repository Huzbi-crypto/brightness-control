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
| Hook the view lifecycle to get the XAML root | **done** - hook fires with the view object |
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

## The hook

`src/bcdll.cpp` installs a 5-byte `E9` detour on `ControlCenterView::OnGotFocus`
and logs the implementation object it receives:

```
14:54:13.048  OnGotFocus: rva=0xC0950 va=0x7FFFFDD10950 first bytes 40 53 48 83 EC
14:54:13.051  OnGotFocus hooked; trampoline=0x7FFFFDC40000 hook=0x7FF81ABE1000
14:54:27.330  OnGotFocus fired: this=0x1EF27078D78 args=0x1EF30EEC8C0
```

Two details that make it safe:

- The relay stub is allocated with `VirtualAlloc` scanning outward from the
  target, because `E9 rel32` only reaches 2 GB. The trampoline itself ends in an
  absolute `mov rax, imm64; jmp rax`, so it has no distance limit.
- The hook refuses to install if the target already begins with `E9`. With the
  Windhawk mod enabled that is exactly what is there, and patching over another
  detour's trampoline corrupts it.

This is why the mod has to be disabled while developing:

```powershell
Set-ItemProperty HKLM:\SOFTWARE\Windhawk\Engine\Mods\per-monitor-brightness -Name Disabled -Value 1
Restart-Service Windhawk -Force
```

## What the view actually is

`GetRuntimeClassName` gives `ControlCenter.ControlCenterView`, and `GetIids`
gives 53 interfaces. The relevant ones, resolved against the SDK headers:

| Interface | Present |
|---|---|
| `IUserControl` | yes |
| `IControl` (+2..7) | yes |
| `IFrameworkElement` (+2..7) | yes |
| `IUIElement` (+2..10) | yes |
| `IDependencyObject` (+2) | yes |
| `IComponentConnector` (+2) | yes |
| `IControlOverrides`, `IUIElementOverrides*` | yes |
| **`IContentControl`** | **no** - `QI` returns `0x80004002` |

That last row is the useful part. Despite the `UserControl` shape, the class does
not implement `IContentControl`, so there is no `Content` property to read the
panel's root from. The way in is `VisualTreeHelper` on the view itself, which is
a `UIElement` - walk its visual children until the card container turns up.

`IComponentConnector` being present also confirms `Connect` is a real hook
target, matching what the PDB said.

## Open problem: WinRT calls inside the hook kill ShellHost

The detour itself is sound. A pass-through hook - log, then call the original -
runs indefinitely with the panel opening and closing normally:

```
15:30:10.122  hook entered: this=0x19F6C0A31D8 args=0x19F77F6AB80
```

As soon as the hook touches WinRT, ShellHost dies with `0xc000001d`
(STATUS_ILLEGAL_INSTRUCTION, faulting module "unknown") a fraction of a second
later - *after* the work has visibly succeeded:

| Probe | Result |
|---|---|
| pass-through, no WinRT | stable |
| one `GetChildrenCount` | `0x00000000 count=1`, then crash |
| full visual tree walk, depth 12 | whole tree logged, `walk complete`, then crash |
| tree walk + `IFrameworkElement` size reads | same |

So the data is correct and the crash comes after the hook returns, which points
at the XAML state or the stack frame rather than at the vtable layout - the
calls themselves return valid values.

Next things to try, in order:

1. Do the WinRT work on a `DispatcherQueue` callback posted from the hook, so
   nothing XAML-touching runs inside the focus event itself.
2. Same, but posted for the *next* dispatcher turn rather than inline.
3. Check stack alignment across the relay: the hook is entered by `jmp`, not
   `call`, and the trampoline is then reached by `call` from the hook.

## Where it stands

The crash is fixed and the whole pipeline runs inside ShellHost:

```
hook entered: this=0x232FD9A5C18
TryEnqueue: 0x00000000 queued=1
FOUND SlidersGroup at depth 9: ContentControl 358x125
captured ItemsStackPanel: 332x105
insert: QI IPanel 0x00000000
insert: get_Children 0x00000000
insert: RoActivateInstance 0x00000000
insert: Append 0x00000000
insert: children 2 -> 3
```

The sliders area is:

```
SlidersGroup (ContentControl 358x125)
  ContentPresenter -> GridView 332x114 -> Border -> ScrollViewer -> Grid
    ScrollContentPresenter -> ItemsPresenter
      ItemsStackPanel 332x105
        GridViewItem 332x44   <- brightness
        GridViewItem 332x44   <- volume
```

**Not done:** the appended element does not render. `Append` returns `S_OK` and
the child count goes 2 -> 3, but the `GridView` re-templates its items on the
next layout pass and drops a raw child from the virtualizing `ItemsStackPanel`.
The row needs to be built as a real `GridViewItem` with an explicit size and
inserted through the item source, or added as a sibling of the `GridView`
inside a panel the shell does not virtualize.
