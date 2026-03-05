#include "RootTool.h"
#include "resources.h"
#include <imgui.h>
#include <windows.h>
#include <TlHelp32.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <cstring>

// VHDManager from ext4handle
#include "VHDManager.h"

namespace fs = std::filesystem;

// ─── Internals ────────────────────────────────────────────────────────────────

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

// ─── Construction / Discovery ─────────────────────────────────────────────────

RootTool::RootTool() { RefreshEmulatorInfo(); }

void RootTool::Log(const std::string& msg, bool /*isError*/) {
    m_log += msg + "\n";
    m_scrollToBottom = true;
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

    // DataDir from registry may already end with "Engine\" or may be the parent.
    // Determine the actual Engine directory.
    std::string engineDir = info.dataDir;

    // Normalize: ensure trailing backslash
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';

    // If DataDir already ends with Engine\, use it directly.
    // Otherwise append Engine\.
    bool alreadyHasEngine = false;
    {
        std::string lower = engineDir;
        for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
        if (lower.find("engine\\") != std::string::npos ||
            lower.find("engine/")  != std::string::npos)
            alreadyHasEngine = true;
    }

    if (!alreadyHasEngine) {
        engineDir += "Engine\\";
    }

    if (!fs::exists(engineDir)) return;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(engineDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();

        // Skip known non-instance directories
        if (name == "Manager" || name == "UserData") continue;

        // Validate: a real instance directory contains a .bstk file
        bool hasBstk = false;
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
            if (f.path().extension() == ".bstk") { hasBstk = true; break; }
        }
        if (hasBstk)
            info.instances.push_back(name);
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

    // Auto-select first available instance
    if (!m_bluestacks.instances.empty())  m_selectedInstance = m_bluestacks.instances[0];
    else if (!m_msi.instances.empty())    m_selectedInstance = m_msi.instances[0];
}

// ─── File Helpers ─────────────────────────────────────────────────────────────

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

// ─── Helpers ──────────────────────────────────────────────────────────────────

bool RootTool::IsMasterInstance(const std::string& instanceName) {
    // Master instances do NOT have _1, _2, _3 etc. suffix
    // e.g. "Pie64" is master, "Pie64_1" is a clone
    std::regex suffixPattern(".*_\\d+$");
    return !std::regex_match(instanceName, suffixPattern);
}

// ─── Actions ──────────────────────────────────────────────────────────────────

void RootTool::KillProcesses() {
    KillProcessByName("HD-Player.exe");
    KillProcessByName("HD-MultiInstanceManager.exe");
    KillProcessByName("BstkSVC.exe");
    Log("[+] Emulator processes killed.");
}

