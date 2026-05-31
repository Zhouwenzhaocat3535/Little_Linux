#include "dfs.h"
#include "ramfs_hash.h"
#include "ramfs_hash_file.h"
#include <stddef.h>
#include <stdint.h>
#include "partition.h"
#include "string.h"

#define _streq(a, b) (strcmp(a, b) == 0)

dfs_mount_point mount_table[MAX_MOUNTS];
int mount_count = 0;

/* 外部字符串函数声明 */
extern void *memset(void *s, int c, size_t n);
uint32_t dfs_alloc_folder_slot(dfs_mount_point *mp);
int dfs_sync_file(ramfs_file_info *finfo, dfs_mount_point *mp);
/* 假设 ramfs 提供全局根目录和路径解析函数 */
extern ramfs_foder root;   /* RAMFS 根目录 */
ramfs_foder *ramfs_get_dir_by_abs_path(const char *path);  /* 由 ramfs 提供，根据绝对路径返回目录指针 */

/* 分区读写辅助 */
static int dfs_read_partition(int drive, int part, uint32_t offset, void *buf, uint32_t size) {
    return partition_read(drive, part, offset, size, buf);
}
static int dfs_write_partition(int drive, int part, uint32_t offset, void *buf, uint32_t size) {
    return partition_write(drive, part, offset, size, buf);
}

/* 获取分区总字节数（通过扇区数计算）*/
static uint64_t dfs_get_partition_size(int drive, int part) {
    /* 你需要根据实际 partition_entry 的 sector_count 实现，假设存在函数 partition_get_sectors(drive,part) */
    extern uint64_t partition_get_sector_count(int drive, int part);
    return partition_get_sector_count(drive, part) * 512;
}

/* ========== 磁盘文件夹操作 ========== */
static int dfs_load_folder_from_disk(ramfs_foder *mem_folder, int drive, int part, uint32_t disk_offset) {
    dfs_disk_folder disk_f;
    if (dfs_read_partition(drive, part, disk_offset, &disk_f, sizeof(dfs_disk_folder)) != 0)
        return -1;
    if (!disk_f.used) return -2;

    strcpy(mem_folder->name, disk_f.name);
    mem_folder->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
    mem_folder->drive_addr = disk_offset;
    mem_folder->is_mount_root = 0;
    ramfs_hashmap_init(&mem_folder->foder_map);
    ramfs_file_hashmap_init(&mem_folder->file_map);

    /* 加载子文件夹元数据（不加载其内部子项）*/
    for (uint32_t i = 0; i < disk_f.subfolder_cnt; i++) {
        ramfs_foder_info info;
        strcpy(info.name, disk_f.subfolders[i].name);
        info.type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
        info.drive_addr = disk_f.subfolders[i].drive_addr;
        /* 创建内存文件夹节点，但内容暂时为空（懒加载）*/
        ramfs_foder *sub = (ramfs_foder*)malloc(sizeof(ramfs_foder));
        if (!sub) return -3;
        memset(sub, 0, sizeof(ramfs_foder));
        strcpy(sub->name, disk_f.subfolders[i].name);
        sub->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
        sub->drive_addr = disk_f.subfolders[i].drive_addr;
        sub->is_mount_root = 0;
        ramfs_hashmap_init(&sub->foder_map);
        ramfs_file_hashmap_init(&sub->file_map);
        info.ptr = sub;
        ramfs_hashmap_insert(&mem_folder->foder_map, info.name, &info);
    }
    /* 加载文件元数据（文件数据本身延迟加载）*/
    for (uint32_t i = 0; i < disk_f.file_cnt; i++) {
        ramfs_file_info finfo;
        strcpy(finfo.name, disk_f.files[i].name);
        finfo.type = 0;
        finfo.length = disk_f.files[i].length;
        finfo.drive_addr = disk_f.files[i].drive_addr;
        finfo.ptr = NULL;   /* 按需读入 */
        ramfs_file_hashmap_insert(&mem_folder->file_map, finfo.name, &finfo);
    }
    return 0;
}

