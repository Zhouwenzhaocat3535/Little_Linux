/*
 * Minimal AHCI driver for x86, no OS dependency.
 * Fixed IDENTIFY PRDT, port start FR wait, structure zeroing.
 * Supports byte-level read/write (max 256 bytes).
 */
#include "drive.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* PCI configuration space access */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA     0xCFC

static inline u32 pci_read_config_dword(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 addr = (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    __asm__ volatile ("outl %0, %1" : : "a"(addr), "d"(PCI_CONFIG_ADDRESS));
    u32 data;
    __asm__ volatile ("inl %1, %0" : "=a"(data) : "d"(PCI_CONFIG_DATA));
    return data;
}

static inline void pci_write_config_dword(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    u32 addr = (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC);
    __asm__ volatile ("outl %0, %1" : : "a"(addr), "d"(PCI_CONFIG_ADDRESS));
    __asm__ volatile ("outl %0, %1" : : "a"(value), "d"(PCI_CONFIG_DATA));
}

/* MMIO access */
//#define mmio_read32(addr)     (*(volatile u32*)(addr))
//#define mmio_write32(addr, v) (*(volatile u32*)(addr) = (v))

#define mmio_read32(addr)     (*(volatile u32*)(unsigned long)(addr))
#define mmio_write32(addr, v) (*(volatile u32*)(unsigned long)(addr) = (v))

/* AHCI register offsets (HBA) */
#define HBA_CAP       0x00
#define HBA_GHC       0x04
#define HBA_PI        0x0C

/* Port offsets (0x80 each) */
#define PxCLB         0x00
#define PxCLBU        0x04
#define PxFB          0x08
#define PxFBU         0x0C
#define PxIS          0x10
#define PxIE          0x14
#define PxCMD         0x18
#define PxTFD         0x20
#define PxSIG         0x24
#define PxSSTS        0x28
#define PxCI          0x30

/* GHC bits */
#define AHCI_ENABLE   (1u << 31)
#define HBA_RESET     (1u << 0)

/* PxCMD bits */
#define PxCMD_ST      (1u << 0)
#define PxCMD_FRE     (1u << 4)
#define PxCMD_CR      (1u << 15)
#define PxCMD_FR      (1u << 14)

/* PxSSTS DET mask */
#define SSTS_DET_MASK 0x0F
#define DET_PRESENT   0x03

/* FIS type */
#define FIS_TYPE_REG_H2D 0x27

/* ATA commands */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define SECTOR_SIZE   512
#define MAX_PORTS     32
#define MAX_DRIVES    MAX_PORTS

/* Command header (16 bytes) */
typedef struct {
    u32 cfl:5;
    u32 a:1;
    u32 w:1;
    u32 p:1;
    u32 r:1;
    u32 b:1;
    u32 c:1;
    u32 rsvd:1;
    u32 pmp:4;
    u32 prdtl:16;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
} __attribute__((packed)) cmd_hdr_t;

/* Physical Region Descriptor (16 bytes) */
typedef struct {
    u32 dba;
    u32 dbau;
    u32 rsvd:16;
    u32 dbc:16;
    u32 i:1;
    u32 rsvd2:31;
} __attribute__((packed)) prd_t;

/* Command table (aligned 128) */
typedef struct {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsvd[48];
    prd_t prdt[1];
} __attribute__((packed, aligned(128))) cmd_tbl_t;

/* Global data */
static u8 __attribute__((aligned(4096))) cmd_list_area[MAX_PORTS][4096];
static u8 __attribute__((aligned(4096))) fis_area[MAX_PORTS][4096];
static cmd_tbl_t __attribute__((aligned(128))) cmd_table_area[MAX_PORTS];

int drive_port[MAX_DRIVES];
u64 drive_size_bytes[MAX_DRIVES];
int drive_count = 0;

static uint64_t ahci_hba_base = 0;
static u8 byte_buffer[256];

static int ahci_initialized = 0;

/* Helper: busy-wait loops (crude but works) */
static void wait_cycles(u32 count) {
    volatile u32 i;
    for (i = 0; i < count; ++i) __asm__ volatile ("pause");
}

/* Convert virtual (linear) to physical (identity mapping assumed) */
static inline u32 virt_to_phys(void *virt) {
    return (u32)virt;
}

/* Stop port command engine */
static void port_stop(int port) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    u32 cmd = mmio_read32(base + PxCMD);
    cmd &= ~(PxCMD_ST | PxCMD_FRE);
    mmio_write32(base + PxCMD, cmd);
    while ((mmio_read32(base + PxCMD) & (PxCMD_FR | PxCMD_CR)) != 0)
        wait_cycles(100);
}