void RootTool::PatchHDPlayer(const std::string& installDir) {
    if (installDir.empty()) { Log("[!] InstallDir not found.", true); return; }

    // Auto-kill emulator first
    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    std::string exePath  = installDir + "HD-Player.exe";
    std::string bakPath  = exePath    + ".bak";

    if (!fs::exists(exePath)) { Log("[!] HD-Player.exe not found: " + exePath, true); return; }

    // Backup (only once)
    if (!fs::exists(bakPath)) {
        std::error_code ec;
        fs::copy_file(exePath, bakPath, ec);
        if (ec) { Log("[!] Backup failed: " + ec.message(), true); return; }
        Log("[*] Backup created: " + bakPath);
    } else {
        Log("[*] Backup already exists, skipping copy.");
    }

    // Read binary into memory
    std::ifstream f(exePath, std::ios::binary | std::ios::ate);
    if (!f) { Log("[!] Cannot open HD-Player.exe. Close the emulator first.", true); return; }
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0); f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
    f.close();

    char msg[256];
    snprintf(msg, sizeof(msg), "[*] Loaded %s (%zu bytes)", exePath.c_str(), buf.size());
    Log(msg);

    // ── PE helpers (inline lambdas) ──────────────────────────────────────────
    auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > buf.size()) return 0;
        return (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
               ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
    };
    auto rd16 = [&](size_t off) -> uint16_t {
        if (off + 2 > buf.size()) return 0;
        return (uint16_t)buf[off] | ((uint16_t)buf[off+1]<<8);
    };

    // ── Parse PE header ─────────────────────────────────────────────────────
    if (buf.size() < 0x40) { Log("[!] File too small for PE.", true); return; }
    size_t peOff = rd32(0x3C);
    if (peOff + 24 >= buf.size() || buf[peOff] != 'P' || buf[peOff+1] != 'E') {
        Log("[!] Invalid PE header.", true); return;
    }

    int    numSections = rd16(peOff + 6);
    size_t optHdrSize  = rd16(peOff + 20);
    size_t secTableOff = peOff + 24 + optHdrSize;

    // Section info
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

    // Find .text and .rdata sections
    const SecInfo* textSec = nullptr;
    for (auto& s : secs)
        if (strncmp(s.name, ".text", 5) == 0) { textSec = &s; break; }
    if (!textSec) { Log("[!] .text section not found.", true); return; }

    size_t textStart = textSec->rawOff;
    size_t textEnd   = textSec->rawOff + textSec->rawSz;

    // RVA-to-file-offset converter
    auto rvaToFile = [&](uint32_t rva) -> size_t {
        for (auto& s : secs)
            if (rva >= s.va && rva < s.va + s.vsz)
                return s.rawOff + (rva - s.va);
        return (size_t)-1;
    };

    // Search buffer for ASCII string, returns file offsets
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

    // Check: test al,al (84 C0) + jz (74 XX) preceded immediately by CALL (E8 XX XX XX XX)
    auto isTestJzAfterCall = [&](size_t testOff) -> bool {
        if (testOff + 4 > buf.size()) return false;
        if (buf[testOff] != 0x84 || buf[testOff+1] != 0xC0) return false;
        if (buf[testOff+2] != 0x74) return false;
        if (testOff < 5) return false;
        return (buf[testOff - 5] == 0xE8);
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  UNIVERSAL PATCH-SITE FINDER (4 strategies from patcher_tool)
    // ═════════════════════════════════════════════════════════════════════════
    size_t patchOffset = std::string::npos;
    int    patchMethod = 0;
    std::string patchMethodDesc;

    // Helper: scan .text for LEA xrefs to a given RVA, walk backward for test+jz
    auto findViaStringAnchor = [&](const char* anchor, int method,
                                    const char* methodName, int backRange) -> bool
    {
        auto strHits = findString(anchor);
        if (strHits.empty()) return false;

        snprintf(msg, sizeof(msg), "[*]   Found \"%s\" at %zu offset(s)", anchor, strHits.size());
        Log(msg);

        for (size_t strFileOff : strHits) {
            // Calculate RVA of this string
            uint32_t strRva = 0;
            for (auto& s : secs) {
                if (strFileOff >= s.rawOff && strFileOff < s.rawOff + s.rawSz) {
                    strRva = s.va + (uint32_t)(strFileOff - s.rawOff);
                    break;
                }
            }
            if (strRva == 0) continue;

            // Scan .text for LEA instructions referencing this string via RIP-relative
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

                // Walk backward from LEA to find test al,al + jz
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

    // ── Strategy 1: Anchor on "Verified the disk integrity!" ──────────────
    Log("[*] Strategy 1: Searching for \"Verified the disk integrity!\" anchor...");
    if (!findViaStringAnchor("Verified the disk integrity!", 1,
                              "Anchor: \"Verified the disk integrity!\"", 80))
    {
        Log("[~]   Strategy 1 failed.");

        // ── Strategy 2: Anchor on "plrDiskCheckThreadEntry" ──────────────
        Log("[*] Strategy 2: Searching for \"plrDiskCheckThreadEntry\" anchor...");
        if (!findViaStringAnchor("plrDiskCheckThreadEntry", 2,
                                  "Anchor: \"plrDiskCheckThreadEntry\"", 0x700))
        {
            Log("[~]   Strategy 2 failed.");

            // ── Strategy 3: Anchor on shutdown message ───────────────────
            Log("[*] Strategy 3: Searching for shutdown tamper message anchor...");
            if (!findViaStringAnchor(
                    "Shutting down: disk file have been illegally tampered with!", 3,
                    "Anchor: shutdown tamper message", 0x700))
            {
                Log("[~]   Strategy 3 failed.");

                // ── Strategy 4: Full .text scan with validation ──────────
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
                    // Narrow down by checking for "Verified" or "Failed to verify" LEA nearby
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

    // Already patched?
    if (buf[patchOffset+2] == 0x90 && buf[patchOffset+3] == 0x90) {
        Log("[~] Already patched. Nothing to do.");
        return;
    }

    // Apply: 74 ?? -> 90 90  (jz rel8 -> NOP NOP)
    snprintf(msg, sizeof(msg), "[*] Patching at 0x%zX: %02X %02X -> 90 90",
             patchOffset + 2, buf[patchOffset+2], buf[patchOffset+3]);
    Log(msg);
    buf[patchOffset + 2] = 0x90;
    buf[patchOffset + 3] = 0x90;

    std::ofstream out(exePath, std::ios::binary);
    if (!out || !out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size()))
        Log("[!] Failed to write HD-Player.exe -- run as Administrator.", true);
    else
        Log("[+] Patched successfully!");
}

void RootTool::ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    // Auto-kill emulator first
    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    // Resolve paths: DataDir may already be the Engine dir
    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';

    // ── .bstk file ─────────────────────────────────────────────────────
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

        if (!WriteFileString(filepath, result))
            Log("[!] Failed to write " + filepath + " — run as Administrator.", true);
        else
            Log("[+] " + filepath + " updated (Readonly -> Normal).");
    }

    Log("[+] Disk R/W configs applied.");
}

void RootTool::RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    // Auto-kill emulator first
    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    // Resolve paths
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

        if (!WriteFileString(filepath, result))
            Log("[!] Failed to write " + filepath + " — run as Administrator.", true);
        else
            Log("[+] " + filepath + " reverted (Normal -> Readonly).");
    }

    Log("[+] Disk reverted to Readonly.");
}

