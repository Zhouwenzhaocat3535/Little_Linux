#pragma once

/*
 * ramfs_file_hash.h - 文件信息哈希表（以 name 为键）
 * 依赖：strlen, memcmp, memset, malloc, free（用户已实现）
 */

#include <stddef.h>
#include "stdio.h"
#include "string.h"
#include "ramfs_types.h"
#include "malloc.h"

/* ========== 内部辅助函数（基于用户提供的库函数） ========== */

/* 安全字符串复制（限制长度） */
void _file_strcpy(char* dest, const char* src);

/* 字符串比较：相等返回1，不等返回0 */
int _file_streq(const char* a, const char* b);

/* 哈希函数（djb2） */
unsigned int _file_hash(const char* key);

/* ========== 对外 API ========== */

/* 初始化哈希表 */
void ramfs_file_hashmap_init(ramfs_file_hashmap* map);

/* 插入或更新键值对，成功返回1，内存不足返回0 */
int ramfs_file_hashmap_insert(ramfs_file_hashmap* map, const char* key, const ramfs_file_info* value);

/* 查找：返回指向值的指针，未找到返回 NULL */
ramfs_file_info* ramfs_file_hashmap_find(ramfs_file_hashmap* map, const char* key);

/* 删除键值对，成功返回1，失败返回0 */
int ramfs_file_hashmap_delete(ramfs_file_hashmap* map, const char* key);

/* 返回条目总数 */
int ramfs_file_hashmap_count(ramfs_file_hashmap* map);
/* 列出所有文件名（键），返回 char[n][128]，需调用者 free */
char* ramfs_file_hashmap_list(ramfs_file_hashmap* map);

/* 销毁哈希表：释放所有节点内存 */
void ramfs_file_hashmap_destroy(ramfs_file_hashmap* map);