static int dfs_save_folder_to_disk(ramfs_foder *mem_folder, int drive, int part) {
    dfs_disk_folder disk_f;
    memset(&disk_f, 0, sizeof(disk_f));
    strcpy(disk_f.name, mem_folder->name);
    disk_f.used = 1;
    disk_f.subfolder_cnt = 0;
    disk_f.file_cnt = 0;

    /* 收集子文件夹 */
    for (int i = 0; i < RAMFS_HASH_BUCKET_SIZE && disk_f.subfolder_cnt < DFS_MAX_SUB_ENTRIES; i++) {
        ramfs_hash_node *node = mem_folder->foder_map.buckets[i];
        while (node && disk_f.subfolder_cnt < DFS_MAX_SUB_ENTRIES) {
            ramfs_foder_info *info = (ramfs_foder_info*)&node->value;
            strcpy(disk_f.subfolders[disk_f.subfolder_cnt].name, info->name);
            disk_f.subfolders[disk_f.subfolder_cnt].drive_addr = info->drive_addr;
            disk_f.subfolder_cnt++;
            node = node->next;
        }
    }
    /* 收集文件 */
    for (int i = 0; i < RAMFS_FILE_BUCKET_SIZE && disk_f.file_cnt < DFS_MAX_SUB_ENTRIES; i++) {
        ramfs_file_hash_node *node = mem_folder->file_map.buckets[i];
        while (node && disk_f.file_cnt < DFS_MAX_SUB_ENTRIES) {
            ramfs_file_info *finfo = (ramfs_file_info*)&node->value;
            strcpy(disk_f.files[disk_f.file_cnt].name, finfo->name);
            disk_f.files[disk_f.file_cnt].length = finfo->length;
            disk_f.files[disk_f.file_cnt].drive_addr = finfo->drive_addr;
            disk_f.file_cnt++;
            node = node->next;
        }
    }
    return dfs_write_partition(drive, part, mem_folder->drive_addr, &disk_f, sizeof(dfs_disk_folder));
}

/* 递归同步（根据 type 决定是否写回）*/
int dfs_sync_folder(ramfs_foder *folder) {
    if (!folder) return 0;
    if (folder->type == RAMFS_FOLDER_TYPE_MEM) return 0;

    /* 查找挂载点（同上） */
    dfs_mount_point *mp = NULL;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].root_dir == folder || folder->is_mount_root) {
            mp = &mount_table[i];
            break;
        }
    }
    if (!mp) return -1;

    if (folder->type == RAMFS_FOLDER_TYPE_DIRTY || folder->type == RAMFS_FOLDER_TYPE_NEW) {
        /* 1. 如果是 NEW 文件夹且未分配槽位，先分配一个 */
        if (folder->type == RAMFS_FOLDER_TYPE_NEW && folder->drive_addr == 0) {
            uint32_t new_offset = dfs_alloc_folder_slot(mp);
            if (new_offset == 0) return -2;
            folder->drive_addr = new_offset;
        }

        /* 2. 递归同步子文件夹（子文件夹内部会处理自己的类型） */
        for (int i = 0; i < RAMFS_HASH_BUCKET_SIZE; i++) {
            ramfs_hash_node *node = folder->foder_map.buckets[i];
            while (node) {
                ramfs_foder_info *info = (ramfs_foder_info*)&node->value;
                ramfs_foder *sub = (ramfs_foder*)info->ptr;
                if (sub->type == RAMFS_FOLDER_TYPE_NEW || sub->type == RAMFS_FOLDER_TYPE_DIRTY)
                    dfs_sync_folder(sub);
                node = node->next;
            }
        }

        /* 3. 同步文件 */
        for (int i = 0; i < RAMFS_FILE_BUCKET_SIZE; i++) {
            ramfs_file_hash_node *node = folder->file_map.buckets[i];
            while (node) {
                ramfs_file_info *finfo = &node->value;
                if (finfo->type == RAMFS_TYPE_NEW || finfo->type == RAMFS_TYPE_DIRTY)
                    dfs_sync_file(finfo, mp);
                node = node->next;
            }
        }

        /* 4. 写回自身 */
        if (dfs_save_folder_to_disk(folder, mp->partition_drive, mp->partition_index) == 0)
            folder->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
    }
    return 0;
}
/* ========== 挂载实现 ========== */
static int is_directory_empty(ramfs_foder *dir) {
    if (!dir) return 0;
    if (dir->foder_map.count != 0) return 0;
    if (dir->file_map.count != 0) return 0;
    return 1;
}

