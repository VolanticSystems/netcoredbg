// Copyright (c) 2026 Volantic
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

// Desktop CLR (.NET Framework 4.x) runtime discovery implementation.
// Uses mscoree.dll / ICLRMetaHost to obtain ICorDebug for .NET Framework processes.

#ifdef WIN32

#include <windows.h>
#include <tlhelp32.h>
#include <metahost.h>
#include <string>
#include <algorithm>
#include <memory>

#include "debugger/desktopclr.h"
#include "utils/torelease.h"
#include "utils/utf.h"
#include "utils/logger.h"

#pragma comment(lib, "mscoree.lib")

namespace netcoredbg
{

// Check if a module name matches (case-insensitive) a target name.
static bool ModuleNameMatches(const WCHAR *modulePath, const WCHAR *targetName)
{
    // Find the last path separator
    const WCHAR *fileName = modulePath;
    const WCHAR *p = modulePath;
    while (*p)
    {
        if (*p == L'\\' || *p == L'/')
            fileName = p + 1;
        p++;
    }
    return _wcsicmp(fileName, targetName) == 0;
}

CLRType DetectCLRType(DWORD pid)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        LOGE("CreateToolhelp32Snapshot failed for PID %u: 0x%08x", pid, GetLastError());
        return CLRType::Unknown;
    }

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);

    CLRType result = CLRType::Unknown;

    if (Module32FirstW(hSnapshot, &me))
    {
        do
        {
            if (ModuleNameMatches(me.szModule, L"coreclr.dll"))
            {
                result = CLRType::CoreCLR;
                break;
            }
            if (ModuleNameMatches(me.szModule, L"clr.dll") ||
                ModuleNameMatches(me.szModule, L"mscorwks.dll"))
            {
                result = CLRType::DesktopCLR;
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }

    CloseHandle(hSnapshot);
    return result;
}

HRESULT CreateDesktopCLRDebuggingInterface(DWORD pid, ICorDebug **ppCorDebug)
{
    if (!ppCorDebug)
        return E_INVALIDARG;

    *ppCorDebug = nullptr;

    HRESULT hr;

    // Step 1: Get ICLRMetaHost via CLRCreateInstance
    ICLRMetaHost *pMetaHost = nullptr;
    hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID *)&pMetaHost);
    if (FAILED(hr))
    {
        LOGE("CLRCreateInstance failed: 0x%08x", hr);
        return hr;
    }

    // Step 2: Open the target process to enumerate its loaded runtimes
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProcess == NULL)
    {
        LOGE("OpenProcess failed for PID %u: 0x%08x", pid, GetLastError());
        pMetaHost->Release();
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Step 3: Enumerate loaded runtimes in the target process
    IEnumUnknown *pEnumerator = nullptr;
    hr = pMetaHost->EnumerateLoadedRuntimes(hProcess, &pEnumerator);
    if (FAILED(hr))
    {
        LOGE("EnumerateLoadedRuntimes failed: 0x%08x", hr);
        CloseHandle(hProcess);
        pMetaHost->Release();
        return hr;
    }

    // Step 4: Get the first runtime (typically only one CLR loaded)
    IUnknown *pUnk = nullptr;
    ULONG fetched = 0;
    hr = pEnumerator->Next(1, &pUnk, &fetched);
    if (hr != S_OK || fetched == 0 || pUnk == nullptr)
    {
        LOGE("No .NET Framework runtime found in process %u", pid);
        pEnumerator->Release();
        CloseHandle(hProcess);
        pMetaHost->Release();
        return E_FAIL;
    }

    ICLRRuntimeInfo *pRuntimeInfo = nullptr;
    hr = pUnk->QueryInterface(IID_ICLRRuntimeInfo, (LPVOID *)&pRuntimeInfo);
    pUnk->Release();
    if (FAILED(hr))
    {
        LOGE("QueryInterface for ICLRRuntimeInfo failed: 0x%08x", hr);
        pEnumerator->Release();
        CloseHandle(hProcess);
        pMetaHost->Release();
        return hr;
    }

    // Log the runtime version
    WCHAR versionString[64];
    DWORD versionLen = _countof(versionString);
    if (SUCCEEDED(pRuntimeInfo->GetVersionString(versionString, &versionLen)))
    {
        std::string ver = to_utf8(versionString);
        LOGI("Found .NET Framework runtime: %s", ver.c_str());
    }

    // Step 5: Get ICorDebug from the runtime info
    ICorDebug *pCorDebug = nullptr;
    hr = pRuntimeInfo->GetInterface(CLSID_CLRDebuggingLegacy, IID_ICorDebug, (LPVOID *)&pCorDebug);
    if (FAILED(hr))
    {
        LOGE("GetInterface for ICorDebug failed: 0x%08x", hr);
        pRuntimeInfo->Release();
        pEnumerator->Release();
        CloseHandle(hProcess);
        pMetaHost->Release();
        return hr;
    }

    *ppCorDebug = pCorDebug;

    // Cleanup
    pRuntimeInfo->Release();
    pEnumerator->Release();
    CloseHandle(hProcess);
    pMetaHost->Release();

    LOGI("Successfully obtained ICorDebug for .NET Framework process %u", pid);
    return S_OK;
}

