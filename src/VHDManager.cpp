#include "VHDManager.h"
#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>
#pragma comment(lib, "virtdisk.lib")

extern "C" {
#include <ext4.h>
#include <ext4_blockdev.h>
}

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// MBR structures
#pragma pack(push, 1)
struct MBRPartitionEntry {
    uint8_t  status;
    uint8_t  first_chs[3];
    uint8_t  partition_type;
    uint8_t  last_chs[3];
    uint32_t lba_start;
    uint32_t sector_count;
};

struct MBR {
    uint8_t  bootloader[446];
    MBRPartitionEntry partitions[4];
    uint16_t signature;
};

struct VHDFooter {
    char cookie[8];
    uint32_t features;
    uint32_t file_format_version;
    uint64_t data_offset;
    uint32_t timestamp;
    char creator_app[4];
    uint32_t creator_version;
    uint32_t creator_host_os;
    uint64_t original_size;
    uint64_t current_size;
    uint32_t disk_geometry;
    uint32_t disk_type;
    uint32_t checksum;
    uint8_t  unique_id[16];
    uint8_t  saved_state;
    uint8_t  reserved[427];
};

struct VHDDynamicHeader {
    char cookie[8];         // "cxsparse"
    uint64_t data_offset;
    uint64_t table_offset;
    uint32_t header_version;
    uint32_t max_table_entries;
    uint32_t block_size;
    uint32_t checksum;
    uint8_t  parent_uuid[16];
    uint32_t parent_timestamp;
    uint32_t reserved1;
    uint8_t  parent_name[512];
    uint8_t  parent_locators[192];
    uint8_t  reserved2[256];
};
#pragma pack(pop)

static std::string MakeError(const char* msg, const char* detail) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s", msg, detail);
    return std::string(buf);
}

static std::string MakeError(const char* msg, int code) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%d", msg, code);
    return std::string(buf);
}

static std::string MakeError(const char* msg, unsigned long code) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%lu", msg, code);
    return std::string(buf);
}

// Dynamic VHD block reading support
struct DynamicVHDInfo {
    bool is_dynamic;
    uint32_t block_size;
    uint32_t max_table_entries;
    uint64_t table_offset;
    uint64_t virtual_size;
    uint32_t* bat;  // Block Allocation Table
};

static DynamicVHDInfo g_dynVHD = {};

// Allocate a new block in a dynamic VHD when writing to an unallocated region
static bool AllocateDynamicBlock(HANDLE hFile, uint32_t block_index) {
    if (block_index >= g_dynVHD.max_table_entries)
        return false;
    
    // Get current file size — VHD footer (512 bytes) is at the very end
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize))
        return false;
    
    // New block replaces the footer position; footer moves to new end
    uint64_t new_block_file_offset = fileSize.QuadPart - 512;
    uint32_t new_bat_sector = (uint32_t)(new_block_file_offset / 512);
    
    // Calculate sector bitmap size
    // Each bit in the bitmap represents one 512-byte sector
    uint32_t sectors_per_block = g_dynVHD.block_size / 512;
    uint32_t bitmap_bytes_raw = (sectors_per_block + 7) / 8;
    // Round up to 512-byte sector boundary
    uint32_t bitmap_padded = (bitmap_bytes_raw + 511) & ~511u;
    if (bitmap_padded == 0) bitmap_padded = 512;
    
    // Seek to new block position
    LARGE_INTEGER li;
    li.QuadPart = new_block_file_offset;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
        return false;
    
    // Write sector bitmap (all 0xFF = all sectors present)
    {
        std::vector<uint8_t> bitmap(bitmap_padded, 0xFF);
        DWORD written;
        if (!WriteFile(hFile, bitmap.data(), bitmap_padded, &written, NULL) || written != bitmap_padded)
            return false;
    }
    
    // Write zero-filled data block in 64KB chunks
    {
        const uint32_t chunk = 65536;
        std::vector<uint8_t> zeros(chunk, 0);
        uint32_t remaining = g_dynVHD.block_size;
        while (remaining > 0) {
            uint32_t to_write = (remaining < chunk) ? remaining : chunk;
            DWORD written;
            if (!WriteFile(hFile, zeros.data(), to_write, &written, NULL) || written != to_write)
                return false;
            remaining -= to_write;
        }
    }
    
    // Read the VHD footer copy from offset 0 (header mirror in dynamic VHD)
    uint8_t footer[512];
    li.QuadPart = 0;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
    DWORD bytesRead;
    if (!ReadFile(hFile, footer, 512, &bytesRead, NULL) || bytesRead != 512)
        return false;
    
    // Write footer at new end of file
    li.QuadPart = new_block_file_offset + bitmap_padded + g_dynVHD.block_size;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
        return false;
    DWORD written;
    if (!WriteFile(hFile, footer, 512, &written, NULL) || written != 512)
        return false;
    
    // Update BAT entry in memory
    g_dynVHD.bat[block_index] = new_bat_sector;
    
    // Update BAT entry on disk (big-endian format)
    uint32_t be_bat_entry = _byteswap_ulong(new_bat_sector);
    li.QuadPart = g_dynVHD.table_offset + ((uint64_t)block_index * sizeof(uint32_t));
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
    if (!WriteFile(hFile, &be_bat_entry, sizeof(be_bat_entry), &written, NULL))
        return false;
    
    FlushFileBuffers(hFile);
    return true;
}