int dfs_mount(const char *mountpoint, int drive, int part) {
    /* 1. 检查挂载点路径是否存在且为空目录 */
    ramfs_foder *mount_parent = ramfs_get_dir_by_abs_path(mountpoint);
    if (!mount_parent) return -1;
    if (!is_directory_empty(mount_parent)) return -2;

    /* 2. 读取超级块，验证 */
    dfs_superblock sb;
    if (dfs_read_partition(drive, part, 0, &sb, sizeof(sb)) != 0) return -3;
    if (sb.magic != DFS_MAGIC) return -4;

    /* 3. 创建内存根目录节点 */
    ramfs_foder *root_dir = (ramfs_foder*)malloc(sizeof(ramfs_foder));
    if (!root_dir) return -5;
    memset(root_dir, 0, sizeof(ramfs_foder));
    if (dfs_load_folder_from_disk(root_dir, drive, part, sb.root_pointer) != 0) {
        free(root_dir);
        return -6;
    }
    root_dir->is_mount_root = 1;
	root_dir->mount = &mount_table[mount_count];
    
    /* 4. 将 root_dir 作为子目录插入到 mount_parent 的哈希表中 */
    ramfs_foder_info info;
    /* 提取挂载点最后一级名称 */
    const char *last = mountpoint;
    for (const char *p = mountpoint; *p; p++) if (*p == '/') last = p+1;
    strcpy(info.name, last);
    info.type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
    info.drive_addr = sb.root_pointer;
    info.ptr = root_dir;
    if (!ramfs_hashmap_insert(&mount_parent->foder_map, info.name, &info)) {
        free(root_dir);
        return -7;
    }

    /* 5. 记录挂载表 */
    strcpy(mount_table[mount_count].path, mountpoint);
    mount_table[mount_count].partition_drive = drive;
    mount_table[mount_count].partition_index = part;
    mount_table[mount_count].root_dir = root_dir;
    mount_count++;
    return 0;
}

void dfs_free_folder_tree(ramfs_foder *f) {
        if (!f) return;
        /* 释放子文件夹 */
        for (int i = 0; i < RAMFS_HASH_BUCKET_SIZE; i++) {
            ramfs_hash_node *node = f->foder_map.buckets[i];
            while (node) {
                ramfs_foder *sub = (ramfs_foder*)((ramfs_foder_info*)&node->value)->ptr;
                dfs_free_folder_tree(sub);
                node = node->next;
            }
        }
        /* 释放文件数据 */
        for (int i = 0; i < RAMFS_FILE_BUCKET_SIZE; i++) {
            ramfs_file_hash_node *node = f->file_map.buckets[i];
            while (node) {
                if (node->value.ptr) free(node->value.ptr);
                node = node->next;
            }
        }
        ramfs_hashmap_destroy(&f->foder_map);
        ramfs_file_hashmap_destroy(&f->file_map);
        free(f);
}
    

