#pragma once
/*
 * ramfs_hash.h - 文件夹信息哈希表（以 name 为键）
 * 不依赖任何标准库函数，裸金属专用
 * 依赖用户提供的 malloc/free
 */

#include <stddef.h>
#include "ramfs_types.h"
#include "stdio.h"
#include "string.h"
#include "ramfs_types.h"
#include "malloc.h"

/* ========== 对外 API ========== */

/* 初始化哈希表 */
void ramfs_hashmap_init(ramfs_hashmap* map);

/* 插入或更新键值对。成功返回1，内存不足返回0 */
int ramfs_hashmap_insert(ramfs_hashmap* map, const char* key, const ramfs_foder_info* value);
/* 查找：返回指向值的指针，未找到返回 NULL */
ramfs_foder_info* ramfs_hashmap_find(ramfs_hashmap* map, const char* key);

/* 删除键值对，成功返回1，失败（键不存在）返回0 */
int ramfs_hashmap_delete(ramfs_hashmap* map, const char* key);
/* 返回当前条目总数 */
int ramfs_hashmap_count(ramfs_hashmap* map);

/* 返回所有键（目录名）组成的二维数组 char[n][128]，调用者需 free 释放。
 * 若表空或分配失败返回 NULL。
 */
char* ramfs_hashmap_list(ramfs_hashmap* map);
/* 销毁哈希表：释放所有节点内存，重置计数器 */
void ramfs_hashmap_destroy(ramfs_hashmap* map);