VHDManager::VHDManager()
    : m_hVHD(INVALID_HANDLE_VALUE)
    , m_ext4_bdev(nullptr)
    , m_bdif(nullptr)
    , m_ext4_mounted(false)
    , m_mounted_partition_index(-1)
    , m_virtDiskHandle(NULL)
    , m_isVirtDiskAttached(false)
{
    memset(&m_block_device, 0, sizeof(m_block_device));
}

VHDManager::~VHDManager() {
    CloseVHD();
}

bool VHDManager::OpenVHD(const std::string& path) {
    if (IsOpen()) {
        CloseVHD();
    }
    
    m_vhd_path = path;
    
    // Check if it's a VHDX file, use Windows VirtDisk API natively!
    if (path.length() >= 5 && (path.substr(path.length() - 5) == ".vhdx" || path.substr(path.length() - 5) == ".VHDX")) {
        VIRTUAL_STORAGE_TYPE storageType;
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
        
        std::wstring wpath(path.begin(), path.end());
        HANDLE hVhd = NULL;
        DWORD res = OpenVirtualDisk(&storageType, wpath.c_str(), VIRTUAL_DISK_ACCESS_ALL, OPEN_VIRTUAL_DISK_FLAG_NONE, NULL, &hVhd);
        if (res != ERROR_SUCCESS) {
            SetError(MakeError("Failed to open VHDX via VirtDisk. Error: ", res));
            return false;
        }
        
        ATTACH_VIRTUAL_DISK_PARAMETERS attachParams = {};
        attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        res = AttachVirtualDisk(hVhd, NULL, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &attachParams, NULL);
        if (res != ERROR_SUCCESS) {
            CloseHandle(hVhd);
            SetError(MakeError("Failed to attach VHDX via VirtDisk. Error: ", res));
            return false;
        }
        
        WCHAR diskPath[MAX_PATH] = {0};
        ULONG diskPathSize = sizeof(diskPath);
        res = GetVirtualDiskPhysicalPath(hVhd, &diskPathSize, diskPath);
        if (res != ERROR_SUCCESS) {
            DetachVirtualDisk(hVhd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
            CloseHandle(hVhd);
            SetError(MakeError("Failed to get physical path for VHDX. Error: ", res));
            return false;
        }
        
        m_hVHD = CreateFileW(diskPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m_hVHD == INVALID_HANDLE_VALUE) {
            DWORD winErr = ::GetLastError();
            DetachVirtualDisk(hVhd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
            CloseHandle(hVhd);
            SetError(MakeError("Failed to open physical drive. Error: ", winErr));
            return false;
        }
        
        m_virtDiskHandle = hVhd;
        m_isVirtDiskAttached = true;
        g_dynVHD.is_dynamic = false; // Windows dynamically handles the sizing internally
        
        if (!ParsePartitions()) {
            CloseVHD();
            return false;
        }
        
        return true;
    }
    
    m_hVHD = CreateFileA(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    if (m_hVHD == INVALID_HANDLE_VALUE) {
        DWORD winErr = ::GetLastError();
        SetError(MakeError("Failed to open VHD file. Error: ", winErr));
        return false;
    }
    
    if (!ReadVHDFooter()) {
        CloseVHD();
        return false;
    }
    
    if (!ParsePartitions()) {
        CloseVHD();
        return false;
    }
    
    return true;
}

void VHDManager::CloseVHD() {
    if (IsExt4Mounted()) {
        UnmountExt4();
    }
    
    if (g_dynVHD.bat) {
        delete[] g_dynVHD.bat;
        g_dynVHD.bat = nullptr;
    }
    g_dynVHD.is_dynamic = false;
    
    if (m_hVHD != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hVHD);
        m_hVHD = INVALID_HANDLE_VALUE;
    }
    
    if (m_isVirtDiskAttached && m_virtDiskHandle != NULL) {
        DetachVirtualDisk(m_virtDiskHandle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(m_virtDiskHandle);
        m_virtDiskHandle = NULL;
        m_isVirtDiskAttached = false;
    }
    
    m_partitions.clear();
    m_vhd_path.clear();
}

bool VHDManager::ReadVHDFooter() {
    uint8_t buffer[512];
    DWORD bytesRead;
    
    // Try reading footer from beginning (copy header for dynamic VHD)
    SetFilePointer(m_hVHD, 0, NULL, FILE_BEGIN);
    if (!ReadFile(m_hVHD, buffer, 512, &bytesRead, NULL)) {
        SetError("Failed to read file");
        return false;
    }
    
    VHDFooter* footer = (VHDFooter*)buffer;
    
    bool found_footer = false;
    
    if (memcmp(footer->cookie, "conectix", 8) == 0) {
        found_footer = true;
        
        uint32_t disk_type = _byteswap_ulong(footer->disk_type);
        uint64_t data_offset = _byteswap_uint64(footer->data_offset);
        uint64_t current_size = _byteswap_uint64(footer->current_size);
        
        if (disk_type == 3) {
            // Dynamic VHD - need to read BAT
            g_dynVHD.is_dynamic = true;
            g_dynVHD.virtual_size = current_size;
            
            // Read dynamic header
            LARGE_INTEGER li;
            li.QuadPart = data_offset;
            SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
            
            uint8_t dynbuf[1024];
            ReadFile(m_hVHD, dynbuf, 1024, &bytesRead, NULL);
            
            VHDDynamicHeader* dynhdr = (VHDDynamicHeader*)dynbuf;
            
            if (memcmp(dynhdr->cookie, "cxsparse", 8) == 0) {
                g_dynVHD.table_offset = _byteswap_uint64(dynhdr->table_offset);
                g_dynVHD.max_table_entries = _byteswap_ulong(dynhdr->max_table_entries);
                g_dynVHD.block_size = _byteswap_ulong(dynhdr->block_size);
                
                // Read BAT
                g_dynVHD.bat = new uint32_t[g_dynVHD.max_table_entries];
                
                li.QuadPart = g_dynVHD.table_offset;
                SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
                
                DWORD bat_size = g_dynVHD.max_table_entries * sizeof(uint32_t);
                ReadFile(m_hVHD, g_dynVHD.bat, bat_size, &bytesRead, NULL);
                
                // Byte swap BAT entries
                for (uint32_t i = 0; i < g_dynVHD.max_table_entries; i++) {
                    g_dynVHD.bat[i] = _byteswap_ulong(g_dynVHD.bat[i]);
                }
            }
        }
    }
    
    if (!found_footer) {
        // Try at end of file
        LARGE_INTEGER fileSize;
        GetFileSizeEx(m_hVHD, &fileSize);
        
        LARGE_INTEGER li;
        li.QuadPart = fileSize.QuadPart - 512;
        SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, buffer, 512, &bytesRead, NULL);
        
        if (memcmp(buffer, "conectix", 8) == 0) {
            found_footer = true;
            // Fixed VHD - data starts right after header or at offset 0
        }
    }
    
    if (!found_footer) {
        // Not a VHD? Try as raw image
        SetError("No VHD footer found - treating as raw image");
        // Continue anyway - might be a raw disk image
    }
    
    return true;
}

static uint64_t GetDiskSize(HANDLE h) {
    GET_LENGTH_INFORMATION li = {0};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), &bytesReturned, NULL)) {
        return li.Length.QuadPart;
    }
    
    LARGE_INTEGER fileSize;
    if (GetFileSizeEx(h, &fileSize)) {
        return (uint64_t)fileSize.QuadPart;
    }
    return 0;
}