/* 卸载：同步 + 从父目录哈希表中移除 + 释放内存树 */
int dfs_umount(const char *mountpoint) {
    dfs_mount_point *mp = NULL;
    int idx = -1;
    for (int i = 0; i < mount_count; i++) {
        if (_streq(mount_table[i].path, mountpoint)) {
            mp = &mount_table[i];
            idx = i;
            break;
        }
    }
    if (!mp) return -1;

    /* 同步整个挂载树 */
    dfs_sync_folder(mp->root_dir);

    /* 从父目录哈希表中移除挂载点节点 */
    /* 需要获得父目录：根据 mp->path 找到父目录 */
    char parent_path[256];
    strcpy(parent_path, mp->path);
    char *last_slash = parent_path;
    char *p = parent_path;
    while (*p) { if (*p == '/') last_slash = p; p++; }
    if (last_slash != parent_path) *last_slash = '\0';
    else strcpy(parent_path, "/");
    ramfs_foder *parent_dir = ramfs_get_dir_by_abs_path(parent_path);
    if (parent_dir) {
        const char *name = last_slash + 1;
        ramfs_hashmap_delete(&parent_dir->foder_map, name);
    }

    /* 释放内存树（递归释放子目录和文件的数据）*/
    /* 这里需要实现递归释放函数，简单起见调用一个辅助函数 */
    dfs_free_folder_tree(mp->root_dir);

    /* 从挂载表中删除 */
    for (int i = idx; i < mount_count-1; i++)
        mount_table[i] = mount_table[i+1];
    mount_count--;
    return 0;
}

/* load：递归加载目录下所有文件的数据到内存 */
static int dfs_load_folder_recursive(ramfs_foder *folder, int drive, int part) {
    if (!folder) return 0;
    /* 确保目录的哈希表已加载（懒加载时可能还没子项？但这里假设已有元数据）*/
    for (int i = 0; i < RAMFS_FILE_BUCKET_SIZE; i++) {
        ramfs_file_hash_node *node = folder->file_map.buckets[i];
        while (node) {
            ramfs_file_info *finfo = &node->value;
            if (finfo->ptr == NULL && finfo->length > 0) {
                void *data = malloc(finfo->length);
                if (data) {
                    if (dfs_read_partition(drive, part, finfo->drive_addr, data, finfo->length) == 0)
                        finfo->ptr = data;
                    else
                        free(data);
                }
            }
            node = node->next;
        }
    }
    /* 递归子文件夹 */
    for (int i = 0; i < RAMFS_HASH_BUCKET_SIZE; i++) {
        ramfs_hash_node *node = folder->foder_map.buckets[i];
        while (node) {
            ramfs_foder *sub = (ramfs_foder*)((ramfs_foder_info*)&node->value)->ptr;
            dfs_load_folder_recursive(sub, drive, part);
            node = node->next;
        }
    }
    return 0;
}

int dfs_load(const char *mountpoint, const char *relative_path) {
    /* 找到挂载点 */
    dfs_mount_point *mp = NULL;
    for (int i = 0; i < mount_count; i++) {
        if (_streq(mount_table[i].path, mountpoint)) {
            mp = &mount_table[i];
            break;
        }
    }
    if (!mp) return -1;

    /* 根据相对路径在挂载点内找到目标目录 */
    char full_abs[512];
    strcpy(full_abs, mountpoint);
    if (!_streq(mountpoint, "/")) strcpy(full_abs + strlen(full_abs), "/");
    strcpy(full_abs + strlen(full_abs), relative_path);
    ramfs_foder *target = ramfs_get_dir_by_abs_path(full_abs);
    if (!target) return -2;

    return dfs_load_folder_recursive(target, mp->partition_drive, mp->partition_index);
}

/* 格式化分区（简化，只创建超级块和空根目录）*/
int dfs_format(int drive, int part) {
    uint64_t part_size = dfs_get_partition_size(drive, part);
    uint32_t block_count = part_size / DFS_BLOCK_SIZE;
    if (block_count < 1024) return -1; /* 太小 */

    dfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = DFS_MAGIC;
    sb.total_folders = DFS_MAX_FOLDERS;
    sb.cur_folders = 1;
    sb.root_pointer = sizeof(dfs_superblock);  /* 根目录紧接超级块 */
    sb.bitmap_start = sb.root_pointer + DFS_MAX_FOLDERS * sizeof(dfs_disk_folder);
    sb.data_start = ((sb.bitmap_start + (block_count + 7) / 8) + 511) & ~511; /* 按512对齐 */
    sb.block_count = block_count - (sb.data_start / DFS_BLOCK_SIZE);

    /* 写入超级块 */
    if (dfs_write_partition(drive, part, 0, &sb, sizeof(sb)) != 0) return -2;

    /* 初始化根目录磁盘结构 */
    dfs_disk_folder root_disk;
    memset(&root_disk, 0, sizeof(root_disk));
    strcpy(root_disk.name, "/");
    root_disk.used = 1;
    root_disk.subfolder_cnt = 0;
    root_disk.file_cnt = 0;
    if (dfs_write_partition(drive, part, sb.root_pointer, &root_disk, sizeof(root_disk)) != 0) return -3;

    /* 清空其他文件夹项（可选）*/
    /* 清空位图区（全0）*/
    uint32_t bitmap_bytes = (block_count + 7) / 8;
    uint8_t *zero = (uint8_t*)malloc(bitmap_bytes);
    if (zero) {
        memset(zero, 0, bitmap_bytes);
        dfs_write_partition(drive, part, sb.bitmap_start, zero, bitmap_bytes);
        free(zero);
    }
    return 0;
}