/* Start port: FRE then wait FR, then ST */
static void port_start(int port) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    u32 cmd = mmio_read32(base + PxCMD);
    cmd |= PxCMD_FRE;
    mmio_write32(base + PxCMD, cmd);
    /* Wait for FR to be set */
    while (!(mmio_read32(base + PxCMD) & PxCMD_FR))
        wait_cycles(10);
    cmd |= PxCMD_ST;
    mmio_write32(base + PxCMD, cmd);
    /* Wait for ST to be set (optional) */
    while (!(mmio_read32(base + PxCMD) & PxCMD_ST))
        wait_cycles(10);
}

/* Initialize a port: allocate command list, FIS receive, command table */
static void port_init(int port) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    port_stop(port);

    /* Zero out command list and command table */
    u32 *cl = (u32*)cmd_list_area[port];
    for (int i = 0; i < 4096 / 4; ++i) cl[i] = 0;
    cmd_tbl_t *tbl = &cmd_table_area[port];
    u32 *tbl_words = (u32*)tbl;
    for (int i = 0; i < sizeof(cmd_tbl_t) / 4; ++i) tbl_words[i] = 0;

    mmio_write32(base + PxCLB, virt_to_phys(cmd_list_area[port]));
    mmio_write32(base + PxCLBU, 0);
    mmio_write32(base + PxFB, virt_to_phys(fis_area[port]));
    mmio_write32(base + PxFBU, 0);
    mmio_write32(base + PxIS, 0xFFFFFFFF);
    mmio_write32(base + PxIE, 0);   /* no interrupts */

    port_start(port);
}

/* Wait for command slot 0 completion (timeout 1e6 loops) */
static int wait_cmd_complete(int port, int timeout_us) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    volatile u32 *ci = (volatile u32*)(base + PxCI);
    while (timeout_us--) {
        if ((*ci & (1u << 0)) == 0) {
            u32 is = mmio_read32(base + PxIS);
            if (is & (1u << 30))  /* TF error */
                return -1;
            return 0;
        }
        wait_cycles(10);
    }
    return -1;
}

/* Execute ATA command on port (slot 0). data_buf may be NULL if no data. */
static int exec_ata_cmd(int port, u8 cmd, u64 lba, u16 sector_count,
                        u8 *data_buf, int is_write) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    cmd_hdr_t *hdr = (cmd_hdr_t*)cmd_list_area[port];
    cmd_tbl_t *tbl = &cmd_table_area[port];

    /* Fill command header */
    hdr[0].cfl = 5;                 /* 5 DWORDs = 20 bytes */
    hdr[0].w = is_write ? 1 : 0;
    hdr[0].prdtl = (data_buf != NULL) ? 1 : 0;
    hdr[0].ctba = virt_to_phys(tbl);
    hdr[0].ctbau = 0;
    hdr[0].prdbc = 0;

    /* Setup PRDT if we have a data buffer */
    if (data_buf) {
        u32 bytes = (sector_count == 0) ? 512 : sector_count * SECTOR_SIZE;
        tbl->prdt[0].dba = virt_to_phys(data_buf);
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc = bytes - 1;
        tbl->prdt[0].i = 0;
    }

    /* Build Register H2D FIS */
    u8 *cfis = tbl->cfis;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;                     /* command, update flags */
    cfis[2] = cmd;
    cfis[3] = 0;                        /* features low */
    cfis[4] = (u8)lba;
    cfis[5] = (u8)(lba >> 8);
    cfis[6] = (u8)(lba >> 16);
    cfis[7] = (u8)(lba >> 24);
    cfis[8] = (u8)((lba >> 32) & 0xFF);
    cfis[9] = (u8)((lba >> 40) & 0xFF);
    cfis[10] = (u8)sector_count;
    cfis[11] = (u8)(sector_count >> 8);
    cfis[12] = 0;                       /* features high */
    cfis[13] = 0;                       /* aux */
    cfis[14] = 0x40;                    /* device: LBA, 48-bit */
    cfis[15] = 0;                       /* control */

    /* Start command */
    mmio_write32(base + PxCI, 1u << 0);
    return wait_cmd_complete(port, 1000000);
}