bool VHDManager::ParsePartitions() {
    m_partitions.clear();
    
    // For dynamic VHD, we need to read virtual sector 0
    uint8_t sector0[512];
    DWORD bytesRead;
    
    if (g_dynVHD.is_dynamic) {
        // Read virtual sector 0 through BAT
        // Sector 0 is in block 0
        uint32_t block_index = 0;
        uint32_t bat_entry = g_dynVHD.bat[block_index];
        
        if (bat_entry == 0xFFFFFFFF) {
            // Block not allocated - try raw ext4
            SetError("Block 0 not allocated in dynamic VHD");
            
            // Still add as raw ext4 partition covering entire virtual disk
            PartitionInfo info;
            info.type = 0x83;
            info.offset = 0;
            info.size = g_dynVHD.virtual_size;
            info.is_ext4 = true;
            m_partitions.push_back(info);
            return true;
        }
        
        // Read from physical offset
        // Each BAT entry points to a sector offset (in 512-byte sectors)
        // Add 1 sector for the bitmap
        uint64_t phys_offset = ((uint64_t)bat_entry) * 512;
        
        // Skip bitmap (bitmap size = block_size / (512 * 8) rounded up to sector)
        uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
        bitmap_sectors = (bitmap_sectors + 511) / 512;
        if (bitmap_sectors == 0) bitmap_sectors = 1;
        
        phys_offset += bitmap_sectors * 512;
        
        LARGE_INTEGER li;
        li.QuadPart = phys_offset;
        SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, sector0, 512, &bytesRead, NULL);
        
    } else {
        // Fixed VHD or raw - check if "conectix" header takes first 512 bytes
        SetFilePointer(m_hVHD, 0, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, sector0, 512, &bytesRead, NULL);
        
        // If first 512 bytes is VHD header, skip it
        if (memcmp(sector0, "conectix", 8) == 0) {
            // Fixed VHD: disk data starts at offset 512
            SetFilePointer(m_hVHD, 512, NULL, FILE_BEGIN);
            ReadFile(m_hVHD, sector0, 512, &bytesRead, NULL);
        }
    }
    
    MBR* mbr = (MBR*)sector0;
    
    // Check for MBR
    if (mbr->signature == 0xAA55) {
        for (int i = 0; i < 4; i++) {
            const MBRPartitionEntry& entry = mbr->partitions[i];
            if (entry.partition_type == 0) continue;
            
            PartitionInfo info;
            info.type = entry.partition_type;
            info.offset = (uint64_t)entry.lba_start * 512;
            info.size = (uint64_t)entry.sector_count * 512;
            info.is_ext4 = (entry.partition_type == 0x83);
            m_partitions.push_back(info);
        }
        
        if (!m_partitions.empty()) {
            return true;
        }
    }
    
    // Check for GPT
    uint8_t sector1[512];
    if (g_dynVHD.is_dynamic) {
        // Read virtual sector 1 - still in block 0 for most block sizes
        uint32_t bat_entry = g_dynVHD.bat[0];
        if (bat_entry != 0xFFFFFFFF) {
            uint64_t phys_offset = ((uint64_t)bat_entry) * 512;
            uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
            bitmap_sectors = (bitmap_sectors + 511) / 512;
            if (bitmap_sectors == 0) bitmap_sectors = 1;
            phys_offset += bitmap_sectors * 512 + 512; // +512 for sector 1
            
            LARGE_INTEGER li;
            li.QuadPart = phys_offset;
            SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
            ReadFile(m_hVHD, sector1, 512, &bytesRead, NULL);
        }
    } else {
        // Try sector 1 for GPT
        LARGE_INTEGER li;
        li.QuadPart = 512;
        SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
        ReadFile(m_hVHD, sector1, 512, &bytesRead, NULL);
    }
    
    if (memcmp(sector1, "EFI PART", 8) == 0) {
        // GPT found - parse GPT entries
        // For now, assume first partition starts at LBA 2048
        PartitionInfo info;
        info.type = 0x83;
        info.offset = 2048 * 512;
        info.size = g_dynVHD.is_dynamic ? g_dynVHD.virtual_size - info.offset : GetDiskSize(m_hVHD) - info.offset;
        info.is_ext4 = true;
        m_partitions.push_back(info);
        return true;
    }
    
    // No MBR or GPT - check if raw ext4
    // Check for ext4 superblock at offset 1024 from virtual disk start
    uint16_t ext4_magic = 0;
    
    if (g_dynVHD.is_dynamic) {
        uint32_t bat_entry = g_dynVHD.bat[0];
        if (bat_entry != 0xFFFFFFFF) {
            uint64_t phys_offset = ((uint64_t)bat_entry) * 512;
            uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
            bitmap_sectors = (bitmap_sectors + 511) / 512;
            if (bitmap_sectors == 0) bitmap_sectors = 1;
            phys_offset += bitmap_sectors * 512 + 1024 + 0x38;
            
            LARGE_INTEGER li;
            li.QuadPart = phys_offset;
            SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
            ReadFile(m_hVHD, &ext4_magic, 2, &bytesRead, NULL);
        }
    } else {
        // Try raw ext4 at various offsets
        uint64_t try_offsets[] = { 1024, 512 + 1024, 0 };
        
        for (int i = 0; try_offsets[i] != 0; i++) {
            LARGE_INTEGER li;
            li.QuadPart = try_offsets[i] + 0x38;
            SetFilePointerEx(m_hVHD, li, NULL, FILE_BEGIN);
            ReadFile(m_hVHD, &ext4_magic, 2, &bytesRead, NULL);
            
            if (ext4_magic == 0xEF53) {
                PartitionInfo info;
                info.type = 0x83;
                info.offset = (try_offsets[i] == 1024) ? 0 : 512;
                
                info.size = GetDiskSize(m_hVHD) - info.offset;
                if (!m_isVirtDiskAttached) info.size -= 512; // minus VHD footer
                info.is_ext4 = true;
                m_partitions.push_back(info);
                return true;
            }
        }
    }
    
    if (ext4_magic == 0xEF53) {
        PartitionInfo info;
        info.type = 0x83;
        info.offset = 0;
        info.size = g_dynVHD.is_dynamic ? g_dynVHD.virtual_size : GetDiskSize(m_hVHD);
        info.is_ext4 = true;
        m_partitions.push_back(info);
        return true;
    }
    
    SetError("No partition table or ext4 filesystem found");
    return false;
}