std::string GetDesktopCLRPath(DWORD pid)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return std::string();

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);

    std::string result;

    if (Module32FirstW(hSnapshot, &me))
    {
        do
        {
            if (ModuleNameMatches(me.szModule, L"clr.dll") ||
                ModuleNameMatches(me.szModule, L"mscorwks.dll"))
            {
                result = to_utf8(me.szExePath);
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }

    CloseHandle(hSnapshot);
    return result;
}

std::string FindHostingCoreCLRPath()
{
    // Strategy 1: Look for coreclr.dll next to netcoredbg.exe
    {
        WCHAR exePath[MAX_PATH];
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0)
        {
            std::wstring dir(exePath);
            auto pos = dir.find_last_of(L"\\/");
            if (pos != std::wstring::npos)
            {
                dir = dir.substr(0, pos + 1);
                std::wstring candidate = dir + L"coreclr.dll";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    return to_utf8(candidate.c_str());
                }
            }
        }
    }

    // Strategy 2: Search system .NET runtime directories (newest first)
    {
        const char *programFiles = getenv("ProgramFiles");
        if (!programFiles)
            programFiles = "C:\\Program Files";

        std::string runtimeBase = std::string(programFiles) + "\\dotnet\\shared\\Microsoft.NETCore.App";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA((runtimeBase + "\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            std::string bestVersion;
            std::string bestPath;
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (fd.cFileName[0] == '.')
                    continue;

                std::string candidate = runtimeBase + "\\" + fd.cFileName + "\\coreclr.dll";
                DWORD attrs = GetFileAttributesA(candidate.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES)
                {
                    // Simple version comparison: pick the latest (lexicographic works for dotnet versions)
                    if (fd.cFileName > bestVersion)
                    {
                        bestVersion = fd.cFileName;
                        bestPath = candidate;
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);

            if (!bestPath.empty())
                return bestPath;
        }
    }

    return std::string();
}

bool IsFrameworkExecutable(const std::string &exePath)
{
    HANDLE hFile = CreateFileA(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    auto closeFile = [&](void*) { CloseHandle(hFile); };
    std::unique_ptr<void, decltype(closeFile)> guard(hFile, closeFile);

    // Read DOS header
    IMAGE_DOS_HEADER dosHeader;
    DWORD bytesRead;
    if (!ReadFile(hFile, &dosHeader, sizeof(dosHeader), &bytesRead, NULL) || bytesRead != sizeof(dosHeader))
        return false;
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    // Seek to PE header
    if (SetFilePointer(hFile, dosHeader.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return false;

    // Read PE signature + file header
    DWORD peSignature;
    if (!ReadFile(hFile, &peSignature, sizeof(peSignature), &bytesRead, NULL) || bytesRead != sizeof(peSignature))
        return false;
    if (peSignature != IMAGE_NT_SIGNATURE)
        return false;

    IMAGE_FILE_HEADER fileHeader;
    if (!ReadFile(hFile, &fileHeader, sizeof(fileHeader), &bytesRead, NULL) || bytesRead != sizeof(fileHeader))
        return false;

    // Read optional header to get the CLR data directory
    // Check IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR (index 14) — present in all .NET assemblies
    if (fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 || fileHeader.Machine == IMAGE_FILE_MACHINE_IA64)
    {
        IMAGE_OPTIONAL_HEADER64 optHeader;
        if (!ReadFile(hFile, &optHeader, sizeof(optHeader), &bytesRead, NULL) || bytesRead != sizeof(optHeader))
            return false;
        if (optHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            return false;
        return optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
    }
    else
    {
        IMAGE_OPTIONAL_HEADER32 optHeader;
        if (!ReadFile(hFile, &optHeader, sizeof(optHeader), &bytesRead, NULL) || bytesRead != sizeof(optHeader))
            return false;
        if (optHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            return false;
        return optHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
    }
}

bool WaitForDesktopCLRLoad(DWORD pid, int timeoutMs)
{
    const int pollIntervalMs = 100;
    int elapsed = 0;

    while (elapsed < timeoutMs)
    {
        CLRType type = DetectCLRType(pid);
        if (type == CLRType::DesktopCLR || type == CLRType::CoreCLR)
            return true;

        Sleep(pollIntervalMs);
        elapsed += pollIntervalMs;
    }

    return false;
}

HRESULT CreateDesktopCLRDebuggingInterfaceForLaunch(ICorDebug **ppCorDebug)
{
    if (!ppCorDebug)
        return E_INVALIDARG;

    *ppCorDebug = nullptr;

    HRESULT hr;

    ICLRMetaHost *pMetaHost = nullptr;
    hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID *)&pMetaHost);
    if (FAILED(hr))
    {
        LOGE("CLRCreateInstance failed: 0x%08x", hr);
        return hr;
    }

    // Get the v4.0 runtime directly (all .NET Framework 4.x uses this version)
    ICLRRuntimeInfo *pRuntimeInfo = nullptr;
    hr = pMetaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID *)&pRuntimeInfo);
    if (FAILED(hr))
    {
        LOGE("GetRuntime v4.0.30319 failed: 0x%08x", hr);
        pMetaHost->Release();
        return hr;
    }

    WCHAR versionString[64];
    DWORD versionLen = _countof(versionString);
    if (SUCCEEDED(pRuntimeInfo->GetVersionString(versionString, &versionLen)))
    {
        LOGI("Using .NET Framework runtime: %s", to_utf8(versionString).c_str());
    }

    ICorDebug *pCorDebug = nullptr;
    hr = pRuntimeInfo->GetInterface(CLSID_CLRDebuggingLegacy, IID_ICorDebug, (LPVOID *)&pCorDebug);
    if (FAILED(hr))
    {
        LOGE("GetInterface for ICorDebug failed: 0x%08x", hr);
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return hr;
    }

    *ppCorDebug = pCorDebug;

    pRuntimeInfo->Release();
    pMetaHost->Release();

    LOGI("Successfully obtained ICorDebug for .NET Framework launch");
    return S_OK;
}

} // namespace netcoredbg

#endif // WIN32
