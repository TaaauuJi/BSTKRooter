# BSTK Rooter

One-click root tool for **BlueStacks 5** and **MSI App Player**.

> **Run as Administrator** — the tool needs elevated privileges to modify emulator files.

---

## Usage

### 1. Kill Emulator Processes
Stops `HD-Player.exe`, `HD-MultiInstanceManager.exe`, and `BstkSVC.exe`.  
All other actions auto-kill the emulator before running, so this is only needed if you want to stop it manually.

### 2. Fix Illegally Tempered
Patches `HD-Player.exe` to bypass the disk integrity check.  
A `.bak` backup is created on first run. Works across all BlueStacks/MSI versions.

### 3. Disk R/W
Changes `Root.vhd` and `fastboot.vdi` from **Readonly → Normal** in the instance `.bstk` file, allowing the emulator to write to the root partition.

### 4. Disk R/O (Revert)
Reverses **Disk R/W** — sets the disks back to **Readonly**.

### 5. One Click Root
Installs the `su` binary directly into the VHD:
- Opens `Root.vhd`
- Mounts the ext4 partition
- Creates `/system/xbin/` directory
- Copies `su` with permissions `06755` (suid/sgid) and owner `root:root`
- Unmounts and closes

> **Master instances only** — works on base instances (e.g. `Pie64`, `Tiramisu64`), not clones (`Pie64_1`, `Pie64_2`, etc.).

### 6. One Click Unroot
Removes the `su` binary from the VHD to reverse rooting.  

> **Master instances only.**

---

## Typical Workflow

```
1. Launch BstkRooter.exe as Administrator
2. Select emulator (BlueStacks 5 / MSI App Player)
3. Select instance from dropdown
4. Click "Fix Illegally Tempered"
5. Click "Disk R/W"
6. Click "One Click Root"
7. Start the emulator — you now have root
```

To undo everything:
```
1. Click "One Click Unroot"
2. Click "Disk R/O (Revert)"
3. Restore HD-Player.exe.bak if needed
```
