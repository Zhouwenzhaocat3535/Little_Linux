#ifndef DFS_H
#define DFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ramfs.h"
#include "partition.h"
#include "ramfs_hash.h"
#include "ramfs_hash_file.h"

#ifndef INT32_T_DEFINED
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;
#define INT32_T_DEFINED
#endif

/* ---------- DFS 磁盘布局常量 ---------- */
#define DFS_MAGIC                   0x44465300      /* "DFS\0" */
#define DFS_BLOCK_SIZE              1024            /* 1KB 块大小 */
#define DFS_MAX_FOLDERS             4096            /* 最大文件夹数量（超级块中固定）*/
#define DFS_MAX_SUB_ENTRIES         128             /* 每个目录最多子项数（为磁盘占用考虑缩小）*/

/* ---------- 磁盘上的文件夹项（子文件夹/文件） ---------- */
typedef struct dfs_disk_foder_info {
    char name[128];
    int32_t drive_addr;             /* 子文件夹项区的偏移 / 文件数据起始块偏移 */
} dfs_disk_foder_info;

typedef struct dfs_disk_file_info {
    char name[128];
    int64_t length;
    int32_t drive_addr;             /* 文件数据起始块偏移（按块对齐）*/
} dfs_disk_file_info;

/* 磁盘上单个文件夹的完整描述（紧凑结构，无 padding，固定大小）*/
typedef struct dfs_disk_folder {
    char name[128];
    uint32_t used;                  /* 0=未使用, 1=有效 */
    uint32_t subfolder_cnt;
    uint32_t file_cnt;
    dfs_disk_foder_info subfolders[DFS_MAX_SUB_ENTRIES];
    dfs_disk_file_info files[DFS_MAX_SUB_ENTRIES];
} dfs_disk_folder;

/* 超级块（位于分区起始处，占用一个块） */
typedef struct dfs_superblock {
    uint32_t magic;
    uint32_t total_folders;         /* 预留文件夹总数（DFS_MAX_FOLDERS）*/
    uint32_t cur_folders;           /* 当前已使用的文件夹数 */
    uint32_t root_pointer;          /* 根目录所在文件夹项区的偏移（字节）*/
    uint32_t bitmap_start;          /* 位图起始偏移（字节）*/
    uint32_t data_start;            /* 文件数据区起始偏移（字节）*/
    uint32_t block_count;           /* 总可用数据块数（基于分区大小计算）*/
    uint8_t  reserved[508];
} dfs_superblock;

/* ---------- 内存扩展结构（增强 ramfs_foder，假设原有结构已添加 extra 字段） ---------- */
/* 注意：下面这些字段已直接加到 ramfs_foder 中，不再单独定义 Ex 结构 */
//#define RAMFS_FOLDER_TYPE_MEM      0   /* 纯内存（未关联磁盘）*/
//#define RAMFS_FOLDER_TYPE_DISK_CLEAN 1 /* 来自磁盘未修改 */
//#define RAMFS_FOLDER_TYPE_NEW      2   /* 新建未同步 */
//#define RAMFS_FOLDER_TYPE_DIRTY    3   /* 子项有修改 */
//已在ramfs.h中定义

/* 全局挂载点记录 */
typedef struct dfs_mount_point {
    char path[256];                 /* 挂载点绝对路径（在 ramfs 中的目录）*/
    int partition_drive;            /* 分区所在的 drive 索引 */
    int partition_index;            /* 分区号（来自 partition table）*/
    ramfs_foder *root_dir;          /* 内存中的根目录（type = DISK_CLEAN）*/
} dfs_mount_point;

#define MAX_MOUNTS 8
extern dfs_mount_point mount_table[MAX_MOUNTS];
extern int mount_count;

/* ---------- 公开 API ---------- */
int dfs_format(int drive, int part);                                   /* 格式化分区 */
int dfs_mount(const char *mountpoint, int drive, int part);           /* 挂载分区到 ramfs 路径 */
int dfs_umount(const char *mountpoint);                               /* 卸载并同步 */
int dfs_load(const char *mountpoint, const char *relative_path);      /* 预加载目录/文件到内存 */
void dfs_sync_all(void);                                              /* 紧急同步所有挂载点 */
int dfs_load_folder_children(ramfs_foder *dir);
/* 内部辅助：同步单个目录（递归） */
int dfs_sync_folder(ramfs_foder *folder);

/* 获得挂载点信息（供 ramfs 内部使用）*/
dfs_mount_point *dfs_find_mount_point(const char *abs_path);
ramfs_foder *dfs_get_mount_root(const char *abs_path);
int dfs_is_path_mounted(const char *abs_path);
int dfs_load_file_data(ramfs_file_info *finfo, dfs_mount_point *mp);
void dfs_init();
#endif
