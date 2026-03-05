#pragma once
#include <string>
#include <vector>

// Forward declarations
class VHDManager;

struct EmulatorInfo {
    bool        isBlueStacks = true;
    std::string name;
    std::string installDir;
    std::string dataDir;
    std::vector<std::string> instances;
    bool        found() const { return !installDir.empty(); }
};

class RootTool {
public:
    RootTool();

    // Call once after ImGui context is created to apply the theme
    static void SetupTheme();
    void        RenderUI();

private:
    // Emulator discovery
    void RefreshEmulatorInfo();
    void FindInstances(EmulatorInfo& info);

    // Actions
    void KillProcesses();
    void PatchHDPlayer(const std::string& installDir);
    void ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName);
    void RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName);
    void OneClickRoot(const std::string& dataDir, const std::string& instanceName);
    void OneClickUnroot(const std::string& dataDir, const std::string& instanceName);

    // Helpers
    std::string ReadRegistryString(const std::string& subKey, const std::string& valueName);
    std::string ReadFileString(const std::string& path);
    bool        WriteFileString(const std::string& path, const std::string& content);
    void        Log(const std::string& msg, bool isError = false);
    bool        IsMasterInstance(const std::string& instanceName);

    // State
    EmulatorInfo m_bluestacks;
    EmulatorInfo m_msi;
    int          m_selectedEmulator = 0; // 0=BlueStacks, 1=MSI
    std::string  m_selectedInstance;
    std::string  m_log;
    bool         m_scrollToBottom = false;
};