bool VHDManager::IsExt4Filesystem(uint64_t offset) {
    LARGE_INTEGER pos;
    pos.QuadPart = offset + 1024 + 0x38;
    
    SetFilePointerEx(m_hVHD, pos, NULL, FILE_BEGIN);
    
    uint16_t magic;
    DWORD bytesRead;
    
    if (!ReadFile(m_hVHD, &magic, sizeof(magic), &bytesRead, NULL)) {
        return false;
    }
    
    return (magic == 0xEF53);
}

// Block device - handles both fixed and dynamic VHD
int VHDManager::BlockOpen(struct ext4_blockdev* bdev) {
    return EOK;
}

int VHDManager::BlockClose(struct ext4_blockdev* bdev) {
    return EOK;
}

int VHDManager::BlockRead(struct ext4_blockdev* bdev, void* buf, 
                         uint64_t blk_id, uint32_t blk_cnt) {
    VHDBlockDevice* vhd = (VHDBlockDevice*)bdev->bdif->p_user;
    
    uint8_t* outbuf = (uint8_t*)buf;
    uint32_t sector_size = vhd->block_size;
    
    for (uint32_t i = 0; i < blk_cnt; i++) {
        uint64_t virtual_offset = vhd->partition_offset + ((blk_id + i) * sector_size);
        uint64_t phys_offset;
        
        if (g_dynVHD.is_dynamic) {
            // Translate virtual offset to physical offset
            uint32_t block_index = (uint32_t)(virtual_offset / g_dynVHD.block_size);
            uint32_t offset_in_block = (uint32_t)(virtual_offset % g_dynVHD.block_size);
            
            if (block_index >= g_dynVHD.max_table_entries) {
                return EIO;
            }
            
            uint32_t bat_entry = g_dynVHD.bat[block_index];
            
            if (bat_entry == 0xFFFFFFFF) {
                // Block not allocated - return zeros
                memset(outbuf + (i * sector_size), 0, sector_size);
                continue;
            }
            
            // Physical offset = BAT entry (in sectors) * 512 + bitmap + offset in block
            uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
            bitmap_sectors = (bitmap_sectors + 511) / 512;
            if (bitmap_sectors == 0) bitmap_sectors = 1;
            
            phys_offset = ((uint64_t)bat_entry) * 512 + (bitmap_sectors * 512) + offset_in_block;
            
        } else {
            phys_offset = virtual_offset;
        }
        
        LARGE_INTEGER li;
        li.QuadPart = phys_offset;
        
        if (!SetFilePointerEx(vhd->hFile, li, NULL, FILE_BEGIN)) {
            return EIO;
        }
        
        DWORD bytesRead;
        if (!ReadFile(vhd->hFile, outbuf + (i * sector_size), sector_size, &bytesRead, NULL)) {
            return EIO;
        }
        
        if (bytesRead != sector_size) {
            return EIO;
        }
    }
    
    return EOK;
}

