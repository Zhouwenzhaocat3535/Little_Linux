/*
 * MBR Partition Management for Bare-Metal AHCI Driver
 * Depends on ahci_read/ahci_write and global drive table.
 * Provides partition read/write, partition table update with locking.
 */

#include <stdint.h>
#include <stddef.h>
#include "drive.h"

/* ---------- Constants ---------- */
#define SECTOR_SIZE             512
#define MBR_SIGNATURE_OFFSET    510
#define MBR_SIGNATURE           0xAA55
#define PARTITION_TABLE_OFFSET  446
#define PARTITION_ENTRY_SIZE    16
#define MAX_PARTS_PER_DRIVE     32      /* max partitions per disk (primary + logical) */
#define MAX_DRIVES              32

/* Partition type codes */
#define PART_TYPE_EXTENDED      0x05
#define PART_TYPE_EXTENDED_LBA  0x0F
#define PART_TYPE_EXTENDED_W95  0x85

/* ---------- Partition data structures ---------- */
struct partition_entry {
    uint64_t start_lba;         /* absolute LBA of partition start */
    uint64_t sector_count;      /* number of sectors in partition */
    uint8_t  type;              /* partition type code (for debugging) */
};

struct drive_partitions {
    int                     count;                      /* number of valid partitions */
    struct partition_entry  parts[MAX_PARTS_PER_DRIVE];
};

/* Global partition table: one entry per detected drive (0..drive_count-1) */
static struct drive_partitions drive_parts[MAX_DRIVES];

/* ---------- Simple spinlock (non-recursive) ---------- */
static volatile int partition_lock = 0;

static inline void lock_acquire(void) {
    while (__sync_lock_test_and_set(&partition_lock, 1))
        __asm__ volatile ("pause");
}

static inline void lock_release(void) {
    __sync_lock_release(&partition_lock);
}

/* ---------- Sector read/write helpers using ahci_read/ahci_write ---------- */
/* Read one whole sector (512 bytes) from drive into buffer */
static int read_sector(int drive, uint64_t lba, void *buffer) {
    uint8_t *buf = (uint8_t*)buffer;
    uint32_t addr = lba * SECTOR_SIZE;   /* byte address */
    /* Read first 256 bytes */
    void *ptr = ahci_read(drive, addr, 256);
    if (!ptr) return -1;
    for (int i = 0; i < 256; i++) buf[i] = ((uint8_t*)ptr)[i];
    /* Read second 256 bytes */
    ptr = ahci_read(drive, addr + 256, 256);
    if (!ptr) return -1;
    for (int i = 0; i < 256; i++) buf[256 + i] = ((uint8_t*)ptr)[i];
    return 0;
}
/* Write one whole sector from buffer to drive */
/*
static int write_sector(int drive, uint64_t lba, void *buffer) {
    uint8_t *buf = (uint8_t*)buffer;
    uint32_t addr = lba * SECTOR_SIZE;
    // Write first 256 bytes
    if (ahci_write(drive, addr, 256, buf) != 0) return -1;
    // Write second 256 bytes
    if (ahci_write(drive, addr + 256, 256, buf + 256) != 0) return -1;
    return 0;
}
*/
/* ---------- MBR parsing (primary partitions + extended chain) ---------- */
/* Parse one partition entry (16 bytes) */
static void parse_partition_entry(uint8_t *entry, struct partition_entry *out) {
    uint8_t type = entry[4];
    uint32_t start_lba32 = *(uint32_t*)(entry + 8);
    uint32_t sector_count32 = *(uint32_t*)(entry + 12);
    out->start_lba = start_lba32;
    out->sector_count = sector_count32;
    out->type = type;
}

/* Recursively parse extended partition (EBR chain) */
static int parse_ebr(int drive, uint64_t ext_start_lba, struct drive_partitions *dp) {
    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(4)));
    uint64_t current_lba = ext_start_lba;
    while (current_lba != 0) {
        if (read_sector(drive, current_lba, sector) != 0)
            return -1;

        /* Check signature at offset 510 (only needed for MBR, but EBR also has it) */
        uint16_t sig = *(uint16_t*)(sector + MBR_SIGNATURE_OFFSET);
        if (sig != MBR_SIGNATURE)
            break;   /* invalid chain */

        /* First entry: logical partition */
        struct partition_entry log_part;
        parse_partition_entry(sector + PARTITION_TABLE_OFFSET, &log_part);
        if (log_part.sector_count > 0 && dp->count < MAX_PARTS_PER_DRIVE) {
            /* Logical partition's start LBA is absolute (most standard) */
            dp->parts[dp->count].start_lba = log_part.start_lba;
            dp->parts[dp->count].sector_count = log_part.sector_count;
            dp->parts[dp->count].type = log_part.type;
            dp->count++;
        }

        /* Second entry: pointer to next EBR */
        struct partition_entry next_ebr;
        parse_partition_entry(sector + PARTITION_TABLE_OFFSET + PARTITION_ENTRY_SIZE, &next_ebr);
        current_lba = next_ebr.start_lba;   /* zero terminates */
    }
    return 0;
}

