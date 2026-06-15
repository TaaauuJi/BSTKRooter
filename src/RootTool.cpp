/*
 * BSTKRooter
 * Copyright (c) 2026 Taaauu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "RootTool.h"
#include "resources.h"
#include <imgui.h>
#include <windows.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <thread>
#include <mutex>

#include "VHDManager.h"

namespace fs = std::filesystem;

extern HWND g_hWnd;

static void KillProcessByName(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe{ sizeof(pe) };
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, name) == 0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    }
    CloseHandle(snap);
}



RootTool::RootTool() {
    RefreshEmulatorInfo();

    // tray icon
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = ::LoadIcon(::GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON1));
    if (!nid.hIcon) {
        nid.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION);
    }
    strcpy_s(nid.szTip, sizeof(nid.szTip), "BSTK Rooter");
    
    ::Shell_NotifyIconA(NIM_ADD, &nid);
}

RootTool::~RootTool() {
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = 1;
    ::Shell_NotifyIconA(NIM_DELETE, &nid);
}

void RootTool::LaunchEmulator(const std::string& exePath, const std::string& args) {
    Log("[*] Launching emulator as standard user (de-elevated)...");
    Log("[*]   Exe:  " + exePath);
    Log("[*]   Args: " + args);

    bool launched = false;

    // ── Method 1: Duplicate Explorer.exe's non-elevated token ────────────
    // Explorer.exe runs as the logged-in standard user. We duplicate its
    // token and use CreateProcessWithTokenW to spawn HD-Player under it.
    DWORD explorerPid = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe{ sizeof(pe) };
            for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
                if (_stricmp(pe.szExeFile, "explorer.exe") == 0) {
                    explorerPid = pe.th32ProcessID;
                    break;
                }
            }
            CloseHandle(snap);
        }
    }

    if (explorerPid != 0) {
        Log("[*] Found explorer.exe (PID " + std::to_string(explorerPid) + "), duplicating token...");
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, explorerPid);
        if (hProcess) {
            HANDLE hToken = NULL;
            if (OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hToken)) {
                HANDLE hNewToken = NULL;
                if (DuplicateTokenEx(hToken, TOKEN_QUERY | TOKEN_DUPLICATE |
                    TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT |
                    TOKEN_ADJUST_SESSIONID,
                    NULL, SecurityImpersonation, TokenPrimary, &hNewToken))
                {
                    std::string cmdLine = "\"" + exePath + "\" " + args;
                    int wLen = MultiByteToWideChar(CP_ACP, 0, cmdLine.c_str(), -1, NULL, 0);
                    std::vector<wchar_t> wCmdBuf(wLen);
                    MultiByteToWideChar(CP_ACP, 0, cmdLine.c_str(), -1, wCmdBuf.data(), wLen);

                    STARTUPINFOW si = { sizeof(si) };
                    PROCESS_INFORMATION pi = {};

                    if (CreateProcessWithTokenW(hNewToken, 0, NULL, wCmdBuf.data(),
                        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
                    {
                        launched = true;
                        Log("[+] Emulator launched as standard user via token duplication.");
                        if (pi.hProcess) CloseHandle(pi.hProcess);
                        if (pi.hThread)  CloseHandle(pi.hThread);
                    } else {
                        Log("[!] CreateProcessWithTokenW failed: " + std::to_string(GetLastError()), true);
                    }
                    CloseHandle(hNewToken);
                } else {
                    Log("[!] DuplicateTokenEx failed: " + std::to_string(GetLastError()), true);
                }
                CloseHandle(hToken);
            } else {
                Log("[!] OpenProcessToken failed: " + std::to_string(GetLastError()), true);
            }
            CloseHandle(hProcess);
        } else {
            Log("[!] OpenProcess for explorer.exe failed: " + std::to_string(GetLastError()), true);
        }
    } else {
        Log("[~] Could not find explorer.exe process.");
    }

    // ── Method 2: COM Shell dispatch via IShellDispatch2 ─────────────────
    if (!launched) {
        Log("[*] Trying COM IShellDispatch2 approach...");
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        HWND hwndShell = GetShellWindow();
        if (hwndShell) {
            IShellWindows* psw = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr,
                CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&psw));
            if (SUCCEEDED(hr) && psw) {
                VARIANT vtLoc = {};
                vtLoc.vt = VT_I4;
                vtLoc.lVal = CSIDL_DESKTOP;
                VARIANT vtEmpty = {};
                vtEmpty.vt = VT_EMPTY;

                long lHwnd = 0;
                IDispatch* pdisp = nullptr;
                hr = psw->FindWindowSW(&vtLoc, &vtEmpty, SWC_DESKTOP,
                    &lHwnd, SWFO_NEEDDISPATCH, &pdisp);
                if (SUCCEEDED(hr) && pdisp) {
                    IServiceProvider* psp = nullptr;
                    hr = pdisp->QueryInterface(IID_PPV_ARGS(&psp));
                    if (SUCCEEDED(hr) && psp) {
                        IShellBrowser* psb = nullptr;
                        hr = psp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&psb));
                        if (SUCCEEDED(hr) && psb) {
                            IShellView* psv = nullptr;
                            hr = psb->QueryActiveShellView(&psv);
                            if (SUCCEEDED(hr) && psv) {
                                IDispatch* pdispBg = nullptr;
                                hr = psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdispBg));
                                if (SUCCEEDED(hr) && pdispBg) {
                                    IShellFolderViewDual* psfvd = nullptr;
                                    hr = pdispBg->QueryInterface(IID_PPV_ARGS(&psfvd));
                                    if (SUCCEEDED(hr) && psfvd) {
                                        IDispatch* pdApp = nullptr;
                                        hr = psfvd->get_Application(&pdApp);
                                        if (SUCCEEDED(hr) && pdApp) {
                                            IShellDispatch2* psd2 = nullptr;
                                            hr = pdApp->QueryInterface(IID_PPV_ARGS(&psd2));
                                            if (SUCCEEDED(hr) && psd2) {
                                                int wFileLen = MultiByteToWideChar(CP_ACP, 0, exePath.c_str(), -1, NULL, 0);
                                                std::vector<wchar_t> wFile(wFileLen);
                                                MultiByteToWideChar(CP_ACP, 0, exePath.c_str(), -1, wFile.data(), wFileLen);

                                                int wArgsLen = MultiByteToWideChar(CP_ACP, 0, args.c_str(), -1, NULL, 0);
                                                std::vector<wchar_t> wArgs(wArgsLen);
                                                MultiByteToWideChar(CP_ACP, 0, args.c_str(), -1, wArgs.data(), wArgsLen);

                                                BSTR bstrFile = SysAllocString(wFile.data());
                                                BSTR bstrArgs = SysAllocString(wArgs.data());

                                                VARIANT vArgs = {}; vArgs.vt = VT_BSTR; vArgs.bstrVal = bstrArgs;
                                                VARIANT vDir  = {}; vDir.vt = VT_EMPTY;
                                                VARIANT vOp   = {}; vOp.vt = VT_EMPTY;
                                                VARIANT vShow = {}; vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;

                                                hr = psd2->ShellExecute(bstrFile, vArgs, vDir, vOp, vShow);
                                                if (SUCCEEDED(hr)) {
                                                    launched = true;
                                                    Log("[+] Emulator launched via IShellDispatch2 COM dispatch.");
                                                } else {
                                                    Log("[!] IShellDispatch2::ShellExecute failed: 0x" + std::to_string(hr), true);
                                                }
                                                SysFreeString(bstrFile);
                                                SysFreeString(bstrArgs);
                                                psd2->Release();
                                            }
                                            pdApp->Release();
                                        }
                                        psfvd->Release();
                                    }
                                    pdispBg->Release();
                                }
                                psv->Release();
                            }
                            psb->Release();
                        }
                        psp->Release();
                    }
                    pdisp->Release();
                }
                psw->Release();
            }
        } else {
            Log("[~] GetShellWindow returned NULL.");
        }

        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    // ── Method 3: Elevated CreateProcess fallback ────────────────────────
    if (!launched) {
        Log("[~] All de-elevation methods failed, falling back to elevated CreateProcess...");
        std::string sc = "\"" + exePath + "\" " + args;
        std::vector<char> cmdBuf(sc.begin(), sc.end());
        cmdBuf.push_back('\0');

        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            Log("[+] Emulator process created (elevated fallback).");
            if (pi.hProcess) CloseHandle(pi.hProcess);
            if (pi.hThread)  CloseHandle(pi.hThread);
        } else {
            Log("[!] CreateProcessA fallback also failed: " + std::to_string(GetLastError()), true);
        }
    }
}

void RootTool::Log(const std::string& msg, bool isError) {
    std::ofstream logFile("bstk_kitsune.log", std::ios::app);
    if (logFile) {
        time_t t = time(nullptr);
        char buf[26]; ctime_s(buf, sizeof(buf), &t);
        std::string timeStr(buf); if (!timeStr.empty() && timeStr.back() == '\n') timeStr.pop_back();
        logFile << "[" << timeStr << "] " << (isError ? "[ERROR] " : "[INFO] ") << msg << std::endl;
    }
}

void RootTool::SetStatus(const std::string& msg, bool isError) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusMsg     = msg;
    m_statusIsError = isError;
}

void RootTool::ShowSystemNotification(const std::string& title, const std::string& message) {
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    strcpy_s(nid.szInfoTitle, sizeof(nid.szInfoTitle), title.c_str());
    strcpy_s(nid.szInfo, sizeof(nid.szInfo), message.c_str());
    
    ::Shell_NotifyIconA(NIM_MODIFY, &nid);
}

// =====================================================================
// RESOURCE HELPERS
// =====================================================================

std::string RootTool::ExtractResourceToTemp(int resourceId, const char* tmpName) {
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return {};
    DWORD sz = SizeofResource(nullptr, hRes);
    const void* data = LockResource(hData);
    if (!data || sz == 0) return {};

    char tempDir[MAX_PATH]{}; ::GetTempPathA(MAX_PATH, tempDir);
    std::string tmpPath = std::string(tempDir) + tmpName;
    std::ofstream f(tmpPath, std::ios::binary);
    if (!f) return {};
    f.write(reinterpret_cast<const char*>(data), sz);
    f.close();
    return tmpPath;
}

std::string RootTool::FindDataVhdx(const std::string& instanceDir) {
    std::string dataPath = instanceDir + "Data.vhdx";
    if (fs::exists(dataPath)) return dataPath;

    std::error_code ec;
    std::string anyVhdx;
    for (const auto& entry : fs::directory_iterator(instanceDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        std::string name = entry.path().stem().string();
        for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
        if (ext == ".vhdx") {
            if (name.find("data") != std::string::npos) return entry.path().string();
            if (anyVhdx.empty()) anyVhdx = entry.path().string();
        }
    }
    return anyVhdx;
}


void RootTool::InstallKitsuneMagisk(const std::string& dataDir, const std::string& selectedInstance) {
    if (dataDir.empty() || selectedInstance.empty()) return;
    std::string masterInst = GetMasterInstanceName(selectedInstance);
    KillProcesses(); ::Sleep(1000);

    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string instanceDir = engineDir + masterInst + "\\";
    std::string vhdPath = instanceDir + "Root.vhd";
    if (!fs::exists(vhdPath)) { SetStatus("Root.vhd not found.", true); return; }

    auto copyRes = [&](VHDManager& v, int resId, const char* tmp, const std::string& dst) -> bool {
        std::string p = ExtractResourceToTemp(resId, tmp);
        if (p.empty()) return false;
        v.DeleteFile(dst);
        bool ok = v.CopyFileFromHost(p, dst);
        fs::remove(p);
        if (ok) { v.SetFilePermissions(dst, 0755); v.SetFileOwner(dst, 0, 0); }
        return ok;
    };

    SetStatus("Phase 1: Copying to Root.vhd...", false);
    {
        VHDManager v;
        if (!v.OpenVHD(vhdPath)) { SetStatus("Failed to open Root.vhd.", true); return; }
        const auto& pa = v.GetPartitions(); int ei = -1;
        for (size_t i = 0; i < pa.size(); i++) if (pa[i].is_ext4 && ei < 0) ei = (int)i;
        if (ei < 0 || !v.MountExt4Partition(ei)) { v.CloseVHD(); SetStatus("No ext4 in Root.vhd.", true); return; }
        v.MakeDirectory("/android/system/etc/init");
        v.MakeDirectory("/android/system/etc/init/magisk");
        Log("[*] Cleaning up any old su binaries to prevent Magisk conflicts...");
        v.DeleteFile("/android/system/xbin/su");
        v.DeleteFile("/android/system/bin/su");
        struct R { int id; const char* t; const char* d; };
        R f[] = {
            {IDR_MAGISK_RC,"bstk_rc.tmp","/android/system/etc/init/magisk.rc"},
            {IDR_MAGISK32,"bstk_m32.tmp","/android/system/etc/init/magisk/magisk32"},
            {IDR_MAGISK64,"bstk_m64.tmp","/android/system/etc/init/magisk/magisk64"},
            {IDR_MAGISKINIT,"bstk_mi.tmp","/android/system/etc/init/magisk/magiskinit"},
            {IDR_MAGISKPOLICY,"bstk_mp.tmp","/android/system/etc/init/magisk/magiskpolicy"},
            {IDR_STUB_APK,"bstk_sa.tmp","/android/system/etc/init/magisk/stub.apk"},
            {IDR_MAGISK_CONFIG,"bstk_mc.tmp","/android/system/etc/init/magisk/config"},
        };
        for (auto& e : f) copyRes(v, e.id, e.t, e.d);
        v.UnmountExt4(); v.CloseVHD();
    }

    SetStatus("Phase 2: Copying to Data disk...", false);
    {
        std::string dd = FindDataVhdx(instanceDir);
        if (!dd.empty()) {
            VHDManager v;
            if (v.OpenVHD(dd)) {
                const auto& pa = v.GetPartitions(); int ei = -1;
                for (size_t i = 0; i < pa.size(); i++) if (pa[i].is_ext4 && ei < 0) ei = (int)i;
                if (ei >= 0 && v.MountExt4Partition(ei)) {
                    v.MakeDirectory("/adb"); v.MakeDirectory("/adb/magisk"); v.MakeDirectory("/adb/magisk/chromeos");
                    struct R { int id; const char* t; const char* d; };
                    R f[] = {
                        {IDR_MAGISK32,"bstk_d32.tmp","/adb/magisk/magisk32"},
                        {IDR_MAGISK64,"bstk_d64.tmp","/adb/magisk/magisk64"},
                        {IDR_MAGISKINIT,"bstk_di.tmp","/adb/magisk/magiskinit"},
                        {IDR_MAGISKPOLICY,"bstk_dp.tmp","/adb/magisk/magiskpolicy"},
                        {IDR_BUSYBOX,"bstk_bb.tmp","/adb/magisk/busybox"},
                        {IDR_MAGISKBOOT,"bstk_mb.tmp","/adb/magisk/magiskboot"},
                        {IDR_STUB_APK,"bstk_ds.tmp","/adb/magisk/stub.apk"},
                        {IDR_UTIL_FUNCTIONS,"bstk_uf.tmp","/adb/magisk/util_functions.sh"},
                        {IDR_BOOT_PATCH,"bstk_bp.tmp","/adb/magisk/boot_patch.sh"},
                        {IDR_ADDON_D,"bstk_ad.tmp","/adb/magisk/addon.d.sh"},
                        {IDR_CHROMEOS_FUTILITY,"bstk_cf.tmp","/adb/magisk/chromeos/futility"},
                        {IDR_CHROMEOS_KEYBLOCK,"bstk_ck.tmp","/adb/magisk/chromeos/kernel.keyblock"},
                        {IDR_CHROMEOS_VBPRIVK,"bstk_cv.tmp","/adb/magisk/chromeos/kernel_data_key.vbprivk"},
                        {IDR_MAGISK_DB,"bstk_db.tmp","/adb/magisk.db"},
                    };
                    for (auto& e : f) copyRes(v, e.id, e.t, e.d);
                    v.UnmountExt4();
                }
                v.CloseVHD();
            }
        }
    }

    SetStatus("Opening emulator...", false);
    {
        EmulatorInfo& emu = (m_selectedEmulator == 0) ? m_bluestacks : m_msi;
        std::string hp = emu.installDir;
        if (!hp.empty() && hp.back() != '\\') hp += '\\';
        hp += "HD-Player.exe";
        LaunchEmulator(hp, "--instance " + masterInst);
        ::Sleep(3000);

        ShowSystemNotification("Install Kitsune Magisk", 
            "Kitsune Magisk files have been successfully copied offline!\n\n"
            "Please install the Magisk manager APK manually now from within the emulator.");

        SetStatus("Kitsune Magisk files copied. Please install Magisk APK manually.", false);
    }
}

void RootTool::UninstallKitsuneMagisk(const std::string& dataDir, const std::string& selectedInstance) {
    if (dataDir.empty() || selectedInstance.empty()) return;
    std::string masterInst = GetMasterInstanceName(selectedInstance);

    KillProcesses(); ::Sleep(1000);
    std::string ed = dataDir;
    if (!ed.empty() && ed.back() != '\\' && ed.back() != '/') ed += '\\';
    std::string instanceDir = ed + masterInst + "\\";

    SetStatus("Cleaning Root.vhd...", false);
    {
        VHDManager v;
        if (v.OpenVHD(instanceDir + "Root.vhd")) {
            const auto& pa = v.GetPartitions(); int ei = -1;
            for (size_t i = 0; i < pa.size(); i++) if (pa[i].is_ext4 && ei < 0) ei = (int)i;
            if (ei >= 0 && v.MountExt4Partition(ei)) {
                v.DeleteRecursive("/android/system/etc/init/magisk");
                if (v.FileExists("/android/system/etc/init/magisk.rc")) v.DeleteFile("/android/system/etc/init/magisk.rc");
                v.UnmountExt4();
            }
            v.CloseVHD();
        }
    }

    SetStatus("Cleaning Data disk...", false);
    {
        std::string dd = FindDataVhdx(instanceDir);
        if (!dd.empty()) {
            VHDManager v;
            if (v.OpenVHD(dd)) {
                const auto& pa = v.GetPartitions(); int ei = -1;
                for (size_t i = 0; i < pa.size(); i++) if (pa[i].is_ext4 && ei < 0) ei = (int)i;
                if (ei >= 0 && v.MountExt4Partition(ei)) {
                    v.DeleteRecursive("/adb");
                    v.UnmountExt4();
                }
                v.CloseVHD();
            }
        }
    }

    SetStatus("Opening emulator...", false);
    {
        EmulatorInfo& emu = (m_selectedEmulator == 0) ? m_bluestacks : m_msi;
        std::string hp = emu.installDir;
        if (!hp.empty() && hp.back() != '\\') hp += '\\';
        hp += "HD-Player.exe";
        LaunchEmulator(hp, "--instance " + masterInst);
        ::Sleep(3000);

        ShowSystemNotification("Uninstall Kitsune Magisk", 
            "Kitsune Magisk files have been successfully cleaned from the disks offline!\n\n"
            "Please uninstall the Magisk manager APK manually now from within the emulator.");

        SetStatus("Kitsune Magisk uninstalled. Please uninstall Magisk APK manually.", false);
    }
}

std::string RootTool::ReadRegistryString(const std::string& subKey, const std::string& valueName) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};

    char   val[MAX_PATH]{};
    DWORD  len = sizeof(val);
    DWORD  type;
    bool   ok = (RegQueryValueExA(hKey, valueName.c_str(), nullptr, &type, (LPBYTE)val, &len) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return ok ? std::string(val) : std::string{};
}

void RootTool::FindInstances(EmulatorInfo& info) {
    info.instances.clear();
    if (info.dataDir.empty()) return;

    std::string engineDir = info.dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';

    bool alreadyHasEngine = false;
    {
        std::string lower = engineDir;
        for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
        if (lower.find("engine\\") != std::string::npos || lower.find("engine/") != std::string::npos)
            alreadyHasEngine = true;
    }

    if (!alreadyHasEngine) engineDir += "Engine\\";

    if (!fs::exists(engineDir)) return;

    std::string metaPath = engineDir + "UserData\\MimMetaData.json";
    if (fs::exists(metaPath)) {
        std::string content = ReadFileString(metaPath);
        std::regex blockRegex(R"(\{([^{}]*\"InstanceName\"[^{}]*)\})");
        auto next = std::sregex_iterator(content.begin(), content.end(), blockRegex);
        auto end = std::sregex_iterator();
        while (next != end) {
            std::string block = next->str(1);
            std::regex nameRegex(R"(\"Name\"\s*:\s*\"([^\"]+)\")");
            std::regex instNameRegex(R"(\"InstanceName\"\s*:\s*\"([^\"]+)\")");
            std::smatch nameMatch, instNameMatch;
            if (std::regex_search(block, nameMatch, nameRegex) && std::regex_search(block, instNameMatch, instNameRegex)) {
                BstkInstance inst;
                inst.displayName = nameMatch[1].str();
                inst.instanceName = instNameMatch[1].str();

                bool found = false;
                for (const auto& existing : info.instances) {
                    if (existing.instanceName == inst.instanceName) { found = true; break; }
                }
                if (!found) info.instances.push_back(inst);
            }
            next++;
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(engineDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();

        if (name == "Manager" || name == "UserData") continue;

        bool hasBstk = false;
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
            if (f.path().extension() == ".bstk") { hasBstk = true; break; }
        }
        if (hasBstk) {
            bool found = false;
            for (const auto& inst : info.instances) {
                if (inst.instanceName == name) { found = true; break; }
            }
            if (!found) {
                BstkInstance inst;
                inst.displayName = name;
                inst.instanceName = name;
                info.instances.push_back(inst);
            }
        }
    }
}

void RootTool::RefreshEmulatorInfo() {
    auto init = [&](EmulatorInfo& e, bool isBstk, const char* regKey, const char* displayName) {
        e.isBlueStacks = isBstk;
        e.name         = displayName;
        e.installDir   = ReadRegistryString(regKey, "InstallDir");
        e.dataDir      = ReadRegistryString(regKey, "DataDir");
        FindInstances(e);
    };

    init(m_bluestacks, true,  "SOFTWARE\\BlueStacks_nxt",  "BlueStacks 5");
    init(m_msi,        false, "SOFTWARE\\BlueStacks_msi5", "MSI App Player");

    if (!m_bluestacks.instances.empty())  m_selectedInstance = m_bluestacks.instances[0].instanceName;
    else if (!m_msi.instances.empty())    m_selectedInstance = m_msi.instances[0].instanceName;
}



std::string RootTool::ReadFileString(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

bool RootTool::WriteFileString(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}



bool RootTool::IsMasterInstance(const std::string& instanceName) {
    std::regex suffixPattern(".*_\\d+$");
    return !std::regex_match(instanceName, suffixPattern);
}

std::string RootTool::GetMasterInstanceName(const std::string& instanceName) {
    std::regex suffixPattern("^(.+)_\\d+$");
    std::smatch match;
    if (std::regex_match(instanceName, match, suffixPattern))
        return match[1].str();
    return instanceName;
}



void RootTool::KillProcesses() {
    KillProcessByName("HD-Player.exe");
    KillProcessByName("HD-MultiInstanceManager.exe");
    KillProcessByName("BstkSVC.exe");
    SetStatus("Emulator processes stopped.", false);
}

void RootTool::PatchHDPlayer(const std::string& installDir) {
    if (installDir.empty()) { Log("[!] InstallDir not found.", true); return; }


    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    std::string exePath  = installDir + "HD-Player.exe";
    std::string bakPath  = exePath    + ".bak";

    if (!fs::exists(exePath)) { Log("[!] HD-Player.exe not found: " + exePath, true); return; }


    if (!fs::exists(bakPath)) {
        std::error_code ec;
        fs::copy_file(exePath, bakPath, ec);
        if (ec) { Log("[!] Backup failed: " + ec.message(), true); return; }
        Log("[*] Backup created: " + bakPath);
    } else {
        Log("[*] Backup already exists, skipping copy.");
    }


    std::ifstream f(exePath, std::ios::binary | std::ios::ate);
    if (!f) { Log("[!] Cannot open HD-Player.exe. Close the emulator first.", true); return; }
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0); f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
    f.close();

    char msg[256];
    snprintf(msg, sizeof(msg), "[*] Loaded %s (%zu bytes)", exePath.c_str(), buf.size());
    Log(msg);


    auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > buf.size()) return 0;
        return (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
               ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
    };
    auto rd16 = [&](size_t off) -> uint16_t {
        if (off + 2 > buf.size()) return 0;
        return (uint16_t)buf[off] | ((uint16_t)buf[off+1]<<8);
    };


    if (buf.size() < 0x40) { Log("[!] File too small for PE.", true); return; }
    size_t peOff = rd32(0x3C);
    if (peOff + 24 >= buf.size() || buf[peOff] != 'P' || buf[peOff+1] != 'E') {
        Log("[!] Invalid PE header.", true); return;
    }

    int    numSections = rd16(peOff + 6);
    size_t optHdrSize  = rd16(peOff + 20);
    size_t secTableOff = peOff + 24 + optHdrSize;


    struct SecInfo { char name[9]{}; uint32_t va, vsz, rawOff, rawSz; };
    std::vector<SecInfo> secs;
    for (int i = 0; i < numSections; i++) {
        size_t sh = secTableOff + (size_t)i * 40;
        if (sh + 40 > buf.size()) break;
        SecInfo si;
        memcpy(si.name, &buf[sh], 8); si.name[8] = 0;
        si.vsz    = rd32(sh + 8);
        si.va     = rd32(sh + 12);
        si.rawSz  = rd32(sh + 16);
        si.rawOff = rd32(sh + 20);
        secs.push_back(si);
    }


    const SecInfo* textSec = nullptr;
    for (auto& s : secs)
        if (strncmp(s.name, ".text", 5) == 0) { textSec = &s; break; }
    if (!textSec) { Log("[!] .text section not found.", true); return; }

    size_t textStart = textSec->rawOff;
    size_t textEnd   = textSec->rawOff + textSec->rawSz;


    auto rvaToFile = [&](uint32_t rva) -> size_t {
        for (auto& s : secs)
            if (rva >= s.va && rva < s.va + s.vsz)
                return s.rawOff + (rva - s.va);
        return (size_t)-1;
    };


    auto findString = [&](const char* str, size_t start = 0, size_t end = (size_t)-1) {
        std::vector<size_t> hits;
        size_t len = strlen(str);
        if (end > buf.size()) end = buf.size();
        if (len == 0 || start + len > end) return hits;
        for (size_t i = start; i + len <= end; i++)
            if (memcmp(&buf[i], str, len) == 0)
                hits.push_back(i);
        return hits;
    };


    auto isTestJzAfterCall = [&](size_t testOff) -> bool {
        if (testOff + 4 > buf.size()) return false;
        if (buf[testOff] != 0x84 || buf[testOff+1] != 0xC0) return false;
        if (buf[testOff+2] != 0x74) return false;
        if (testOff < 5) return false;
        return (buf[testOff - 5] == 0xE8);
    };


    size_t patchOffset = std::string::npos;
    int    patchMethod = 0;
    std::string patchMethodDesc;


    auto findViaStringAnchor = [&](const char* anchor, int method,
                                    const char* methodName, int backRange) -> bool
    {
        auto strHits = findString(anchor);
        if (strHits.empty()) return false;

        snprintf(msg, sizeof(msg), "[*]   Found \"%s\" at %zu offset(s)", anchor, strHits.size());
        Log(msg);

        for (size_t strFileOff : strHits) {

            uint32_t strRva = 0;
            for (auto& s : secs) {
                if (strFileOff >= s.rawOff && strFileOff < s.rawOff + s.rawSz) {
                    strRva = s.va + (uint32_t)(strFileOff - s.rawOff);
                    break;
                }
            }
            if (strRva == 0) continue;


            for (size_t i = textStart; i + 7 <= textEnd; i++) {
                bool isLea = false;
                int leaLen = 7;

                if ((buf[i] == 0x48 || buf[i] == 0x4C) &&
                    buf[i+1] == 0x8D && (buf[i+2] & 0xC7) == 0x05)
                    isLea = true;
                else if (buf[i] == 0x8D && (buf[i+1] & 0xC7) == 0x05) {
                    isLea = true; leaLen = 6;
                }
                if (!isLea) continue;

                int32_t  disp      = (int32_t)rd32(i + leaLen - 4);
                uint32_t instrRva  = textSec->va + (uint32_t)(i - textStart);
                uint32_t targetRva = instrRva + leaLen + disp;
                if (targetRva != strRva) continue;


                size_t searchStart = (i > textStart + (size_t)backRange)
                                     ? i - backRange : textStart;
                for (size_t j = i - 2; j >= searchStart && j < i; j--) {
                    if (isTestJzAfterCall(j)) {
                        patchOffset    = j;
                        patchMethod    = method;
                        patchMethodDesc = methodName;
                        snprintf(msg, sizeof(msg),
                            "[+]   FOUND patch site at 0x%zX: %02X %02X %02X %02X (via %s)",
                            j, buf[j], buf[j+1], buf[j+2], buf[j+3], methodName);
                        Log(msg);
                        return true;
                    }
                }
            }
        }
        return false;
    };


    Log("[*] Strategy 1: Searching for \"Verified the disk integrity!\" anchor...");
    if (!findViaStringAnchor("Verified the disk integrity!", 1,
                              "Anchor: \"Verified the disk integrity!\"", 80))
    {
        Log("[~]   Strategy 1 failed.");


        Log("[*] Strategy 2: Searching for \"plrDiskCheckThreadEntry\" anchor...");
        if (!findViaStringAnchor("plrDiskCheckThreadEntry", 2,
                                  "Anchor: \"plrDiskCheckThreadEntry\"", 0x700))
        {
            Log("[~]   Strategy 2 failed.");


            Log("[*] Strategy 3: Searching for shutdown tamper message anchor...");
            if (!findViaStringAnchor(
                    "Shutting down: disk file have been illegally tampered with!", 3,
                    "Anchor: shutdown tamper message", 0x700))
            {
                Log("[~]   Strategy 3 failed.");


                Log("[*] Strategy 4: Full .text scan for CALL+test+jz pattern...");
                std::vector<size_t> candidates;
                for (size_t i = textStart + 5; i + 4 <= textEnd; i++) {
                    if (isTestJzAfterCall(i))
                        candidates.push_back(i);
                }
                snprintf(msg, sizeof(msg), "[*]   Found %zu candidate(s)", candidates.size());
                Log(msg);

                if (candidates.size() == 1) {
                    patchOffset     = candidates[0];
                    patchMethod     = 4;
                    patchMethodDesc = ".text scan (unique match)";
                    snprintf(msg, sizeof(msg),
                        "[+]   FOUND unique patch site at 0x%zX: %02X %02X %02X %02X",
                        patchOffset, buf[patchOffset], buf[patchOffset+1],
                        buf[patchOffset+2], buf[patchOffset+3]);
                    Log(msg);
                } else if (candidates.size() > 1) {

                    auto verifyHits = findString("Verified the disk integrity!");
                    auto failHits   = findString("Failed to verify the disk integrity!");

                    for (size_t cand : candidates) {
                        for (size_t k = cand; k < cand + 200 && k + 7 <= textEnd; k++) {
                            if ((buf[k] == 0x48 || buf[k] == 0x4C) &&
                                buf[k+1] == 0x8D && (buf[k+2] & 0xC7) == 0x05)
                            {
                                int32_t  disp      = (int32_t)rd32(k + 3);
                                uint32_t instrRva  = textSec->va + (uint32_t)(k - textStart);
                                uint32_t targetRva = instrRva + 7 + disp;
                                size_t   targetOff = rvaToFile(targetRva);

                                for (auto h : verifyHits) {
                                    if (targetOff == h) {
                                        patchOffset     = cand;
                                        patchMethod     = 4;
                                        patchMethodDesc = ".text scan (validated with verify string)";
                                        goto done_search;
                                    }
                                }
                                for (auto h : failHits) {
                                    if (targetOff == h) {
                                        patchOffset     = cand;
                                        patchMethod     = 4;
                                        patchMethodDesc = ".text scan (validated with fail string)";
                                        goto done_search;
                                    }
                                }
                            }
                        }
                    }
                    Log("[~]   Multiple candidates, none validated.");
                }
            }
        }
    }
done_search:

    if (patchOffset == std::string::npos) {
        Log("[!] All strategies exhausted -- patch site not found.", true);
        return;
    }

    snprintf(msg, sizeof(msg), "[*] Patch found via: %s (method %d)",
             patchMethodDesc.c_str(), patchMethod);
    Log(msg);


    if (buf[patchOffset+2] == 0x90 && buf[patchOffset+3] == 0x90) {
        Log("[~] Already patched. Nothing to do.");
        return;
    }


    snprintf(msg, sizeof(msg), "[*] Patching at 0x%zX: %02X %02X -> 90 90",
             patchOffset + 2, buf[patchOffset+2], buf[patchOffset+3]);
    Log(msg);
    buf[patchOffset + 2] = 0x90;
    buf[patchOffset + 3] = 0x90;

    std::ofstream out(exePath, std::ios::binary);
    if (!out || !out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size()))
        SetStatus("Failed to patch — run as Administrator.", true);
    else {
        SetStatus("Patched successfully!", false);
        ShowSystemNotification("HD-Player Patch", "Patched successfully!");
    }
}

void RootTool::ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }


    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);


    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';


    std::string instanceDir = engineDir + instanceName + "\\";
    std::string bstkPath    = instanceDir + instanceName + ".bstk";

    for (const std::string& filepath : { bstkPath }) {
        if (!fs::exists(filepath)) continue;
        Log("[*] Updating " + filepath);

        std::string content = ReadFileString(filepath);
        std::string result;
        result.reserve(content.size());

        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            bool targetLine = (line.find("location=\"fastboot.vdi\"") != std::string::npos ||
                               line.find("location=\"Root.vhd\"") != std::string::npos) &&
                               line.find("type=\"Readonly\"") != std::string::npos;
            if (targetLine)
                line = std::regex_replace(line, std::regex("type=\"Readonly\""), "type=\"Normal\"");
            result += line + "\n";
        }

        if (!WriteFileString(filepath, result)) {
            SetStatus("Failed to update .bstk — run as Administrator.", true);
            return;
        }
    }

    SetStatus("Disk set to R/W.", false);
    ShowSystemNotification("Disk Configuration", "Disk set to R/W successfully.");
}

void RootTool::RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }


    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);


    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';

    std::string instanceDir = engineDir + instanceName + "\\";
    std::string bstkPath    = instanceDir + instanceName + ".bstk";

    for (const std::string& filepath : { bstkPath }) {
        if (!fs::exists(filepath)) continue;
        Log("[*] Reverting " + filepath);

        std::string content = ReadFileString(filepath);
        std::string result;
        result.reserve(content.size());

        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            bool targetLine = (line.find("location=\"fastboot.vdi\"") != std::string::npos ||
                               line.find("location=\"Root.vhd\"") != std::string::npos) &&
                               line.find("type=\"Normal\"") != std::string::npos;
            if (targetLine)
                line = std::regex_replace(line, std::regex("type=\"Normal\""), "type=\"Readonly\"");
            result += line + "\n";
        }

        if (!WriteFileString(filepath, result)) {
            SetStatus("Failed to update .bstk — run as Administrator.", true);
            return;
        }
    }

    SetStatus("Disk reverted to Readonly.", false);
    ShowSystemNotification("Disk Configuration", "Disk reverted to Readonly successfully.");
}



void RootTool::OneClickRoot(const std::string& dataDir, const std::string& selectedInstance) {
    if (dataDir.empty() || selectedInstance.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    std::string masterInst = GetMasterInstanceName(selectedInstance);


    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);


    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';
    std::string instanceDir = engineDir + masterInst + "\\";
    std::string vhdPath     = instanceDir + "Root.vhd";

    if (!fs::exists(vhdPath)) {
        Log("[!] Root.vhd not found: " + vhdPath, true);
        return;
    }
    Log("[*] Found Root.vhd: " + vhdPath);


    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(IDR_SU_BINARY), RT_RCDATA);
    if (!hRes) {
        SetStatus("Internal error: su resource missing.", true);
        return;
    }
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        SetStatus("Internal error: su resource load failed.", true);
        return;
    }
    DWORD    suSize = SizeofResource(nullptr, hRes);
    const void* suData = LockResource(hData);
    if (!suData || suSize == 0) {
        SetStatus("Internal error: su resource empty.", true);
        return;
    }

    char tempDir[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tempDir);
    std::string resSuC = std::string(tempDir) + "bstk_su_c.tmp";
    {
        std::ofstream tmp(resSuC, std::ios::binary);
        if (!tmp) {
            SetStatus("Failed to create temp file.", true);
            return;
        }
        tmp.write(reinterpret_cast<const char*>(suData), suSize);
    }
    Log("[*] Extracted embedded su (" + std::to_string(suSize) + " bytes) to temp.");


    VHDManager vhd;

    Log("[*] Opening VHD...");
    if (!vhd.OpenVHD(vhdPath)) {
        Log("[!] Failed to open VHD: " + vhd.GetLastError(), true);
        return;
    }
    Log("[+] VHD opened successfully.");


    const auto& partitions = vhd.GetPartitions();
    Log("[*] Found " + std::to_string(partitions.size()) + " partition(s).");

    int ext4Index = -1;
    for (size_t i = 0; i < partitions.size(); i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[*]   [%zu] Type: 0x%02X, Size: %.2f MB, ext4: %s",
                 i, partitions[i].type, partitions[i].size / 1024.0 / 1024.0,
                 partitions[i].is_ext4 ? "Yes" : "No");
        Log(msg);
        if (partitions[i].is_ext4 && ext4Index < 0)
            ext4Index = (int)i;
    }

    if (ext4Index < 0) {
        Log("[!] No ext4 partition found in VHD.", true);
        vhd.CloseVHD();
        return;
    }

    Log("[*] Mounting ext4 partition " + std::to_string(ext4Index) + "...");
    if (!vhd.MountExt4Partition(ext4Index)) {
        Log("[!] Failed to mount ext4: " + vhd.GetLastError(), true);
        vhd.CloseVHD();
        return;
    }
    Log("[+] ext4 partition mounted.");


    std::string xbinDir = "/android/system/xbin";
    Log("[*] Creating " + xbinDir + " directory...");
    if (vhd.MakeDirectory(xbinDir)) {
        Log("[+] Directory created (or already exists).");
    } else {
        Log("[~] Directory may already exist: " + vhd.GetLastError());
    }


    std::string suDest = "/android/system/xbin/su";
    Log("[*] Deleting any existing su binary to prevent overwrite failure...");
    vhd.DeleteFile(suDest);

    Log("[*] Copying su_c -> " + suDest + "...");
    if (!vhd.CopyFileFromHost(resSuC, suDest)) {
        SetStatus("Failed to write su binary.", true);
        vhd.UnmountExt4();
        vhd.CloseVHD();
        return;
    }
    Log("[+] su binary copied.");


    Log("[*] Setting permissions 06755 (suid/sgid)...");
    if (vhd.SetFilePermissions(suDest, 06755)) {
        Log("[+] Permissions set: 06755");
    } else {
        Log("[!] chmod failed: " + vhd.GetLastError(), true);
    }


    Log("[*] Setting owner root:root (0:0)...");
    if (vhd.SetFileOwner(suDest, 0, 0)) {
        Log("[+] Owner set: root:root");
    } else {
        Log("[!] chown failed: " + vhd.GetLastError(), true);
    }


    Log("[*] Unmounting ext4...");
    vhd.UnmountExt4();
    Log("[*] Closing VHD...");
    vhd.CloseVHD();


    fs::remove(resSuC);


    std::string confDir = dataDir;
    if (!confDir.empty() && confDir.back() != '\\' && confDir.back() != '/') confDir += '\\';
    {
        std::string lowerConf = confDir;
        for (auto& ch : lowerConf) ch = (char)tolower((unsigned char)ch);
        size_t ePos = lowerConf.find("engine\\");
        if (ePos != std::string::npos && ePos + 7 == lowerConf.length()) {
            confDir = confDir.substr(0, ePos);
        }
    }
    std::string confPath = confDir + "bluestacks.conf";
    if (fs::exists(confPath)) {
        Log("[*] Updating bluestacks.conf for " + selectedInstance);
        std::string confContent = ReadFileString(confPath);
        std::string searchKey = "bst.instance." + selectedInstance + ".enable_root_access=";
        std::string newConf;
        std::istringstream ss(confContent);
        std::string line;
        bool replaced = false;
        while (std::getline(ss, line)) {
            if (line.find(searchKey) != std::string::npos) {
                line = searchKey + "\"0\"";
                replaced = true;
            }
            newConf += line + "\n";
        }
        if (!replaced) newConf += searchKey + "\"0\"\n";
        if (WriteFileString(confPath, newConf)) Log("[+] bluestacks.conf updated.");
        else Log("[!] Failed to write bluestacks.conf", true);
    }

    SetStatus("Rooted successfully!", false);
    ShowSystemNotification("One Click Root", "Rooted successfully!");
}



void RootTool::OneClickUnroot(const std::string& dataDir, const std::string& selectedInstance) {
    if (dataDir.empty() || selectedInstance.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    std::string masterInst = GetMasterInstanceName(selectedInstance);


    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);


    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';
    std::string instanceDir = engineDir + masterInst + "\\";
    std::string vhdPath     = instanceDir + "Root.vhd";

    if (!fs::exists(vhdPath)) {
        Log("[!] Root.vhd not found: " + vhdPath, true);
        return;
    }
    Log("[*] Found Root.vhd: " + vhdPath);


    VHDManager vhd;

    Log("[*] Opening VHD...");
    if (!vhd.OpenVHD(vhdPath)) {
        Log("[!] Failed to open VHD: " + vhd.GetLastError(), true);
        return;
    }
    Log("[+] VHD opened.");


    const auto& partitions = vhd.GetPartitions();
    int ext4Index = -1;
    for (size_t i = 0; i < partitions.size(); i++) {
        if (partitions[i].is_ext4 && ext4Index < 0)
            ext4Index = (int)i;
    }

    if (ext4Index < 0) {
        Log("[!] No ext4 partition found in VHD.", true);
        vhd.CloseVHD();
        return;
    }

    Log("[*] Mounting ext4 partition...");
    if (!vhd.MountExt4Partition(ext4Index)) {
        Log("[!] Failed to mount ext4: " + vhd.GetLastError(), true);
        vhd.CloseVHD();
        return;
    }
    Log("[+] ext4 partition mounted.");


    std::string suPath = "/android/system/xbin/su";
    Log("[*] Deleting " + suPath + "...");
    bool deleted = vhd.DeleteFile(suPath);
    if (!deleted) {
        if (vhd.FileExists(suPath)) {
            Log("[!] Failed to remove su binary: " + vhd.GetLastError(), true);
            SetStatus("Failed to remove su binary.", true);
            vhd.UnmountExt4();
            vhd.CloseVHD();
            return;
        } else {
            Log("[~] su binary not found at " + suPath + " — already unrooted.");
        }
    } else {
        Log("[+] su binary deleted successfully.");
    }


    Log("[*] Unmounting ext4...");
    vhd.UnmountExt4();
    Log("[*] Closing VHD...");
    vhd.CloseVHD();

    SetStatus("Unrooted successfully!", false);
    ShowSystemNotification("One Click Unroot", "Unrooted successfully!");
}



void RootTool::SetupTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 12.0f;
    s.WindowPadding     = { 0, 0 };
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding      = { 12, 10 };
    s.ItemSpacing       = { 12, 12 };
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = { 0.f, 0.f, 0.f, 0.f };  
    c[ImGuiCol_ChildBg]              = { 0.f, 0.f, 0.f, 0.f }; 
    c[ImGuiCol_Border]               = { 0.08f, 0.09f, 0.10f, 1.0f };
    c[ImGuiCol_FrameBg]              = { 0.08f, 0.08f, 0.11f, 1.0f };
    c[ImGuiCol_FrameBgHovered]       = { 0.10f, 0.10f, 0.14f, 1.0f };
    c[ImGuiCol_FrameBgActive]        = { 0.15f, 0.14f, 0.21f, 1.0f };
    
    c[ImGuiCol_Button]               = { 0.08f, 0.08f, 0.11f, 1.0f };
    c[ImGuiCol_ButtonHovered]        = { 0.10f, 0.10f, 0.14f, 1.0f };
    c[ImGuiCol_ButtonActive]         = { 0.56f, 0.52f, 1.0f, 1.0f };

    c[ImGuiCol_Header]               = { 0.08f, 0.08f, 0.11f, 1.0f };
    c[ImGuiCol_HeaderHovered]        = { 0.10f, 0.10f, 0.14f, 1.0f };
    c[ImGuiCol_HeaderActive]         = { 0.15f, 0.14f, 0.21f, 1.0f };

    c[ImGuiCol_CheckMark]            = { 0.56f, 0.52f, 1.0f, 1.0f };
    c[ImGuiCol_SliderGrab]           = { 0.56f, 0.52f, 1.0f, 1.0f };
    c[ImGuiCol_SliderGrabActive]     = { 0.60f, 0.58f, 1.0f, 1.0f };

    c[ImGuiCol_MenuBarBg]            = { 0.f, 0.f, 0.f, 0.f };
    c[ImGuiCol_TitleBg]              = { 0.f, 0.f, 0.f, 0.f };
    c[ImGuiCol_TitleBgActive]        = { 0.f, 0.f, 0.f, 0.f };

    c[ImGuiCol_Text]                 = { 1.00f, 1.00f, 1.00f, 1.0f };
    c[ImGuiCol_TextDisabled]         = { 0.41f, 0.41f, 0.47f, 1.0f };

    c[ImGuiCol_PopupBg]              = { 0.06f, 0.06f, 0.09f, 0.98f };
    c[ImGuiCol_ScrollbarBg]          = { 0.0f, 0.0f, 0.0f, 0.0f };
    c[ImGuiCol_ScrollbarGrab]        = { 1.0f, 1.0f, 1.0f, 0.15f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 1.0f, 1.0f, 1.0f, 0.25f };
    c[ImGuiCol_ScrollbarGrabActive]  = { 1.0f, 1.0f, 1.0f, 0.35f };
    c[ImGuiCol_Separator]            = { 1.0f, 1.0f, 1.0f, 0.08f };
    c[ImGuiCol_SeparatorHovered]     = { 1.0f, 1.0f, 1.0f, 0.12f };
    c[ImGuiCol_SeparatorActive]      = { 1.0f, 1.0f, 1.0f, 0.20f };
    s.PopupRounding = 6.0f;
    s.PopupBorderSize = 1.0f;
}

void RootTool::RenderUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags kWinFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##root_main", nullptr, kWinFlags);

    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();


    ImColor acc = ImColor(142, 132, 255, 60);
    ImColor acc0 = ImColor(142, 132, 255, 0);
    ImColor bg = ImColor(0, 0, 0, 220);
    ImColor border = ImColor(21, 23, 26, 255);
    float r = 12.0f;
    float r2 = 8.0f;
    float sidebarW = 110.0f;


    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, r);
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, r);


    ImVec2 cMin = ImVec2(pos.x + sidebarW, pos.y + 15);
    ImVec2 cMax = ImVec2(pos.x + size.x - 15, pos.y + size.y - 15);
    draw->AddRectFilled(cMin, cMax, ImColor(13, 13, 18, 255), r2);
    draw->AddRect(cMin, cMax, ImColor(19, 18, 26, 255), r2);


    draw->AddRectFilledMultiColor(ImVec2(pos.x + size.x / 2.0f, pos.y), ImVec2(pos.x + size.x, pos.y + 2), acc, acc0, acc0, acc);
    draw->AddRectFilledMultiColor(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x / 2.0f, pos.y + 2), acc0, acc, acc, acc0);
    draw->AddRectFilledMultiColor(ImVec2(pos.x + size.x / 2.0f, pos.y + size.y - 2), ImVec2(pos.x + size.x, pos.y + size.y), acc, acc0, acc0, acc);
    draw->AddRectFilledMultiColor(ImVec2(pos.x, pos.y + size.y - 2), ImVec2(pos.x + size.x / 2.0f, pos.y + size.y), acc0, acc, acc, acc0);


    {
        float titleH = 36.0f;
        float btnSize = 28.0f;
        float btnY = pos.y + (titleH - btnSize) / 2.0f;
        float rightEdge = pos.x + size.x - 15.0f;




        ImVec2 closeBtnMin = ImVec2(rightEdge - btnSize, btnY);
        ImVec2 closeBtnMax = ImVec2(rightEdge, btnY + btnSize);
        bool closeHovered = ImGui::IsMouseHoveringRect(closeBtnMin, closeBtnMax);
        draw->AddRectFilled(closeBtnMin, closeBtnMax,
            closeHovered ? ImColor(232, 17, 35, 255) : ImColor(255, 255, 255, 15), 4.0f);

        float cx = (closeBtnMin.x + closeBtnMax.x) / 2.0f;
        float cy = (closeBtnMin.y + closeBtnMax.y) / 2.0f;
        float hs = 5.0f;
        draw->AddLine(ImVec2(cx - hs, cy - hs), ImVec2(cx + hs, cy + hs), ImColor(255, 255, 255, 255), 1.5f);
        draw->AddLine(ImVec2(cx + hs, cy - hs), ImVec2(cx - hs, cy + hs), ImColor(255, 255, 255, 255), 1.5f);
        if (closeHovered && ImGui::IsMouseClicked(0))
            ::PostMessage(g_hWnd, WM_CLOSE, 0, 0);


        ImVec2 minBtnMin = ImVec2(rightEdge - btnSize * 2 - 6, btnY);
        ImVec2 minBtnMax = ImVec2(rightEdge - btnSize - 6, btnY + btnSize);
        bool minHovered = ImGui::IsMouseHoveringRect(minBtnMin, minBtnMax);
        draw->AddRectFilled(minBtnMin, minBtnMax,
            minHovered ? ImColor(255, 255, 255, 30) : ImColor(255, 255, 255, 15), 4.0f);

        float mx = (minBtnMin.x + minBtnMax.x) / 2.0f;
        float my = (minBtnMin.y + minBtnMax.y) / 2.0f;
        draw->AddLine(ImVec2(mx - hs, my), ImVec2(mx + hs, my), ImColor(255, 255, 255, 255), 1.5f);
        if (minHovered && ImGui::IsMouseClicked(0))
            ::ShowWindow(g_hWnd, SW_MINIMIZE);
    }


    const char* txt = "BSTK ROOTER";
    float y_off = pos.y + size.y / 2.0f - 100.0f;
    for (int i = 0; txt[i] != '\0'; i++) {
        if (txt[i] == ' ') { y_off += 15.0f; continue; }
        char b[2] = { txt[i], '\0' };
        ImVec2 ts = ImGui::CalcTextSize(b);
        draw->AddText(ImVec2(pos.x + sidebarW/2.0f - ts.x/2.0f, y_off), ImColor(255,255,255,255), b);
        y_off += ts.y + 6.0f;
    }


    if (m_logoTexture) {
        float logoSize = 100.0f;
        float logoX = pos.x + sidebarW / 2.0f - logoSize / 2.0f;
        float logoY = pos.y + 15.0f;
        draw->AddImage((ImTextureID)m_logoTexture,
                       ImVec2(logoX, logoY),
                       ImVec2(logoX + logoSize, logoY + logoSize));
    }
    

    float vtx_center = pos.x + sidebarW/2.0f;
    draw->AddRectFilledMultiColor(ImVec2(vtx_center - 1, pos.y + 50), ImVec2(vtx_center + 1, pos.y + size.y/2.0f - 110.0f), acc0, acc0, acc, acc);
    draw->AddRectFilledMultiColor(ImVec2(vtx_center - 1, y_off + 10), ImVec2(vtx_center + 1, pos.y + size.y - 30.0f), acc, acc, acc0, acc0);

    float contentPad = 20.0f;
    float contentLeft = sidebarW + contentPad;
    float contentW = (size.x - 15.0f) - sidebarW - contentPad * 2.0f;
    ImGui::SetCursorPos(ImVec2(contentLeft, 45.0f));

    ImGui::BeginGroup();
    {
        ImGui::TextColored(ImVec4(0.56f, 0.52f, 1.0f, 1.0f), "CONFIGURATIONS");
        ImGui::SameLine();
        ImGui::TextDisabled("Select and Apply");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Emulator: ");
        ImGui::SameLine();
        bool bstk = (m_selectedEmulator == 0);
        if (ImGui::RadioButton("BlueStacks 5",  bstk))  { m_selectedEmulator = 0; m_selectedInstance.clear(); RefreshEmulatorInfo(); }
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        if (ImGui::RadioButton("MSI App Player", !bstk)) { m_selectedEmulator = 1; m_selectedInstance.clear(); RefreshEmulatorInfo(); }

        EmulatorInfo& emu = (m_selectedEmulator == 0) ? m_bluestacks : m_msi;

        ImGui::Spacing();
        ImGui::TextDisabled("Install Dir: "); ImGui::SameLine(); ImGui::TextUnformatted(emu.installDir.c_str());
        ImGui::TextDisabled("Data Dir:    "); ImGui::SameLine(); ImGui::TextUnformatted(emu.dataDir.c_str());

        ImGui::Spacing();

        if (!emu.instances.empty() && m_selectedInstance.empty()) m_selectedInstance = emu.instances[0].instanceName;

        ImGui::Text("Instance:");
        ImGui::SameLine();


        std::string displayPreview = "(none found)";
        if (!m_selectedInstance.empty()) {
            displayPreview = m_selectedInstance;
            for (const auto& inst : emu.instances) {
                if (inst.instanceName == m_selectedInstance) {
                    displayPreview = inst.displayName + " (" + inst.instanceName + ")";
                    break;
                }
            }
        }
        std::string btnLabel = displayPreview + "##inst_dropdown";
        const char* preview = btnLabel.c_str();
        ImVec2 headerSize(300, ImGui::GetFrameHeight());
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();


        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
        if (ImGui::Button(preview, headerSize))
            m_showInstanceList = !m_showInstanceList;
        ImGui::PopStyleColor(3);

        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float arrowX = cursorPos.x + headerSize.x - 20.0f;
            float arrowY = cursorPos.y + headerSize.y / 2.0f - 2.0f;
            if (m_showInstanceList) {
                dl->AddTriangleFilled(ImVec2(arrowX, arrowY + 4), ImVec2(arrowX + 8, arrowY + 4), ImVec2(arrowX + 4, arrowY - 2), ImColor(255, 255, 255, 180));
            } else {
                dl->AddTriangleFilled(ImVec2(arrowX, arrowY), ImVec2(arrowX + 8, arrowY), ImVec2(arrowX + 4, arrowY + 6), ImColor(255, 255, 255, 180));
            }
        }

        // Play button to launch the emulator
        ImGui::SameLine();
        float btnSz = ImGui::GetFrameHeight();
        bool playClicked = ImGui::Button("##play_btn", ImVec2(btnSz, btnSz));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Launch selected instance");
            ImGui::EndTooltip();
        }

        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        float w = btnMax.x - btnMin.x;
        float h = btnMax.y - btnMin.y;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 playColor = ImGui::IsItemActive() ? IM_COL32(100, 90, 220, 255) : (ImGui::IsItemHovered() ? IM_COL32(142, 132, 255, 255) : IM_COL32(255, 255, 255, 220));

        // Triangle pointing right
        float padX = w * 0.35f;
        float padY = h * 0.3f;
        ImVec2 p1 = ImVec2(btnMin.x + padX, btnMin.y + padY);
        ImVec2 p2 = ImVec2(btnMin.x + padX, btnMax.y - padY);
        ImVec2 p3 = ImVec2(btnMax.x - padX * 0.8f, btnMin.y + h * 0.5f);
        drawList->AddTriangleFilled(p1, p2, p3, playColor);

        if (playClicked && !m_selectedInstance.empty()) {
            EmulatorInfo& emu = (m_selectedEmulator == 0) ? m_bluestacks : m_msi;
            std::string hp = emu.installDir;
            if (!hp.empty()) {
                if (hp.back() != '\\') hp += '\\';
                hp += "HD-Player.exe";
                LaunchEmulator(hp, "--instance " + m_selectedInstance);
                SetStatus("Opening emulator instance: " + m_selectedInstance, false);
            }
        }

        // Master/Clone indicator
        if (!m_selectedInstance.empty()) {
            ImGui::SameLine();
            if (IsMasterInstance(m_selectedInstance)) {
                ImGui::TextColored(ImVec4(0.56f, 0.52f, 1.0f, 1.0f), "[Master]");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[Clone]");
            }
        }

        if (m_showInstanceList && !emu.instances.empty()) {
            int maxVisible = (std::min)((int)emu.instances.size(), 5);
            float listH = ImGui::GetTextLineHeightWithSpacing() * maxVisible + ImGui::GetStyle().FramePadding.y * 2;
            if (ImGui::BeginListBox("##instlist", ImVec2(300, listH))) {
                for (const auto& inst : emu.instances) {
                    bool sel = (m_selectedInstance == inst.instanceName);
                    std::string label = inst.displayName + " (" + inst.instanceName + ")##" + inst.instanceName;
                    if (ImGui::Selectable(label.c_str(), sel)) {
                        m_selectedInstance = inst.instanceName;
                        m_showInstanceList = false;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndListBox();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();


        const bool  canAct = emu.found();
        const bool  hasInstance = !m_selectedInstance.empty();

          
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float btnW = (contentW - spacing) / 2.0f;
        const float btnH = 45.0f;

        auto StepBtn = [&](const char* label, bool enabled) -> bool {
            if (!enabled || m_isBusy) ImGui::BeginDisabled();
            bool clicked = ImGui::Button(label, { btnW, btnH });
            if (!enabled || m_isBusy) ImGui::EndDisabled();
            return clicked;
        };
        
        if (StepBtn("Kill Emulator Processes", true)) KillProcesses();
        ImGui::SameLine();
        if (StepBtn("Fix Illegally Tampered", canAct)) {
            std::string iDir = emu.installDir;
            m_isBusy = true; std::thread([this, iDir]() { PatchHDPlayer(iDir); m_isBusy = false; }).detach();
        }


        std::string masterInst = GetMasterInstanceName(m_selectedInstance);

        if (StepBtn("Disk R/W", canAct && hasInstance)) {
            std::string dDir = emu.dataDir; std::string mi = masterInst;
            m_isBusy = true; std::thread([this, dDir, mi]() { ApplyRootConfigs(dDir, mi); m_isBusy = false; }).detach();
        }
        ImGui::SameLine();
        if (StepBtn("Disk R/O", canAct && hasInstance)) {
            std::string dDir = emu.dataDir; std::string mi = masterInst;
            m_isBusy = true; std::thread([this, dDir, mi]() { RevertDiskToReadonly(dDir, mi); m_isBusy = false; }).detach();
        }

        if (StepBtn("One Click Root", canAct && hasInstance)) {
            std::string dDir = emu.dataDir; std::string si = m_selectedInstance;
            m_isBusy = true; std::thread([this, dDir, si]() { OneClickRoot(dDir, si); m_isBusy = false; }).detach();
        }
        ImGui::SameLine();
        if (StepBtn("One Click Unroot", canAct && hasInstance)) {
            std::string dDir = emu.dataDir; std::string si = m_selectedInstance;
            m_isBusy = true; std::thread([this, dDir, si]() { OneClickUnroot(dDir, si); m_isBusy = false; }).detach();
        }
        
        if (StepBtn("Install Kitsune Magisk", canAct && hasInstance)) {
            std::string d = emu.dataDir; std::string s = m_selectedInstance;
            m_isBusy = true; std::thread([this, d, s]() { InstallKitsuneMagisk(d, s); m_isBusy = false; }).detach();
        }
        ImGui::SameLine();
        if (StepBtn("Uninstall Kitsune Magisk", canAct && hasInstance)) {
            std::string d = emu.dataDir; std::string s = m_selectedInstance;
            m_isBusy = true; std::thread([this, d, s]() { UninstallKitsuneMagisk(d, s); m_isBusy = false; }).detach();
        }
        
        ImGui::Spacing();
        std::string currentStatusMsg;
        bool currentStatusIsError;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            currentStatusMsg = m_statusMsg;
            currentStatusIsError = m_statusIsError;
        }
        if (!currentStatusMsg.empty()) {
            ImColor col = currentStatusIsError ? ImColor(255, 77, 77, 255) : ImColor(142, 132, 255, 255);
            ImGui::TextColored(col, "%s %s", currentStatusIsError ? "[!]" : "[OK]", currentStatusMsg.c_str());
        } else {
            ImGui::TextDisabled("Ready to operate.");
        }
    }
    ImGui::EndGroup();

    // "TaaauuJi" Branding
    {
        ImGui::PushFont(m_brandingFont ? m_brandingFont : ImGui::GetFont());
        const char* brandingText = "TaaauuJi";
        ImVec2 textSize = ImGui::CalcTextSize(brandingText);
        
        // Align to bottom-right of content panel with padding
        ImVec2 brandingPos = ImVec2(cMax.x - textSize.x - 20.0f, cMax.y - textSize.y - 15.0f);
        
        ImGui::SetCursorScreenPos(brandingPos);
        ImGui::InvisibleButton("##branding_btn", textSize);
        
        bool isHovered = ImGui::IsItemHovered();
        bool isActive = ImGui::IsItemActive();
        
        ImColor textColor;
        if (isActive) {
            textColor = ImColor(142, 132, 255, 255);
        } else if (isHovered) {
            textColor = ImColor(180, 175, 255, 230);
        } else {
            textColor = ImColor(255, 255, 255, 50); // minimally visible
        }
        
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (isHovered) {
            // Glow effect
            for (int i = 1; i <= 3; ++i) {
                dl->AddText(m_brandingFont, ImGui::GetFontSize(), 
                            ImVec2(brandingPos.x, brandingPos.y), 
                            ImColor(142, 132, 255, (int)(40 / i)), 
                            brandingText);
            }
        }
        dl->AddText(m_brandingFont, ImGui::GetFontSize(), brandingPos, textColor, brandingText);
        
        if (isHovered && ImGui::IsMouseClicked(0)) {
            ::ShellExecuteA(nullptr, "open", "https://github.com/TaaauuJi", nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::PopFont();
    }

    ImGui::End();
}