int VHDManager::BlockWrite(struct ext4_blockdev* bdev, const void* buf,
                          uint64_t blk_id, uint32_t blk_cnt) {
    VHDBlockDevice* vhd = (VHDBlockDevice*)bdev->bdif->p_user;
    
    const uint8_t* inbuf = (const uint8_t*)buf;
    uint32_t sector_size = vhd->block_size;
    
    for (uint32_t i = 0; i < blk_cnt; i++) {
        uint64_t virtual_offset = vhd->partition_offset + ((blk_id + i) * sector_size);
        uint64_t phys_offset;
        
        if (g_dynVHD.is_dynamic) {
            uint32_t block_index = (uint32_t)(virtual_offset / g_dynVHD.block_size);
            uint32_t offset_in_block = (uint32_t)(virtual_offset % g_dynVHD.block_size);
            
            if (block_index >= g_dynVHD.max_table_entries) {
                return EIO;
            }
            
            uint32_t bat_entry = g_dynVHD.bat[block_index];
            
            if (bat_entry == 0xFFFFFFFF) {
                // Block not allocated — allocate it now
                if (!AllocateDynamicBlock(vhd->hFile, block_index)) {
                    return EIO;
                }
                bat_entry = g_dynVHD.bat[block_index];
            }
            
            uint32_t bitmap_sectors = (g_dynVHD.block_size / 512 + 7) / 8;
            bitmap_sectors = (bitmap_sectors + 511) / 512;
            if (bitmap_sectors == 0) bitmap_sectors = 1;
            
            phys_offset = ((uint64_t)bat_entry) * 512 + (bitmap_sectors * 512) + offset_in_block;
            
        } else {
            phys_offset = virtual_offset;
        }
        
        LARGE_INTEGER li;
        li.QuadPart = phys_offset;
        
        if (!SetFilePointerEx(vhd->hFile, li, NULL, FILE_BEGIN)) {
            return EIO;
        }
        
        DWORD bytesWritten;
        if (!WriteFile(vhd->hFile, inbuf + (i * sector_size), sector_size, &bytesWritten, NULL)) {
            return EIO;
        }
        
        if (bytesWritten != sector_size) {
            return EIO;
        }
    }
    
    FlushFileBuffers(vhd->hFile);
    return EOK;
}

