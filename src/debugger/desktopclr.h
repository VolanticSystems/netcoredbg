// Copyright (c) 2026 Volantic
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

// Desktop CLR (.NET Framework 4.x) runtime discovery.
// Uses mscoree.dll / ICLRMetaHost to obtain ICorDebug for .NET Framework processes,
// as an alternative to the CoreCLR path which uses dbgshim.dll.

#pragma once

#ifdef WIN32

#include "cor.h"
#include "cordebug.h"
#include <string>

namespace netcoredbg
{

enum class CLRType
{
    Unknown,
    CoreCLR,     // .NET Core / .NET 5+
    DesktopCLR   // .NET Framework 4.x
};

// Detect which CLR type a process is running by checking loaded modules.
// Returns CLRType::DesktopCLR if clr.dll is loaded, CLRType::CoreCLR if coreclr.dll is loaded.
CLRType DetectCLRType(DWORD pid);

// Create an ICorDebug instance for a .NET Framework process using the mscoree hosting APIs:
//   CLRCreateInstance -> ICLRMetaHost -> EnumerateLoadedRuntimes -> ICLRRuntimeInfo::GetInterface
// Returns S_OK on success with ppCorDebug set, or an error HRESULT.
HRESULT CreateDesktopCLRDebuggingInterface(DWORD pid, ICorDebug **ppCorDebug);

// Get the path to clr.dll loaded in the target process (for CLR path resolution).
std::string GetDesktopCLRPath(DWORD pid);

// Find a CoreCLR installation on the system for hosting ManagedPart.dll.
// When debugging Desktop CLR processes, we still need CoreCLR to host our managed helpers.
// Searches: 1) next to netcoredbg.exe, 2) system .NET runtime dirs.
std::string FindHostingCoreCLRPath();

// Check if an executable file is a .NET Framework assembly by reading its PE headers.
// Returns true if the exe has a CLR header (COM descriptor directory entry).
bool IsFrameworkExecutable(const std::string &exePath);

// Wait for the CLR to load in a process (polls for clr.dll in module list).
// Returns true if clr.dll was found within the timeout, false otherwise.
bool WaitForDesktopCLRLoad(DWORD pid, int timeoutMs = 10000);

// Get ICorDebug for .NET Framework v4.0 without needing a running process.
// Uses ICLRMetaHost::GetRuntime("v4.0.30319") instead of EnumerateLoadedRuntimes.
HRESULT CreateDesktopCLRDebuggingInterfaceForLaunch(ICorDebug **ppCorDebug);

} // namespace netcoredbg

#endif // WIN32
