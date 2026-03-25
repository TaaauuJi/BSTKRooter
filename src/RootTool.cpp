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

#include "VHDManager.h"

namespace fs = std::filesystem;



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



RootTool::RootTool() { RefreshEmulatorInfo(); }

void RootTool::Log(const std::string& /*msg*/, bool /*isError*/) {
}

void RootTool::SetStatus(const std::string& msg, bool isError) {
    m_statusMsg     = msg;
    m_statusIsError = isError;
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
    else
        SetStatus("Patched successfully!", false);
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


    constexpr uint8_t kXorKey = 0xA7;
    std::vector<uint8_t> suDecrypted(suSize);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(suData);
    for (DWORD i = 0; i < suSize; i++)
        suDecrypted[i] = src[i] ^ kXorKey;


    char tempDir[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tempDir);
    std::string resSuC = std::string(tempDir) + "bstk_su_c.tmp";
    {
        std::ofstream tmp(resSuC, std::ios::binary);
        if (!tmp) {
            SetStatus("Failed to create temp file.", true);
            return;
        }
        tmp.write(reinterpret_cast<const char*>(suDecrypted.data()), suSize);
    }
    Log("[*] Decrypted embedded su (" + std::to_string(suSize) + " bytes) to temp.");


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
    if (vhd.FileExists(suPath)) {
        Log("[*] Deleting " + suPath + "...");
        if (vhd.DeleteFile(suPath)) {
        } else {
            SetStatus("Failed to remove su binary.", true);
        }
    } else {
        Log("[~] su binary not found at " + suPath + " — already unrooted.");
    }


    Log("[*] Unmounting ext4...");
    vhd.UnmountExt4();
    Log("[*] Closing VHD...");
    vhd.CloseVHD();

    SetStatus("Unrooted successfully!", false);
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
    ImColor bg = ImColor(0, 0, 0, 200);
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
        extern HWND g_hWnd;
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
        
        if (!m_selectedInstance.empty()) {
            ImGui::SameLine();
            if (IsMasterInstance(m_selectedInstance)) {
                ImGui::TextColored(ImVec4(0.56f, 0.52f, 1.0f, 1.0f), "[Master]");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[Clone]");
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
            if (!enabled) ImGui::BeginDisabled();
            bool clicked = ImGui::Button(label, { btnW, btnH });
            if (!enabled) ImGui::EndDisabled();
            return clicked;
        };
        
        if (StepBtn("Kill Emulator Processes", true)) KillProcesses();
        ImGui::SameLine();
        if (StepBtn("Fix Illegally Tampered", canAct)) PatchHDPlayer(emu.installDir);


        std::string masterInst = GetMasterInstanceName(m_selectedInstance);

        if (StepBtn("Disk R/W", canAct && hasInstance)) ApplyRootConfigs(emu.dataDir, masterInst);
        ImGui::SameLine();
        if (StepBtn("Disk R/O", canAct && hasInstance)) RevertDiskToReadonly(emu.dataDir, masterInst);

        if (StepBtn("One Click Root", canAct && hasInstance)) OneClickRoot(emu.dataDir, m_selectedInstance);
        ImGui::SameLine();
        if (StepBtn("One Click Unroot", canAct && hasInstance)) OneClickUnroot(emu.dataDir, m_selectedInstance);
        
        ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
        if (!m_statusMsg.empty()) {
            ImColor col = m_statusIsError ? ImColor(255, 77, 77, 255) : ImColor(142, 132, 255, 255);
            ImGui::TextColored(col, "%s %s", m_statusIsError ? "[!]" : "[OK]", m_statusMsg.c_str());
        } else {
            ImGui::TextDisabled("Ready to operate.");
        }
    }
    ImGui::EndGroup();

    ImGui::End();
}