bool VHDManager::MountExt4Partition(int partition_index) {
    if (!IsOpen()) {
        SetError("VHD not opened");
        return false;
    }
    
    if (partition_index < 0 || partition_index >= (int)m_partitions.size()) {
        SetError("Invalid partition index");
        return false;
    }
    
    const PartitionInfo& part = m_partitions[partition_index];
    
    if (!part.is_ext4) {
        SetError("Partition is not ext4");
        return false;
    }
    
    if (IsExt4Mounted()) {
        UnmountExt4();
    }
    
    m_block_device.hFile = m_hVHD;
    m_block_device.partition_offset = part.offset;
    m_block_device.partition_size = part.size;
    m_block_device.block_size = 512;
    
    m_ext4_bdev = new ext4_blockdev();
    memset(m_ext4_bdev, 0, sizeof(ext4_blockdev));
    
    m_bdif = new ext4_blockdev_iface();
    memset(m_bdif, 0, sizeof(ext4_blockdev_iface));
    
    m_bdif->open = BlockOpen;
    m_bdif->bread = BlockRead;
    m_bdif->bwrite = BlockWrite;
    m_bdif->close = BlockClose;
    m_bdif->lock = nullptr;
    m_bdif->unlock = nullptr;
    m_bdif->ph_bsize = 512;
    m_bdif->ph_bcnt = part.size / 512;
    m_bdif->ph_bbuf = new uint8_t[512];
    memset(m_bdif->ph_bbuf, 0, 512);
    m_bdif->ph_refctr = 0;
    m_bdif->bread_ctr = 0;
    m_bdif->bwrite_ctr = 0;
    m_bdif->p_user = &m_block_device;
    
    m_ext4_bdev->bdif = m_bdif;
    m_ext4_bdev->part_offset = 0;
    m_ext4_bdev->part_size = part.size;
    
    int rc = ext4_device_register(m_ext4_bdev, "vhd");
    if (rc != EOK) {
        delete[] m_bdif->ph_bbuf;
        delete m_bdif;
        delete m_ext4_bdev;
        m_bdif = nullptr;
        m_ext4_bdev = nullptr;
        SetError(MakeError("Failed to register ext4 device: ", rc));
        return false;
    }
    
    rc = ext4_mount("vhd", "/", false);
    if (rc != EOK) {
        ext4_device_unregister("vhd");
        delete[] m_bdif->ph_bbuf;
        delete m_bdif;
        delete m_ext4_bdev;
        m_bdif = nullptr;
        m_ext4_bdev = nullptr;
        SetError(MakeError("Failed to mount ext4: ", rc));
        return false;
    }
    
    m_ext4_mounted = true;
    m_mounted_partition_index = partition_index;
    
    return true;
}

