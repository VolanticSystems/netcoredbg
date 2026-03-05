# .NET Framework 4.8 Support for netcoredbg

## Goal

Extend netcoredbg to debug **.NET Framework 4.8** processes in addition to .NET Core/5+. The primary use case is debugging NinjaTrader 8 indicators and strategies, which run on the desktop CLR (.NET Framework 4.8).

This is a local fork of [Samsung/netcoredbg](https://github.com/Samsung/netcoredbg) (MIT license). Samsung does not merge external PRs, so this is maintained independently.

## Why netcoredbg

The open-source .NET debugger landscape has a gap: no production-ready, permissively-licensed, DAP-over-stdio debugger supports .NET Framework 4.8. Microsoft's vsdbg does, but its EULA restricts usage to Visual Studio and VS Code. netcoredbg is the most mature alternative (8+ years, actively maintained by Samsung, MIT licensed) and already implements DAP and GDB/MI protocols.

## Architecture: Why This Is Feasible

Both .NET Framework 4.8 (desktop CLR) and .NET Core (CoreCLR) use the **same ICorDebug COM interfaces** for debugging. The entire netcoredbg debugger engine — breakpoints, stepping, variables, stack traces, expression evaluation, threads — works through ICorDebug and requires **zero changes**.

The only CoreCLR-specific code is **runtime discovery and initialization**, isolated to ~130 lines across 3 files.

## What Needs to Change

### 1. Runtime Discovery (the core change)

**Current code (CoreCLR path via dbgshim.dll):**

- `src/debugger/dbgshim.h` — Loads `dbgshim.dll`, resolves 9 function pointers
- `src/debugger/manageddebugger.cpp`:
  - `AttachToProcess()` (~30 lines) — calls `EnumerateCLRs` → `CreateVersionStringFromModule` → `CreateDebuggingInterfaceFromVersionEx` → gets `ICorDebug*`
  - `RunProcess()` (~70 lines) — calls `CreateProcessForLaunch` → `RegisterForRuntimeStartup` → callback receives `ICorDebug*`
  - `Startup(IUnknown *punk)` (~45 lines) — takes `ICorDebug*`, calls `Initialize()`, `SetManagedHandler()`, `DebugActiveProcess()`. **This function is runtime-agnostic and needs no changes.**

**New code needed (.NET Framework path via mscoree.dll):**

```
mscoree.dll: CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost)
  → ICLRMetaHost::EnumerateLoadedRuntimes(processHandle)   // for attach
  → ICLRMetaHost::EnumerateInstalledRuntimes()              // for launch
  → ICLRRuntimeInfo::GetInterface(CLSID_CLRDebuggingLegacy, IID_ICorDebug)
  → ICorDebug*  // same interface, feeds into existing Startup()
```

The new path produces the same `ICorDebug*` that `Startup()` already expects. After that point, the entire debugger engine works unchanged.

**Estimated scope:** ~200-300 lines of new C++ code, plus conditional logic to select the right path.

### 2. Runtime Detection

Need a way to determine which CLR a target process is using:
- Check loaded modules for `clr.dll` (Framework) vs `coreclr.dll` (Core)
- For launch: user specifies via config, or detect from the executable

### 3. CLR Path Resolution

`GetCLRPath()` currently looks for `coreclr.dll`/`libcoreclr.so`. For Framework, the CLR lives at:
- `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\clr.dll` (64-bit)
- `C:\Windows\Microsoft.NET\Framework\v4.0.30319\clr.dll` (32-bit)

### 4. Managed Component Hosting (secondary concern)

netcoredbg hosts a managed C# helper DLL (`ManagedPart.dll`) for symbol reading and expression evaluation, loaded via CoreCLR hosting APIs in `src/managed/interop.cpp`. This uses `coreclr_initialize()` / `coreclr_create_delegate()`.

**Options:**
1. **Keep using CoreCLR for ManagedPart** (simplest) — debuggee runs on .NET Framework, debugger's helper runs on CoreCLR. Both runtimes installed side by side. ManagedPart already targets `netstandard2.0` which is compatible with both.
2. **Host on .NET Framework** — Use `ICLRRuntimeHost` instead. More work, eliminates CoreCLR dependency.
3. **Make ManagedPart optional** — Basic debugging works without it; expression evaluation and symbol reading degrade.

**Recommendation:** Option 1 for now. Requires .NET SDK installed alongside .NET Framework, which is the common case on dev machines.

## What Does NOT Change

- `src/debugger/` (75+ files) — entire debugger engine, pure ICorDebug
- `src/metadata/` — module loading, symbol reading, type printing
- DAP protocol layer (`src/protocols/`)
- GDB/MI protocol layer
- CLI protocol layer
- Managed C# code (`src/managed/`) — already targets netstandard2.0
- Build system (just add `mscoree.lib` linkage on Windows)
- All existing tests

## Key Files

| File | Role | Changes Needed |
|------|------|----------------|
| `src/debugger/dbgshim.h` | CoreCLR runtime loader | Add Framework alternative |
| `src/debugger/manageddebugger.h` | Debugger class declaration | Add runtime type field |
| `src/debugger/manageddebugger.cpp` | `AttachToProcess()`, `RunProcess()`, `Startup()` | Add Framework attach/launch paths |
| `src/debugger/managedcallback.cpp` | Debug event callbacks | Desktop CLR ManagedPart hosting |
| `src/debugger/desktopclr.h/cpp` | **NEW** Desktop CLR discovery | All new code |
| `src/managed/interop.cpp` | Managed component hosting | No change (option 1) |

## Implementation Phases

### Phase 1: Attach to a running .NET Framework process ✅ COMPLETE
- Added `src/debugger/desktopclr.h/cpp` — Desktop CLR discovery via mscoree/ICLRMetaHost
- Modified `AttachToProcess()` to detect CLR type and use Desktop CLR path when appropriate
- Updated `ManagedCallback::CreateProcess()` to host ManagedPart.dll via a system CoreCLR when debugging Desktop CLR (clr.dll can't host managed code the way coreclr.dll does)
- Added `FindHostingCoreCLRPath()` to locate a suitable CoreCLR for ManagedPart hosting
- Updated CMake to link `mscoree.lib` and include NETFXSDK headers
- **Requirement:** Test apps must be compiled with Portable PDB format (`/debug:portable` via Roslyn csc)
- **Tested:** Attach, breakpoints, stepping, stack traces, variable inspection all working

### Phase 2: Launch a .NET Framework process ✅ COMPLETE
- Added `IsFrameworkExecutable()` — PE header check for CLR data directory to detect Framework exes
- Added `CreateDesktopCLRDebuggingInterfaceForLaunch()` — gets ICorDebug via `ICLRMetaHost::GetRuntime("v4.0.30319")`
- Added `RunFrameworkProcess()` — uses `ICorDebug::CreateProcess` to launch under the debugger
- Process starts already under debugger control (no race conditions with CLR loading)
- Entry point hit, breakpoints, stepping, variable inspection all working
- **Tested:** Launch FrameworkTest.exe, entry-point-hit at Main(), breakpoint on line 24, step, variables

### Phase 3: Integration with mcp-debugger
- Wire up the netcoredbg fork as the adapter backend in `@debugmcp/adapter-dotnet`
- Replace the vsdbg bridge with netcoredbg (eliminates EULA concerns)
- **Test:** end-to-end debugging through MCP tools

### Phase 4: NinjaTrader attach
- Attach to NinjaTrader 8's process, debug indicators loaded in its AppDomain
- Handle NT8-specific quirks (custom hosting, multiple AppDomains, etc.)
- **Test:** set breakpoint in a NinjaTrader indicator, hit it, step through, inspect variables

Phase 1 is the proof of concept — if attach works, everything else follows.

## Windows-Only Scope

.NET Framework 4.8 only exists on Windows. The Framework-specific code paths will be `#ifdef WIN32` guarded. Linux/macOS builds are unaffected.

## References

- [ICorDebug Interface](https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/icordebug-interface)
- [ICLRMetaHost Interface](https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/hosting/iclrmetahost-interface)
- [CLRCreateInstance Function](https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/hosting/clrcreateinstance-function)
- [ICLRRuntimeInfo::GetInterface](https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/hosting/iclrruntimeinfo-getinterface-method)
- [dotnet/core#505 — vsdbg licensing discussion](https://github.com/dotnet/core/issues/505)
