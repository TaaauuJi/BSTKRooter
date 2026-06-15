# BSTK Rooter

**BSTK Rooter** is a open-source C++ utility designed to configure and root **BlueStacks 5** and **MSI App Player** instances. By performing low-level offline modifications on virtual disk images, it grants you complete control over your Android environments for game analysis, reverse engineering, and debugging.

[![License](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)]()
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)]()

---

> ⚠️ WARNING & DISCLAIMER ⚠️
>
> **Use this tool at your own risk.** It modifies emulator configuration files and system files directly on your computer. The authors are not liable for corrupted emulator data, lost files, or system instabilities.

---

## 🚀 Features

- **Fix Illegally Tampered** — Patches `HD-Player.exe` to bypass integrity checks on versions 5.22.130+.
- **Disk R/W Conversion** — Easily set `Root.vhd` and `fastboot.vdi` to Normal read-write mode.
- **Disk R/O Revert** — Instantly lock VHD settings back to Readonly.
- **One-Click Root & Unroot** — Injects or removes the native `su` binary and updates `bluestacks.conf`.
- **Kitsune Magisk Integration** — Installs or uninstalls the Kitsune Magisk environment directly from the VHD's EXT4 filesystem.
- **Native Notifications** — Dispatches task completion and manual action alerts directly to the Windows Notification / Action Center.

---

## 🛠 How It Works

1. Injects the `su` binary or `Kitsune Magisk` into the virtual disk's EXT4 partition offline using the native [lwext4](https://github.com/gkostka/lwext4) parser.
2. Patches `HD-Player.exe` to disable disk tampering check loops.
3. Updates the emulator's `.bstk` metadata to mount virtual disk files in normal read-write mode.
4. Updates `bluestacks.conf` to configure standard root privileges.

---

## 📂 Project Structure

```
BSTKRooter/
├── src/              # Application source code
│   ├── main.cpp      # Entry point, D3D11/ImGui setup
│   ├── RootTool.cpp  # Core logic (patching, rooting, system notifications, UI)
│   ├── RootTool.h
│   ├── VHDManager.cpp # VHD/ext4 low-level operations
│   ├── VHDManager.h
│   ├── resources.rc   # Win32 resources (icons, manifest, embedded binaries)
│   ├── resources.h
│   └── app.manifest
├── res/              # System binaries, icons, and logo assets
├── imgui/            # Dear ImGui library
├── lwext4/           # lwext4 EXT4 parsing library
├── build.bat         # Build script
└── setup_dependencies.bat
```

---

## 📦 Getting Started

### Prerequisites

- **Windows 10 / 11**
- **Visual Studio 2022** (with Desktop development with C++ workload)
- **C++17**

### Building from Source

1. Run `setup_dependencies.bat` to download `ImGui` and `lwext4`.
2. Open the **x64 Native Tools Command Prompt for VS 2022**.
3. Run the build script:
   ```cmd
   .\build.bat
   ```
4. Find the output executable at `build/BstkRooter.exe`.

---

## 📖 Usage Guide

1. Launch `BstkRooter.exe` as Administrator.
2. Select your emulator (**BlueStacks 5** or **MSI App Player**).
3. Choose the target instance from the dropdown.

### Traditional Root

1. Click **"One Click Root"**.
2. Click **"Fix Illegally Tampered"**. (*Note: If "Illegally Tampered" error appears, click "Fix Illegally Tampered" button*)
3. Start your emulator to boot with root privileges.

### Unroot

1. Click **"One Click Unroot"**.

### Kitsune Magisk

- **Install:**
  1. Click **"Install Kitsune Magisk"**.
  2. The tool copies files offline to virtual disks and opens the emulator.
  3. A Windows Toast Notification will pop up prompting you to **manually install the Magisk manager APK** in the running emulator.
- **Uninstall:**
  1. Click **"Uninstall Kitsune Magisk"**.
  2. The tool closes the emulator, cleans all Magisk system directories offline, and restarts the emulator.
  3. A Windows Toast Notification will pop up prompting you to **manually uninstall the Magisk manager app**.

---

## 🤝 Credits & Third-Party Dependencies

BSTK Rooter is made possible by and gives full credit to the following excellent open-source projects:

- **[Dear ImGui](https://github.com/ocornut/imgui)**
- **[lwext4](https://github.com/gkostka/lwext4)**
- **[stb_image](https://github.com/nothings/stb)**
- **[Kitsune Magisk](https://web.archive.org/web/20250331162620/https://github.com/HuskyDG/magisk-files)**
- **[Magisk](https://github.com/topjohnwu/Magisk)**
- **[BlueStacks](https://www.bluestacks.com/)**

---

## 📄 License

GPLv3 License — see [LICENSE](LICENSE) for more details.