void dfs_sync_all(void) {
    for (int i = 0; i < mount_count; i++) {
        dfs_sync_folder(mount_table[i].root_dir);
    }
}

/* 辅助函数：根据绝对路径查找挂载点（供 ramfs 内部使用）*/
dfs_mount_point *dfs_find_mount_point(const char *abs_path) {
    dfs_mount_point *best = NULL;
    int best_len = 0;
    for (int i = 0; i < mount_count; i++) {
        const char *mp = mount_table[i].path;
        int len = strlen(mp);
        if (strlen(abs_path) >= len && (len == 1 || abs_path[len] == '/' || abs_path[len] == '\0')) {
            if (memcmp(abs_path, mp, len) == 0 && len > best_len) {
                best_len = len;
                best = &mount_table[i];
            }
        }
    }
    return best;
}
int dfs_load_file_data(ramfs_file_info *finfo, dfs_mount_point *mp) {
    if (!finfo || !mp) return -1;
    /* 如果已经加载或文件长度为0，无需操作 */
    if (finfo->ptr != NULL || finfo->length == 0)
        return 0;
    /* 分配内存并从分区读取 */
    void *data = malloc(finfo->length);
    if (!data) return -2;
    if (partition_read(mp->partition_drive, mp->partition_index,
                       finfo->drive_addr, finfo->length, data) != 0) {
        free(data);
        return -3;
    }
    finfo->ptr = data;
    return 0;
}

/*
 * 从磁盘加载指定目录的直接子项（子文件夹和文件元数据）。
 * 参数：
 *   dir：要加载的内存目录节点（必须已存在，且 type == DISK_CLEAN）
 * 返回值：
 *   0：成功
 *   -1：参数无效或目录类型错误
 *   -2：无法确定所属挂载点
 *   -3：读取磁盘失败
 *   -4：磁盘上的目录项标记为未使用
 *   -5：内存分配失败
 */