/* Read 1 sector via DMA */
static int port_read_sectors(int port, u64 lba, u16 sector_count, u8 *buffer) {
    if (sector_count == 0) return 0;
    if (sector_count > 1) return -1;   /* simplicity: 1 sector per call */
    return exec_ata_cmd(port, ATA_CMD_READ_DMA_EXT, lba, sector_count, buffer, 0);
}

/* Write 1 sector via DMA */
static int port_write_sectors(int port, u64 lba, u16 sector_count, u8 *buffer) {
    if (sector_count == 0) return 0;
    if (sector_count > 1) return -1;
    return exec_ata_cmd(port, ATA_CMD_WRITE_DMA_EXT, lba, sector_count, buffer, 1);
}

/* Identify device: get total size in bytes */
static int identify_device(int port, u64 *size_bytes) {
    u8 identify_buf[SECTOR_SIZE] __attribute__((aligned(2)));
    /* IDENTIFY requires 512-byte data transfer, sector_count=0 triggers PRDT bytes=512 */
    int ret = exec_ata_cmd(port, ATA_CMD_IDENTIFY, 0, 0, identify_buf, 0);
    if (ret != 0) return -1;

    u16 *buf16 = (u16*)identify_buf;
    u32 low  = buf16[100] | (buf16[101] << 16);
    u32 high = buf16[102] | (buf16[103] << 16);
    u64 sectors = ((u64)high << 32) | low;
    *size_bytes = sectors * SECTOR_SIZE;
    return 0;
}

/* Check if device present on port */
static int port_device_present(int port) {
    u32 base = ahci_hba_base + 0x100 + port * 0x80;
    u32 ssts = mmio_read32(base + PxSSTS);
    return (ssts & SSTS_DET_MASK) == DET_PRESENT;
}

static int add_drive_to_table(int port, u64 size) {
    if (drive_count >= MAX_DRIVES) return -1;
    drive_port[drive_count] = port;
    drive_size_bytes[drive_count] = size;
    return drive_count++;
}

static void remove_drive_from_table(int port) {
    for (int i = 0; i < drive_count; ++i) {
        if (drive_port[i] == port) {
            drive_port[i] = drive_port[drive_count-1];
            drive_size_bytes[i] = drive_size_bytes[drive_count-1];
            drive_count--;
            break;
        }
    }
}

static void port_scan_update(int port) {
    int already = 0;
    for (int i = 0; i < drive_count; ++i)
        if (drive_port[i] == port) { already = 1; break; }

    int present = port_device_present(port);
    if (present && !already) {
        port_init(port);
        u64 size = 0;
        if (identify_device(port, &size) == 0)
            add_drive_to_table(port, size);
    } else if (!present && already) {
        port_stop(port);
        remove_drive_from_table(port);
    }
}

/* --------------------------------------------------------------
 *  Exported API
 * -------------------------------------------------------------- */
void ahci_init(void) {
    bool found = 0;
    /* Find AHCI controller: class=0x01, subclass=0x06, prog_if=0x01, function 0 */
    for (u8 bus = 0; bus < 256; ++bus) {
        for (u8 dev = 0; dev < 32; ++dev) {
            u32 class_rev = pci_read_config_dword(bus, dev, 0, 0x08);
            u8 prog_if = (class_rev >> 8) & 0xFF;
            u8 subclass = (class_rev >> 16) & 0xFF;
            u8 baseclass = (class_rev >> 24) & 0xFF;
            if (baseclass == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                uint64_t bar = pci_read_config_dword(bus, dev, 0, 0x24);
				bar &= ~0xF;   /* 只清除低 4 位标志 */

				/* 检测是否为 64 位 BAR */
				if (bar & 0x4) {
					uint32_t bar_high = pci_read_config_dword(bus, dev, 0, 0x28);
					bar |= ((uint64_t)bar_high << 32);
				}
				ahci_hba_base = bar;
				/* Enable bus mastering + MMIO */
                u32 cmd = pci_read_config_dword(bus, dev, 0, 0x04);
                cmd |= (1 << 2) | (1 << 1);
                pci_write_config_dword(bus, dev, 0, 0x04, cmd);
                found = 1;
                break;
            }
        }
        if (found) break;
    }

    if (!found) return;
    
    /* Reset HBA */
    u32 ghc = mmio_read32(ahci_hba_base + HBA_GHC);
    ghc |= HBA_RESET;
    mmio_write32(ahci_hba_base + HBA_GHC, ghc);
    wait_cycles(10000);
    while (mmio_read32(ahci_hba_base + HBA_GHC) & HBA_RESET)
        wait_cycles(100);
    mmio_write32(ahci_hba_base + HBA_GHC, AHCI_ENABLE);

    u32 ports_impl = mmio_read32(ahci_hba_base + HBA_PI);
    for (int port = 0; port < MAX_PORTS; ++port) {
        if (ports_impl & (1u << port))
            port_scan_update(port);
    }
    ahci_initialized = 1;
    return;
}

