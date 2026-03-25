# BSTK Rooter

**BSTK Rooter** is a one-click root tool for **BlueStacks 5** and **MSI App Player**. It gives full control over emulator configurations for game analysis, development, and reverse engineering.

![License](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)

---

> **⚠️ WARNING & DISCLAIMER ⚠️**  
> Use this tool at your own risk. It modifies emulator files directly on your computer (system settings, filesystem images). The authors are not liable for corrupted emulator data, lost files, or system instabilities. Always run as Administrator.

---

## 🚀 Features

- **Fix Illegally Tampered** — Patches `HD-Player.exe` to bypass disk integrity checks on versions 5.22.130+.
- **Disk R/W Conversion** — Sets `Root.vhd` and `fastboot.vdi` to read-write mode.
- **Disk R/O (Revert)** — Reverts VHD settings back to Readonly.
- **One Click Root / Unroot** — Injects or removes the `su` binary and updates `bluestacks.conf`.

---

## 🛠 How It Works

1. Injects the `su` binary into the VHD's EXT4 filesystem using a native [lwext4](https://github.com/gkostka/lwext4) parser.
2. Patches `HD-Player.exe` to bypass disk integrity checks.
3. Updates `.bstk` configuration to mount modified disk images as read-write.
4. Updates `bluestacks.conf` to enable root access.

---

## 📂 Project Structure

```
root_tool/
├── src/              # Application source code
│   ├── main.cpp      # Entry point, D3D11/ImGui setup
│   ├── RootTool.cpp  # Core logic (patching, rooting, UI)
│   ├── RootTool.h
│   ├── VHDManager.cpp # VHD/ext4 filesystem operations
│   ├── VHDManager.h
│   ├── resources.rc   # Win32 resources (icon, manifest, embedded binaries)
│   ├── resources.h
│   └── app.manifest
├── res/              # Icons and embedded binaries
├── imgui/            # Dear ImGui (downloaded via setup_dependencies.bat)
├── lwext4/           # lwext4 library (downloaded via setup_dependencies.bat)
├── scripts/          # Build helper scripts
├── build.bat         # Build script
└── setup_dependencies.bat
```

### Prerequisites

- **Windows 10/11**
- **Visual Studio 2022** (Desktop development with C++ workload)
- **C++17**
- **Python 3** (for build-time resource encryption)

### Build

1. Run `setup_dependencies.bat` to download ImGui and lwext4.
2. Open **x64 Native Tools Command Prompt for VS 2022**.
3. Run `build.bat`.

Output: `build/BstkRooter.exe`

---

## 📖 Usage

1. **Launch** `BstkRooter.exe` as Administrator.
2. **Kill** any running emulator instances (button provided).
3. **Select** BlueStacks 5 or MSI App Player.
4. **Choose** the target instance from the dropdown.
5. **Root:**
   - Click **"Fix Illegally Tampered"**.
   - Click **"Disk R/W"**.
   - Click **"One Click Root"**.
6. **Launch** the emulator — it will boot with root access.

### Unroot

1. Click **"One Click Unroot"**.
2. Click **"Disk R/O (Revert)"**.
3. Restore `HD-Player.exe.bak` if needed.

---

## 📄 License

MIT License — see [LICENSE](LICENSE).