// ─── OneClickRoot ─────────────────────────────────────────────────────────────
// Uses VHDManager (ext4handle) to directly manipulate the VHD:
//   open VHD → mount ext4 → create dir → copy su → set perms → unmount

void RootTool::OneClickRoot(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    // Only works on master instances (no _1, _2 suffix)
    if (!IsMasterInstance(instanceName)) {
        Log("[!] One Click Root only works on master instances (without _1, _2 suffix).", true);
        Log("[~] Instance '" + instanceName + "' appears to be a clone.", true);
        return;
    }

    // Auto-kill emulator first
    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    // ── Locate Root.vhd ─────────────────────────────────────────────────────
    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';
    std::string instanceDir = engineDir + instanceName + "\\";
    std::string vhdPath     = instanceDir + "Root.vhd";

    if (!fs::exists(vhdPath)) {
        Log("[!] Root.vhd not found: " + vhdPath, true);
        return;
    }
    Log("[*] Found Root.vhd: " + vhdPath);

    // ── Extract embedded su_c resource to temp file ─────────────────────────
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(IDR_SU_BINARY), RT_RCDATA);
    if (!hRes) {
        Log("[!] Embedded su_c resource not found in EXE.", true);
        return;
    }
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        Log("[!] Failed to load embedded su_c resource.", true);
        return;
    }
    DWORD    suSize = SizeofResource(nullptr, hRes);
    const void* suData = LockResource(hData);
    if (!suData || suSize == 0) {
        Log("[!] Embedded su_c resource is empty.", true);
        return;
    }

    // Write to a temp file for VHDManager::CopyFileFromHost
    char tempDir[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tempDir);
    std::string resSuC = std::string(tempDir) + "bstk_su_c.tmp";
    {
        std::ofstream tmp(resSuC, std::ios::binary);
        if (!tmp) {
            Log("[!] Cannot create temp file for su binary.", true);
            return;
        }
        tmp.write(reinterpret_cast<const char*>(suData), suSize);
    }
    Log("[*] Extracted embedded su_c (" + std::to_string(suSize) + " bytes) to temp.");

    // ── Open VHD via VHDManager ─────────────────────────────────────────────
    VHDManager vhd;

    Log("[*] Opening VHD...");
    if (!vhd.OpenVHD(vhdPath)) {
        Log("[!] Failed to open VHD: " + vhd.GetLastError(), true);
        return;
    }
    Log("[+] VHD opened successfully.");

    // ── Find and mount ext4 partition ────────────────────────────────────────
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

    // ── Create /android/system/xbin directory ───────────────────────────────
    std::string xbinDir = "/android/system/xbin";
    Log("[*] Creating " + xbinDir + " directory...");
    if (vhd.MakeDirectory(xbinDir)) {
        Log("[+] Directory created (or already exists).");
    } else {
        Log("[~] Directory may already exist: " + vhd.GetLastError());
    }

    // ── Copy su_c → /android/system/xbin/su ─────────────────────────────────
    std::string suDest = "/android/system/xbin/su";
    Log("[*] Copying su_c -> " + suDest + "...");
    if (!vhd.CopyFileFromHost(resSuC, suDest)) {
        Log("[!] Failed to copy su binary: " + vhd.GetLastError(), true);
        vhd.UnmountExt4();
        vhd.CloseVHD();
        return;
    }
    Log("[+] su binary copied.");

    // ── Set permissions to 06755 (setuid + setgid + rwxr-xr-x) ─────────────
    Log("[*] Setting permissions 06755 (suid/sgid)...");
    if (vhd.SetFilePermissions(suDest, 06755)) {
        Log("[+] Permissions set: 06755");
    } else {
        Log("[!] chmod failed: " + vhd.GetLastError(), true);
    }

    // ── Set owner to root:root (uid=0, gid=0) ──────────────────────────────
    Log("[*] Setting owner root:root (0:0)...");
    if (vhd.SetFileOwner(suDest, 0, 0)) {
        Log("[+] Owner set: root:root");
    } else {
        Log("[!] chown failed: " + vhd.GetLastError(), true);
    }

    // ── Unmount and close ───────────────────────────────────────────────────
    Log("[*] Unmounting ext4...");
    vhd.UnmountExt4();
    Log("[*] Closing VHD...");
    vhd.CloseVHD();

    // Clean up temp file
    fs::remove(resSuC);

    Log("[+] One Click Root complete! su installed at /system/xbin/su");
}