void ahci_table_update(void) {
	if (!ahci_initialized) return;
    if (!ahci_hba_base) return;
    u32 ports_impl = mmio_read32(ahci_hba_base + HBA_PI);
    for (int port = 0; port < MAX_PORTS; ++port) {
        if (ports_impl & (1u << port))
            port_scan_update(port);
    }
}

void* ahci_read(int drive_number, uint64_t addr, uint64_t length) {
    if (!ahci_initialized) return(NULL);
    if (drive_number < 0 || drive_number >= drive_count) return 0;
    if (length <= 0 || length > 256) return 0;

    int port = drive_port[drive_number];
    u64 start_sector = addr / SECTOR_SIZE;
    u32 offset = addr % SECTOR_SIZE;
    u64 end_addr = (u64)addr + length - 1;
    u64 end_sector = end_addr / SECTOR_SIZE;
    u16 sectors = (u16)(end_sector - start_sector + 1);

    if (sectors > 2) return 0;   /* length <=256 => at most 2 sectors */

    u8 sector_buf[SECTOR_SIZE * 2] __attribute__((aligned(4)));
    u8 *out = byte_buffer;
    u32 copied = 0;

    for (u16 i = 0; i < sectors; ++i) {
        u64 cur_lba = start_sector + i;
        if (port_read_sectors(port, cur_lba, 1, sector_buf + i * SECTOR_SIZE) != 0)
            return 0;
        u32 start_off = (i == 0) ? offset : 0;
        u32 end_off = (i == sectors - 1) ? ((addr + length - 1) % SECTOR_SIZE + 1) : SECTOR_SIZE;
        u32 chunk = end_off - start_off;
        if (chunk > length - copied) chunk = length - copied;
        for (u32 j = 0; j < chunk; ++j)
            out[copied + j] = sector_buf[i * SECTOR_SIZE + start_off + j];
        copied += chunk;
    }
    return out;
}

int ahci_write(int drive_number, uint64_t addr, uint64_t length, void *data) {
    if (!ahci_initialized) return(-1);
    if (drive_number < 0 || drive_number >= drive_count) return -1;
    if (length <= 0 || length > 256 || !data) return -1;

    int port = drive_port[drive_number];
    u64 start_sector = addr / SECTOR_SIZE;
    u32 offset = addr % SECTOR_SIZE;
    u64 end_addr = (u64)addr + length - 1;
    u64 end_sector = end_addr / SECTOR_SIZE;
    u16 sectors = (u16)(end_sector - start_sector + 1);

    if (sectors > 2) return -1;

    u8 sector_buf[SECTOR_SIZE * 2] __attribute__((aligned(4)));
    u8 *src = (u8*)data;

    for (u16 i = 0; i < sectors; ++i) {
        u64 cur_lba = start_sector + i;
        u32 start_off = (i == 0) ? offset : 0;
        u32 end_off = (i == sectors - 1) ? ((addr + length - 1) % SECTOR_SIZE + 1) : SECTOR_SIZE;
        u32 chunk = end_off - start_off;

        /* Read original sector if we are not overwriting the whole sector */
        if (start_off != 0 || end_off != SECTOR_SIZE) {
            if (port_read_sectors(port, cur_lba, 1, sector_buf + i * SECTOR_SIZE) != 0)
                return -1;
        }
        /* Merge new data */
        for (u32 j = 0; j < chunk; ++j)
            sector_buf[i * SECTOR_SIZE + start_off + j] = src[j];
        /* Write back */
        if (port_write_sectors(port, cur_lba, 1, sector_buf + i * SECTOR_SIZE) != 0)
            return -1;
        src += chunk;
    }
    return 0;
}