void VHDManager::UnmountExt4() {
    if (!m_ext4_mounted) return;
    
    ext4_umount("/");
    ext4_device_unregister("vhd");
    
    delete m_ext4_bdev;
    m_ext4_bdev = nullptr;
    
    if (m_bdif) {
        if (m_bdif->ph_bbuf) {
            delete[] m_bdif->ph_bbuf;
            m_bdif->ph_bbuf = nullptr;
        }
        delete m_bdif;
        m_bdif = nullptr;
    }
    
    m_ext4_mounted = false;
    m_mounted_partition_index = -1;
}

bool VHDManager::FileExists(const std::string& path) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    ext4_file f;
    int rc = ext4_fopen(&f, path.c_str(), "rb");
    if (rc == EOK) {
        ext4_fclose(&f);
        return true;
    }
    return false;
}

bool VHDManager::BackupFile(const std::string& source, const std::string& backup) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    ext4_file src, dst;
    
    int rc = ext4_fopen(&src, source.c_str(), "rb");
    if (rc != EOK) {
        SetError(MakeError("Failed to open source: ", source.c_str()));
        return false;
    }
    
    rc = ext4_fopen(&dst, backup.c_str(), "wb");
    if (rc != EOK) {
        ext4_fclose(&src);
        SetError(MakeError("Failed to create backup: ", backup.c_str()));
        return false;
    }
    
    char buffer[4096];
    size_t bytes_read, bytes_written;
    
    while (true) {
        rc = ext4_fread(&src, buffer, sizeof(buffer), &bytes_read);
        if (rc != EOK || bytes_read == 0) break;
        
        rc = ext4_fwrite(&dst, buffer, bytes_read, &bytes_written);
        if (rc != EOK || bytes_written != bytes_read) {
            ext4_fclose(&src);
            ext4_fclose(&dst);
            SetError("Failed to write backup data");
            return false;
        }
    }
    
    ext4_fclose(&src);
    ext4_fclose(&dst);
    return true;
}

