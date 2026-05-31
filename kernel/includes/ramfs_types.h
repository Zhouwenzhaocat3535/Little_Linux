#pragma once

#include <stdbool.h>
#include "stdint.h"
#include <stddef.h>

/* 文件信息结构体 */
typedef struct ramfs_file_info {
    char name[128];      /* 文件名，同时也是键 */
    char type;           /* 文件类型 */
    long long length;    /* 文件大小 */
    void* ptr;           /* 指向文件数据的指针 */
    int drive_addr;      /* 驱动器地址 */
} ramfs_file_info;


#define RAMFS_FILE_BUCKET_SIZE 16

typedef struct ramfs_file_hash_node {
    char key[128];
    ramfs_file_info value;
    struct ramfs_file_hash_node* next;
} ramfs_file_hash_node;
typedef struct ramfs_file_hashmap {
    ramfs_file_hash_node* buckets[RAMFS_FILE_BUCKET_SIZE];
    int count;
} ramfs_file_hashmap;


/* 前向声明 ramfs_foder 类型（实际定义由用户提供） */
typedef struct ramfs_foder ramfs_foder;

/* 文件夹信息结构体 */
typedef struct ramfs_foder_info {
    char name[128];      /* 目录名 */
    char type;           /* 类型标记 */
    ramfs_foder* ptr;    /* 指向实际目录结构的指针 */
    int drive_addr;      /* 驱动器地址 */
} ramfs_foder_info;

/* 哈希表配置 */
#define RAMFS_HASH_BUCKET_SIZE 16

/* 哈希节点（动态分配） */
typedef struct ramfs_hash_node {
    char key[128];
    ramfs_foder_info value;
    struct ramfs_hash_node* next;
} ramfs_hash_node;

/* 哈希表主结构 */
typedef struct {
    ramfs_hash_node* buckets[RAMFS_HASH_BUCKET_SIZE];
    int count;
} ramfs_hashmap;


struct ramfs_foder
{
	char name[128];//名称
	char type;//暂时全部填写0
	ramfs_hashmap foder_map;//文件夹的哈希表
	ramfs_file_hashmap file_map;//文件的哈希表
	struct dfs_mount_point *mount;
	struct ramfs_foder *parent;
	long long drive_addr;
	bool is_mount_root;
};

/* 文件夹 type 字段取值 */
#define RAMFS_FOLDER_TYPE_MEM          0   /* 纯内存（无磁盘对应）*/
#define RAMFS_FOLDER_TYPE_DISK_CLEAN   1   /* 来自磁盘，未修改 */
#define RAMFS_FOLDER_TYPE_NEW          2   /* 新建未同步 */
#define RAMFS_FOLDER_TYPE_DIRTY        3   /* 子项有修改 */

#define RAMFS_TYPE_MEM      0
#define RAMFS_TYPE_NEW      1
#define RAMFS_TYPE_DIRTY    2