int dfs_load_folder_children(ramfs_foder *dir) {
    if (!dir || dir->type != RAMFS_FOLDER_TYPE_DISK_CLEAN)
        return -1;

    /* 如果哈希表已经非空，说明已经加载过，直接返回成功 */
    if (dir->foder_map.count != 0 || dir->file_map.count != 0)
        return 0;

    /* 获取所属挂载点 */
    dfs_mount_point *mp = dir->mount;
    if (!mp)
        return -2;

    /* 读取磁盘上的目录结构 */
    dfs_disk_folder disk_f;
    if (dfs_read_partition(mp->partition_drive, mp->partition_index,
                           dir->drive_addr, &disk_f, sizeof(dfs_disk_folder)) != 0)
        return -3;
    if (!disk_f.used)
        return -4;

    /* ----- 加载子文件夹 ----- */
    for (uint32_t i = 0; i < disk_f.subfolder_cnt; i++) {
        const char *sub_name = disk_f.subfolders[i].name;
        /* 避免重复插入（理论上不应存在，但安全起见检查） */
        if (ramfs_hashmap_find(&dir->foder_map, sub_name))
            continue;

        /* 创建新的内存文件夹节点 */
        ramfs_foder *sub = (ramfs_foder*)malloc(sizeof(ramfs_foder));
        if (!sub)
            return -5;
        memset(sub, 0, sizeof(ramfs_foder));
        strcpy(sub->name, sub_name);
        sub->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
        sub->drive_addr = disk_f.subfolders[i].drive_addr;
        sub->is_mount_root = 0;
        sub->mount = mp;              /* 继承挂载点 */
        sub->parent = dir;            /* 设置父目录指针 */
        ramfs_hashmap_init(&sub->foder_map);
        ramfs_file_hashmap_init(&sub->file_map);

        /* 插入父目录的哈希表 */
        ramfs_foder_info info;
        strcpy(info.name, sub_name);
        info.type = sub->type;
        info.drive_addr = sub->drive_addr;
        info.ptr = sub;
        ramfs_hashmap_insert(&dir->foder_map, info.name, &info);
    }

    /* ----- 加载文件元数据 ----- */
    for (uint32_t i = 0; i < disk_f.file_cnt; i++) {
        const char *file_name = disk_f.files[i].name;
        if (ramfs_file_hashmap_find(&dir->file_map, file_name))
            continue;

        ramfs_file_info finfo;
        memset(&finfo, 0, sizeof(finfo));
        strcpy(finfo.name, file_name);
        finfo.type = 0;
        finfo.length = disk_f.files[i].length;
        finfo.drive_addr = disk_f.files[i].drive_addr;
        finfo.ptr = NULL;   /* 文件数据延迟加载 */
        ramfs_file_hashmap_insert(&dir->file_map, finfo.name, &finfo);
    }

    return 0;
}


/* 补全部分：dfs_block_alloc, dfs_free_file_blocks, dfs_sync_file 及相关辅助函数 */
/* 需添加到 dfs.c 中，并确保常量、结构体定义完整 */

/* 读取超级块（分区内偏移 0） */
static int dfs_read_superblock(dfs_mount_point *mp, dfs_superblock *sb) {
    return partition_read(mp->partition_drive, mp->partition_index, 0, sizeof(dfs_superblock), sb);
}

/* 读取位图的一个字节（按位图偏移） */
static int read_bitmap_byte(dfs_mount_point *mp, uint32_t byte_offset, uint8_t *byte) {
    return partition_read(mp->partition_drive, mp->partition_index,
                          byte_offset, 1, byte);
}

/* 写回位图的一个字节 */
static int write_bitmap_byte(dfs_mount_point *mp, uint32_t byte_offset, uint8_t byte) {
    return partition_write(mp->partition_drive, mp->partition_index,
                           byte_offset, 1, &byte);
}