// ─── OneClickUnroot ───────────────────────────────────────────────────────────
// Delete su from VHD to reverse rooting

void RootTool::OneClickUnroot(const std::string& dataDir, const std::string& instanceName) {
    if (dataDir.empty() || instanceName.empty()) {
        Log("[!] DataDir or instance name is empty.", true); return;
    }

    // Only works on master instances
    if (!IsMasterInstance(instanceName)) {
        Log("[!] One Click Unroot only works on master instances (without _1, _2 suffix).", true);
        Log("[~] Instance '" + instanceName + "' appears to be a clone.", true);
        return;
    }

    // Auto-kill emulator first
    Log("[*] Killing emulator processes...");
    KillProcesses();
    ::Sleep(1000);

    // ── Locate Root.vhd ─────────────────────────────────────────────────────
    std::string engineDir = dataDir;
    if (!engineDir.empty() && engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';
    std::string instanceDir = engineDir + instanceName + "\\";
    std::string vhdPath     = instanceDir + "Root.vhd";

    if (!fs::exists(vhdPath)) {
        Log("[!] Root.vhd not found: " + vhdPath, true);
        return;
    }
    Log("[*] Found Root.vhd: " + vhdPath);

    // ── Open VHD via VHDManager ─────────────────────────────────────────────
    VHDManager vhd;

    Log("[*] Opening VHD...");
    if (!vhd.OpenVHD(vhdPath)) {
        Log("[!] Failed to open VHD: " + vhd.GetLastError(), true);
        return;
    }
    Log("[+] VHD opened.");

    // ── Find and mount ext4 partition ────────────────────────────────────────
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

    // ── Delete /android/system/xbin/su ──────────────────────────────────────
    std::string suPath = "/android/system/xbin/su";
    if (vhd.FileExists(suPath)) {
        Log("[*] Deleting " + suPath + "...");
        if (vhd.DeleteFile(suPath)) {
            Log("[+] su binary deleted.");
        } else {
            Log("[!] Failed to delete su: " + vhd.GetLastError(), true);
        }
    } else {
        Log("[~] su binary not found at " + suPath + " — already unrooted.");
    }

    // ── Unmount and close ───────────────────────────────────────────────────
    Log("[*] Unmounting ext4...");
    vhd.UnmountExt4();
    Log("[*] Closing VHD...");
    vhd.CloseVHD();

    Log("[+] One Click Unroot complete! su removed from VHD.");
}

// ─── UI ───────────────────────────────────────────────────────────────────────

void RootTool::SetupTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding      = { 8, 5 };
    s.ItemSpacing       = { 8, 6 };
    s.WindowPadding     = { 12, 12 };
    s.IndentSpacing     = 16.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]          = { 0.07f, 0.07f, 0.07f, 1.00f };
    c[ImGuiCol_ChildBg]           = { 0.05f, 0.05f, 0.05f, 1.00f };
    c[ImGuiCol_PopupBg]           = { 0.10f, 0.10f, 0.10f, 1.00f };
    c[ImGuiCol_Border]            = { 0.15f, 0.30f, 0.15f, 0.60f };
    c[ImGuiCol_TitleBg]           = { 0.02f, 0.14f, 0.02f, 1.00f };
    c[ImGuiCol_TitleBgActive]     = { 0.02f, 0.20f, 0.02f, 1.00f };
    c[ImGuiCol_MenuBarBg]         = { 0.05f, 0.10f, 0.05f, 1.00f };
    c[ImGuiCol_Header]            = { 0.10f, 0.35f, 0.10f, 0.70f };
    c[ImGuiCol_HeaderHovered]     = { 0.15f, 0.50f, 0.15f, 0.80f };
    c[ImGuiCol_HeaderActive]      = { 0.10f, 0.40f, 0.10f, 1.00f };
    c[ImGuiCol_Button]            = { 0.10f, 0.38f, 0.10f, 1.00f };
    c[ImGuiCol_ButtonHovered]     = { 0.18f, 0.58f, 0.18f, 1.00f };
    c[ImGuiCol_ButtonActive]      = { 0.08f, 0.30f, 0.08f, 1.00f };
    c[ImGuiCol_FrameBg]           = { 0.10f, 0.10f, 0.10f, 1.00f };
    c[ImGuiCol_FrameBgHovered]    = { 0.14f, 0.22f, 0.14f, 1.00f };
    c[ImGuiCol_FrameBgActive]     = { 0.12f, 0.30f, 0.12f, 1.00f };
    c[ImGuiCol_CheckMark]         = { 0.30f, 0.90f, 0.30f, 1.00f };
    c[ImGuiCol_SliderGrab]        = { 0.25f, 0.75f, 0.25f, 1.00f };
    c[ImGuiCol_SliderGrabActive]  = { 0.35f, 0.95f, 0.35f, 1.00f };
    c[ImGuiCol_SeparatorHovered]  = { 0.20f, 0.65f, 0.20f, 1.00f };
    c[ImGuiCol_SeparatorActive]   = { 0.25f, 0.80f, 0.25f, 1.00f };
    c[ImGuiCol_Tab]               = { 0.06f, 0.20f, 0.06f, 1.00f };
    c[ImGuiCol_TabHovered]        = { 0.18f, 0.55f, 0.18f, 1.00f };
    c[ImGuiCol_TabActive]         = { 0.12f, 0.40f, 0.12f, 1.00f };
    c[ImGuiCol_ScrollbarBg]       = { 0.04f, 0.04f, 0.04f, 1.00f };
    c[ImGuiCol_ScrollbarGrab]     = { 0.15f, 0.40f, 0.15f, 1.00f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 0.22f, 0.58f, 0.22f, 1.00f };
    c[ImGuiCol_ScrollbarGrabActive]  = { 0.12f, 0.32f, 0.12f, 1.00f };
    c[ImGuiCol_Text]              = { 0.88f, 0.95f, 0.88f, 1.00f };
    c[ImGuiCol_TextDisabled]      = { 0.45f, 0.55f, 0.45f, 1.00f };
}

