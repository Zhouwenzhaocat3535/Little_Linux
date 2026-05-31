#include "dfs.h"
#include "stdio.h"
#include "drive.h"
#include "partition.h"

extern uint64_t drive_size_bytes[];

int fs_lsblk_api(int argc, char** argv) {
    if (argc > 1 && (streq(argv[1], "--help") || streq(argv[1], "-h"))) {
        printf("lsblk: list block devices (drives and partitions)\n");
        return 0;
    }
    printf("NAME         SIZE TYPE MOUNTPOINT\n");
    for (int d = 0; d < drive_count; d++) {
        // 手动构造设备名 "sda", "sdb", ...
        char dev_name[16];
        dev_name[0] = 's';
        dev_name[1] = 'd';
        dev_name[2] = 'a' + d;
        dev_name[3] = '\0';
        printf("%-10s %6lluM disk\n", dev_name, drive_size_bytes[d] / (1024*1024));
        int part_cnt = get_partition_count(d);
        for (int p = 0; p < part_cnt; p++) {
            uint64_t sectors = partition_get_sector_count(d, p);
            uint64_t size_mb = sectors * 512 / (1024*1024);
            // 直接打印分区名，如 sda1, sda2 等
            printf("  `- %s%d %6lluM part\n", dev_name, p+1, size_mb);
        }
    }
    return 0;
}

static int parse_device(const char *dev, int *drive, int *part) {
    if (dev[0] != 's' || dev[1] != 'd') return -1;
    int drive_idx = dev[2] - 'a';
    if (drive_idx < 0 || drive_idx >= drive_count) return -1;
    const char *part_str = dev + 3;
    if (*part_str == '\0') {
        // 没有分区号，表示整个磁盘？我们只支持分区，所以返回错误
        return -1;
    }
    int part_idx = atoi((char*)part_str, 10) - 1;
    if (part_idx < 0 || part_idx >= get_partition_count(drive_idx)) return -1;
    *drive = drive_idx;
    *part = part_idx;
    return 0;
}

int fs_mount_api(int argc, char** argv) {
    if (argc != 3) {
        printf("mount: usage mount DEVICE MOUNTPOINT\n");
        printf("Example: mount sda1 /mnt\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("mount DEVICE MOUNTPOINT - mount a DFS partition\n");
        return 0;
    }
    int drive, part;
    if (parse_device(argv[1], &drive, &part) != 0) {
        printf("mount: invalid device name. Use sda1, sda2, etc.\n");
        return -1;
    }
    const char *mountpoint = argv[2];
    // 检查挂载点是否存在且为空（dfs_mount 内部会检查）
    int ret = dfs_mount(mountpoint, drive, part);
    if (ret == -1) printf("mount: mount point does not exist or is not a directory\n");
    else if (ret == -2) printf("mount: mount point is not empty\n");
    else if (ret == -3) printf("mount: failed to read superblock\n");
    else if (ret == -4) printf("mount: not a DFS filesystem (bad magic)\n");
    else if (ret == -5 || ret == -7) printf("mount: memory allocation failed\n");
    else if (ret == -6) printf("mount: failed to load root directory from disk\n");
    else printf("mounted %s on %s\n", argv[1], mountpoint);
    return ret == 0 ? 0 : -1;
}

int fs_umount_api(int argc, char** argv) {
    if (argc != 2) {
        printf("umount: usage umount MOUNTPOINT\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("umount MOUNTPOINT - unmount a DFS partition\n");
        return 0;
    }
    int ret = dfs_umount(argv[1]);
    if (ret == -1) printf("umount: no filesystem mounted on %s\n", argv[1]);
    else printf("unmounted %s\n", argv[1]);
    return ret == 0 ? 0 : -1;
}

int fs_mkfs_api(int argc, char** argv) {
    if (argc != 2) {
        printf("mkfs.dfs: usage mkfs.dfs DEVICE\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("mkfs.dfs DEVICE - format a partition with DFS\n");
        return 0;
    }
    int drive, part;
    if (parse_device(argv[1], &drive, &part) != 0) {
        printf("mkfs.dfs: invalid device name\n");
        return -1;
    }
    // 注意：格式化会破坏原有数据，需要用户确认。简单起见直接格式化。
    int ret = dfs_format(drive, part);
    if (ret == -1) printf("mkfs.dfs: partition too small\n");
    else if (ret == -2) printf("mkfs.dfs: failed to write superblock\n");
    else if (ret == -3) printf("mkfs.dfs: failed to write root directory\n");
    else printf("mkfs.dfs: formatted %s as DFS\n", argv[1]);
    return ret == 0 ? 0 : -1;
}

void dfs_init()
{
	register_command("lsblk", "List block devices", fs_lsblk_api);
	register_command("mount", "Mount a DFS partition", fs_mount_api);
	register_command("umount", "Unmount a DFS partition", fs_umount_api);
	register_command("mkfs.dfs", "Format a partition with DFS", fs_mkfs_api);
}