/* 分配连续数据块（返回起始字节偏移，失败返回0） */
static uint32_t dfs_alloc_file_blocks(dfs_mount_point *mp, uint64_t size) {
    dfs_superblock sb;
    if (dfs_read_superblock(mp, &sb) != 0)
        return 0;

    uint64_t blocks_needed = (size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    uint64_t total_blocks = sb.block_count;

    if (blocks_needed == 0 || blocks_needed > total_blocks)
        return 0;

    /* 位图起始字节偏移 */
    uint32_t bitmap_base = sb.bitmap_start;
    //uint32_t bitmap_bytes = (total_blocks + 7) / 8;
    //uint8_t buf[512];   /* 临时缓冲区，一次读一个扇区 */

    /* 逐字节扫描位图，寻找连续空闲块 */
    uint64_t start_block = 0;
    uint64_t consecutive = 0;
    for (uint64_t i = 0; i < total_blocks; i++) {
        uint32_t byte_off = bitmap_base + i / 8;
        uint8_t bit = 1 << (i % 8);

        /* 读取所在字节（缓存优化：尽量少读） */
        static uint32_t last_byte_off = 0;
        static uint8_t byte_val = 0;
        if (byte_off != last_byte_off) {
            if (read_bitmap_byte(mp, byte_off, &byte_val) != 0)
                return 0;
            last_byte_off = byte_off;
        }

        if ((byte_val & bit) == 0) {
            /* 空闲 */
            if (consecutive == 0)
                start_block = i;
            consecutive++;
            if (consecutive == blocks_needed)
                break;
        } else {
            consecutive = 0;
        }
    }

    if (consecutive < blocks_needed)
        return 0;   /* 空间不足 */

    /* 标记这些块为已占用 */
    for (uint64_t i = 0; i < blocks_needed; i++) {
        uint64_t block = start_block + i;
        uint32_t byte_off = bitmap_base + block / 8;
        uint8_t bit = 1 << (block % 8);
        uint8_t byte_val;
        if (read_bitmap_byte(mp, byte_off, &byte_val) != 0)
            return 0;
        byte_val |= bit;
        if (write_bitmap_byte(mp, byte_off, byte_val) != 0)
            return 0;
    }

    /* 返回数据区起始字节偏移 */
    return sb.data_start + start_block * DFS_BLOCK_SIZE;
}

/* 释放文件原有的磁盘块 */
static void dfs_free_file_blocks(dfs_mount_point *mp, uint32_t start_addr, uint64_t size) {
    dfs_superblock sb;
    if (dfs_read_superblock(mp, &sb) != 0)
        return;

    if (start_addr < sb.data_start)
        return;

    uint64_t block_idx = (start_addr - sb.data_start) / DFS_BLOCK_SIZE;
    uint64_t blocks = (size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;

    uint32_t bitmap_base = sb.bitmap_start;

    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t block = block_idx + i;
        if (block >= sb.block_count) break;
        uint32_t byte_off = bitmap_base + block / 8;
        uint8_t bit = 1 << (block % 8);
        uint8_t byte_val;
        if (read_bitmap_byte(mp, byte_off, &byte_val) != 0)
            continue;
        byte_val &= ~bit;
        write_bitmap_byte(mp, byte_off, byte_val);
    }
}

/* 同步单个文件：将内存中的文件数据写回磁盘 */
int dfs_sync_file(ramfs_file_info *finfo, dfs_mount_point *mp) {
    if (!finfo || !mp) return -1;

    /* 文件长度为0或已经是CLEAN，无需同步 */
    if (finfo->length == 0 || finfo->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        return 0;

    uint32_t new_addr = finfo->drive_addr;

    /* 如果是DIRTY且已有磁盘块，先释放旧块 */
    if (finfo->type == RAMFS_TYPE_DIRTY && finfo->drive_addr != 0) {
        dfs_free_file_blocks(mp, finfo->drive_addr, finfo->length);
        new_addr = 0;   /* 强制重新分配 */
    }

    /* 分配新磁盘块（如果是NEW或旧块已释放） */
    if (new_addr == 0) {
        new_addr = dfs_alloc_file_blocks(mp, finfo->length);
        if (new_addr == 0)
            return -2;  /* 空间不足 */
        finfo->drive_addr = new_addr;
    }

    /* 将内存数据写入磁盘 */
    if (partition_write(mp->partition_drive, mp->partition_index,
                        new_addr, finfo->length, finfo->ptr) != 0)
        return -3;

    finfo->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
    return 0;
}

uint32_t dfs_alloc_folder_slot(dfs_mount_point *mp) {
    if (!mp) return 0;
    
    /* 读取超级块以获取根目录偏移和总文件夹数 */
    dfs_superblock sb;
    if (dfs_read_superblock(mp, &sb) != 0) return 0;
    
    uint32_t folder_start = sb.root_pointer;
    uint32_t folder_end = sb.bitmap_start;  /* 位图起始也是文件夹区结束 */
    uint32_t folder_size = sizeof(dfs_disk_folder);
    
    /* 扫描每个文件夹槽位 */
    for (uint32_t offset = folder_start; offset + folder_size <= folder_end; offset += folder_size) {
        dfs_disk_folder disk_f;
        if (partition_read(mp->partition_drive, mp->partition_index, offset, folder_size, &disk_f) != 0)
            continue;  /* 读取失败，跳过（理论上不应发生） */
        
        if (!disk_f.used) {
            /* 找到空闲槽位，返回偏移 */
            return offset;
        }
    }
    return 0;  /* 无空闲槽位 */
}