/* Parse MBR for a given drive and fill drive_parts[drive] */
static void parse_mbr(int drive) {
    uint8_t mbr[SECTOR_SIZE] __attribute__((aligned(4)));
    struct drive_partitions *dp = &drive_parts[drive];
    dp->count = 0;

    if (read_sector(drive, 0, mbr) != 0)
        return;   /* read error, no partitions */

    /* Check MBR signature */
    uint16_t sig = *(uint16_t*)(mbr + MBR_SIGNATURE_OFFSET);
    if (sig != MBR_SIGNATURE)
        return;   /* no valid MBR */

    /* Parse 4 primary partition entries */
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = mbr + PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE;
        struct partition_entry pe;
        parse_partition_entry(entry, &pe);

        if (pe.sector_count == 0)
            continue;

        /* Check for extended partition */
        if (pe.type == PART_TYPE_EXTENDED || pe.type == PART_TYPE_EXTENDED_LBA ||
            pe.type == PART_TYPE_EXTENDED_W95) {
            /* Recursively parse logical drives inside this extended partition */
            parse_ebr(drive, pe.start_lba, dp);
        } else {
            /* Primary partition */
            if (dp->count < MAX_PARTS_PER_DRIVE) {
                dp->parts[dp->count].start_lba = pe.start_lba;
                dp->parts[dp->count].sector_count = pe.sector_count;
                dp->parts[dp->count].type = pe.type;
                dp->count++;
            }
        }
    }
}

/* ---------- Public API ---------- */
/* Rebuild partition tables for all currently detected drives.
   Must be called after hotplug or system init. */
void update_partition_tables(void) {
    lock_acquire();

    /* Update drive list (detect new/removed drives) */
    ahci_table_update();

    /* Re-parse MBR for every drive present */
    for (int i = 0; i < drive_count; i++) {
        parse_mbr(i);
    }

    lock_release();
}

/* Read from a specific partition.
   drive   : drive index (0 .. drive_count-1)
   part    : partition number within that drive (0 .. n)
   offset  : byte offset from start of partition
   length  : number of bytes to read (max limited by buffer, but no internal limit)
   buffer  : destination buffer (must be at least 'length' bytes)
   returns 0 on success, -1 on error (invalid drive/partition/offset, read failure) */
int partition_read(int drive, int part, uint64_t offset, int length, void *buffer) {
    lock_acquire();

    /* Validate drive and partition range */
    if (drive < 0 || drive >= drive_count || part < 0 || part >= drive_parts[drive].count) {
        lock_release();
        return -1;
    }

    struct partition_entry *pe = &drive_parts[drive].parts[part];
    uint64_t part_start_byte = pe->start_lba * SECTOR_SIZE;
    
    /* Check offset and length bounds */
    if (offset + (uint64_t)length > pe->sector_count * SECTOR_SIZE) {
        lock_release();
        return -1;
    }

    uint64_t abs_addr = part_start_byte + offset;
    lock_release();

    /* Perform actual read using ahci_read (which handles cross-sector automatically) */
    /* Note: ahci_read returns pointer to internal buffer, we need to copy out */
    void *data = ahci_read(drive, abs_addr, length);
    if (!data) return -1;
    for (int i = 0; i < length; i++)
        ((uint8_t*)buffer)[i] = ((uint8_t*)data)[i];
    return 0;
}

/* Write to a specific partition.
   drive   : drive index
   part    : partition number within that drive
   offset  : byte offset from start of partition
   length  : number of bytes to write
   buffer  : source data
   returns 0 on success, -1 on error */
int partition_write(int drive, int part, uint64_t offset, int length, const void *buffer) {
    lock_acquire();

    if (drive < 0 || drive >= drive_count || part < 0 || part >= drive_parts[drive].count) {
        lock_release();
        return -1;
    }

    struct partition_entry *pe = &drive_parts[drive].parts[part];
    uint64_t part_start_byte = pe->start_lba * SECTOR_SIZE;
    
    if (offset + (uint64_t)length > pe->sector_count * SECTOR_SIZE) {
        lock_release();
        return -1;
    }

    uint64_t abs_addr = part_start_byte + offset;
    lock_release();

    return ahci_write(drive, abs_addr, length, (void*)buffer);
}

/* Utility: get number of partitions on a drive (valid after update_partition_tables) */
int get_partition_count(int drive) {
    lock_acquire();
    int cnt = (drive >= 0 && drive < drive_count) ? drive_parts[drive].count : 0;
    lock_release();
    return cnt;
}

/* Optional: initialize partition system (call once after ahci_init) */
void partition_init(void) {
    update_partition_tables();
}

uint64_t partition_get_sector_count(int drive, int part) {
    if (drive < 0 || drive >= drive_count || part < 0 || part >= drive_parts[drive].count)
        return 0;
    return drive_parts[drive].parts[part].sector_count;
}
