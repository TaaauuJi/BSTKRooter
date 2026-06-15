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

#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

class VHDManager;



struct BstkInstance {
    std::string displayName;
    std::string instanceName;
};

struct EmulatorInfo {
    bool        isBlueStacks = true;
    std::string name;
    std::string installDir;
    std::string dataDir;
    std::vector<BstkInstance> instances;
    bool        found() const { return !installDir.empty(); }
};

struct ImFont;

class RootTool {
public:
    RootTool();
    ~RootTool();


    static void SetupTheme();
    void        RenderUI();

    void SetLogo(void* srv, int w, int h) {
        m_logoTexture = srv;
        m_logoWidth = w;
        m_logoHeight = h;
    }

    void SetBrandingFont(ImFont* font) {
        m_brandingFont = font;
    }

private:

    void RefreshEmulatorInfo();
    void FindInstances(EmulatorInfo& info);


    void KillProcesses();
    void PatchHDPlayer(const std::string& installDir);
    void ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName);
    void RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName);
    void OneClickRoot(const std::string& dataDir, const std::string& instanceName);
    void OneClickUnroot(const std::string& dataDir, const std::string& instanceName);

    // Kitsune Magisk methods
    void InstallKitsuneMagisk(const std::string& dataDir, const std::string& selectedInstance);
    void UninstallKitsuneMagisk(const std::string& dataDir, const std::string& selectedInstance);

    // Helpers
    void        LaunchEmulator(const std::string& exePath, const std::string& args);
    std::string ExtractResourceToTemp(int resourceId, const char* tmpName);
    std::string FindDataVhdx(const std::string& instanceDir);


    std::string ReadRegistryString(const std::string& subKey, const std::string& valueName);
    std::string ReadFileString(const std::string& path);
    bool        WriteFileString(const std::string& path, const std::string& content);
    void        Log(const std::string& msg, bool isError = false);
    void        SetStatus(const std::string& msg, bool isError);
    void        ShowSystemNotification(const std::string& title, const std::string& message);
    bool        IsMasterInstance(const std::string& instanceName);
    std::string GetMasterInstanceName(const std::string& instanceName);



    EmulatorInfo m_bluestacks;
    EmulatorInfo m_msi;
    int          m_selectedEmulator = 0;
    std::string  m_selectedInstance;
    std::string  m_statusMsg;
    bool         m_statusIsError = false;
    std::mutex   m_statusMutex;
    std::atomic<bool> m_isBusy{false};

    void* m_logoTexture = nullptr;
    int   m_logoWidth   = 0;
    int   m_logoHeight  = 0;
    bool  m_showInstanceList = false;
    ImFont* m_brandingFont = nullptr;
};