void RootTool::RenderUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags kWinFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##root_main", nullptr, kWinFlags);

    // ── Title bar ────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.95f, 0.30f, 1.0f));
    ImGui::Text(" BSTK ROOTER");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("BlueStacks 5 / MSI App Player");
    ImGui::Separator();
    ImGui::Spacing();

    // ── Emulator selection ───────────────────────────────────────────────────
    ImGui::Text("Emulator:");
    ImGui::SameLine();
    bool bstk = (m_selectedEmulator == 0);
    if (ImGui::RadioButton("BlueStacks 5",  bstk))  { m_selectedEmulator = 0; m_selectedInstance.clear(); RefreshEmulatorInfo(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("MSI App Player", !bstk)) { m_selectedEmulator = 1; m_selectedInstance.clear(); RefreshEmulatorInfo(); }

    EmulatorInfo& emu = (m_selectedEmulator == 0) ? m_bluestacks : m_msi;

    ImGui::Spacing();

    // ── Path info ────────────────────────────────────────────────────────────
    auto LabeledPath = [](const char* label, const std::string& val) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        if (val.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
            ImGui::Text("NOT FOUND");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(val.c_str());
        }
    };

    LabeledPath("Install Dir: ", emu.installDir);
    LabeledPath("Data Dir:    ", emu.dataDir);

    ImGui::Spacing();

    // ── Instance selector ────────────────────────────────────────────────────
    ImGui::Text("Instance:");
    ImGui::SameLine();

    // Sync selected instance when emulator switches
    if (!emu.instances.empty() && m_selectedInstance.empty())
        m_selectedInstance = emu.instances[0];

    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##inst", m_selectedInstance.empty() ? "(none found)" : m_selectedInstance.c_str())) {
        for (const auto& inst : emu.instances) {
            bool sel = (m_selectedInstance == inst);
            if (ImGui::Selectable(inst.c_str(), sel)) m_selectedInstance = inst;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Show master/clone indicator
    if (!m_selectedInstance.empty()) {
        ImGui::SameLine();
        if (IsMasterInstance(m_selectedInstance)) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "(Master)");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "(Clone)");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Action buttons ───────────────────────────────────────────────────────
    const float btnW = 260.0f;
    const float btnH = 36.0f;
    const bool  canAct = emu.found();
    const bool  hasInstance = !m_selectedInstance.empty();
    const bool  isMaster = hasInstance && IsMasterInstance(m_selectedInstance);

    auto StepBtn = [&](const char* label, bool enabled) -> bool {
        if (!enabled) ImGui::BeginDisabled();
        bool clicked = ImGui::Button(label, { btnW, btnH });
        if (!enabled) ImGui::EndDisabled();
        return clicked;
    };

    if (StepBtn("1.  Kill Emulator Processes", true))
        KillProcesses();

    ImGui::SameLine();
    ImGui::TextDisabled("  Stop HD-Player & services");

    if (StepBtn("2.  Fix Illegally Tempered", canAct))
        PatchHDPlayer(emu.installDir);

    ImGui::SameLine();
    ImGui::TextDisabled("  Disk integrity bypass (auto-kills emu)");

    if (StepBtn("3.  Disk R/W", canAct && hasInstance))
        ApplyRootConfigs(emu.dataDir, m_selectedInstance);

    ImGui::SameLine();
    ImGui::TextDisabled("  .bstk: Readonly -> Normal (auto-kills emu)");

    if (StepBtn("4.  Disk R/O (Revert)", canAct && hasInstance))
        RevertDiskToReadonly(emu.dataDir, m_selectedInstance);

    ImGui::SameLine();
    ImGui::TextDisabled("  .bstk: Normal -> Readonly (auto-kills emu)");

    if (StepBtn("5.  One Click Root", canAct && isMaster))
        OneClickRoot(emu.dataDir, m_selectedInstance);

    ImGui::SameLine();
    ImGui::TextDisabled("  Install su via VHD (master only, auto-kills emu)");

    if (StepBtn("6.  One Click Unroot", canAct && isMaster))
        OneClickUnroot(emu.dataDir, m_selectedInstance);

    ImGui::SameLine();
    ImGui::TextDisabled("  Delete su from VHD (master only, auto-kills emu)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Console log ──────────────────────────────────────────────────────────
    ImGui::Text("Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { m_log.clear(); }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.06f, 0.03f, 1.0f));
    ImGui::BeginChild("##log", { 0, 0 }, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.95f, 0.60f, 1.0f));
    ImGui::TextUnformatted(m_log.c_str());
    ImGui::PopStyleColor();
    if (m_scrollToBottom) { ImGui::SetScrollHereY(1.0f); m_scrollToBottom = false; }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
}