bool VHDManager::CopyFileFromHost(const std::string& host_path, const std::string& ext4_path) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    FILE* host_file = fopen(host_path.c_str(), "rb");
    if (!host_file) {
        SetError(MakeError("Failed to open host file: ", host_path.c_str()));
        return false;
    }
    
    ext4_file ext4_f;
    int rc = ext4_fopen(&ext4_f, ext4_path.c_str(), "wb");
    if (rc != EOK) {
        fclose(host_file);
        SetError(MakeError("Failed to create ext4 file: ", ext4_path.c_str()));
        return false;
    }
    
    char buffer[4096];
    size_t bytes_read, bytes_written;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), host_file)) > 0) {
        rc = ext4_fwrite(&ext4_f, buffer, bytes_read, &bytes_written);
        if (rc != EOK || bytes_written != bytes_read) {
            fclose(host_file);
            ext4_fclose(&ext4_f);
            SetError("Failed to write to ext4 file");
            return false;
        }
    }
    
    fclose(host_file);
    ext4_fclose(&ext4_f);
    return true;
}

bool VHDManager::CopyFileToHost(const std::string& ext4_path, const std::string& host_path) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    ext4_file ext4_f;
    int rc = ext4_fopen(&ext4_f, ext4_path.c_str(), "rb");
    if (rc != EOK) {
        SetError(MakeError("Failed to open ext4 file: ", ext4_path.c_str()));
        return false;
    }
    
    FILE* host_file = fopen(host_path.c_str(), "wb");
    if (!host_file) {
        ext4_fclose(&ext4_f);
        SetError(MakeError("Failed to create host file: ", host_path.c_str()));
        return false;
    }
    
    char buffer[4096];
    size_t bytes_read;
    
    while (true) {
        rc = ext4_fread(&ext4_f, buffer, sizeof(buffer), &bytes_read);
        if (rc != EOK || bytes_read == 0) break;
        
        size_t written = fwrite(buffer, 1, bytes_read, host_file);
        if (written != bytes_read) {
            fclose(host_file);
            ext4_fclose(&ext4_f);
            SetError("Failed to write to host file");
            return false;
        }
    }
    
    fclose(host_file);
    ext4_fclose(&ext4_f);
    return true;
}

bool VHDManager::DeleteFile(const std::string& path) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    int rc = ext4_fremove(path.c_str());
    if (rc != EOK) {
        SetError(MakeError("Failed to delete file: ", path.c_str()));
        return false;
    }
    return true;
}

bool VHDManager::ListDirectory(const std::string& path, std::vector<std::string>& entries) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    entries.clear();
    
    ext4_dir dir;
    int rc = ext4_dir_open(&dir, path.c_str());
    if (rc != EOK) {
        SetError(MakeError("Failed to open directory: ", path.c_str()));
        return false;
    }
    
    const ext4_direntry* de;
    while ((de = ext4_dir_entry_next(&dir)) != nullptr) {
        entries.push_back(std::string((char*)de->name, de->name_length));
    }
    
    ext4_dir_close(&dir);
    return true;
}

void VHDManager::SetError(const std::string& error) {
    m_last_error = error;
}

bool VHDManager::SetFilePermissions(const std::string& path, uint32_t mode) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    int rc = ext4_mode_set(path.c_str(), mode);
    if (rc != EOK) {
        SetError(MakeError("Failed to set permissions on: ", path.c_str()));
        return false;
    }
    return true;
}

bool VHDManager::SetFileOwner(const std::string& path, uint32_t uid, uint32_t gid) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    int rc = ext4_owner_set(path.c_str(), uid, gid);
    if (rc != EOK) {
        SetError(MakeError("Failed to set owner on: ", path.c_str()));
        return false;
    }
    return true;
}

bool VHDManager::MakeDirectory(const std::string& path) {
    if (!IsExt4Mounted()) {
        SetError("ext4 not mounted");
        return false;
    }
    
    int rc = ext4_dir_mk(path.c_str());
    if (rc != EOK) {
        SetError(MakeError("Failed to create directory: ", path.c_str()));
        return false;
    }
    return true;
}