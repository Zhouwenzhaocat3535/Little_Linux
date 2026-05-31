/*
 * ramfs_hash.c - 文件夹信息哈希表（以 name 为键）
 * 不依赖任何标准库函数，裸金属专用
 * 依赖用户提供的 malloc/free
 */

#include "ramfs_hash.h"

/* ========== 内部辅助函数（手工实现，无库依赖） ========== */

/* 字符串比较：相等返回 1，不等返回 0 */
static int _streq(const char* a, const char* b) {
    size_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return a[i] == b[i];
}

/* 字符串复制（限制最大127字符+'\0'） */
static void _strcpy(char* dest, const char* src) {
    size_t i = 0;
    while (src[i] && i < 127) {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
}

/* 哈希函数（djb2） */
static unsigned int _hash(const char* key) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++))
        h = ((h << 5) + h) + c;
    return (unsigned int)(h % RAMFS_HASH_BUCKET_SIZE);
}

/* ========== 对外 API ========== */

/* 初始化哈希表 */
void ramfs_hashmap_init(ramfs_hashmap* map) {
    int i;
    for (i = 0; i < RAMFS_HASH_BUCKET_SIZE; ++i)
        map->buckets[i] = NULL;
    map->count = 0;
}

/* 插入或更新键值对。成功返回1，内存不足返回0 */
int ramfs_hashmap_insert(ramfs_hashmap* map, const char* key, const ramfs_foder_info* value) {
    unsigned int idx;
    ramfs_hash_node* node;

    if (!map || !key || !value) return 0;

    idx = _hash(key);
    node = map->buckets[idx];

    /* 查找已有节点 */
    while (node) {
        if (_streq(node->key, key)) {
            node->value = *value;
            _strcpy(node->value.name, key);   /* 同步 name 字段 */
            return 1;
        }
        node = node->next;
    }

    /* 创建新节点 */
    node = (ramfs_hash_node*)malloc(sizeof(ramfs_hash_node));
    if (!node) return 0;

    _strcpy(node->key, key);
    node->value = *value;
    _strcpy(node->value.name, key);
    node->next = map->buckets[idx];
    map->buckets[idx] = node;
    map->count++;
    return 1;
}

/* 查找：返回指向值的指针，未找到返回 NULL */
ramfs_foder_info* ramfs_hashmap_find(ramfs_hashmap* map, const char* key) {
    unsigned int idx;
    ramfs_hash_node* node;

    if (!map || !key) return NULL;

    idx = _hash(key);
    node = map->buckets[idx];
    while (node) {
        if (_streq(node->key, key))
            return &node->value;
        node = node->next;
    }
    return NULL;
}

/* 删除键值对，成功返回1，失败（键不存在）返回0 */
int ramfs_hashmap_delete(ramfs_hashmap* map, const char* key) {
    unsigned int idx;
    ramfs_hash_node* node;
    ramfs_hash_node* prev;

    if (!map || !key) return 0;

    idx = _hash(key);
    node = map->buckets[idx];
    prev = NULL;
    while (node) {
        if (_streq(node->key, key)) {
            if (prev)
                prev->next = node->next;
            else
                map->buckets[idx] = node->next;
            free(node);
            map->count--;
            return 1;
        }
        prev = node;
        node = node->next;
    }
    return 0;
}

/* 返回当前条目总数 */
int ramfs_hashmap_count(ramfs_hashmap* map) {
    return map ? map->count : 0;
}

/* 返回所有键（目录名）组成的二维数组 char[n][128]，调用者需 free 释放。
 * 若表空或分配失败返回 NULL。
 */
char* ramfs_hashmap_list(ramfs_hashmap* map) {
    int i, pos;
    ramfs_hash_node* node;
    char (*result)[128];

    if (!map || map->count == 0) return NULL;

    result = (char(*)[128])malloc(map->count * 128);
    if (!result) return NULL;

    pos = 0;
    for (i = 0; i < RAMFS_HASH_BUCKET_SIZE; ++i) {
        node = map->buckets[i];
        while (node) {
            _strcpy(result[pos], node->key);
            pos++;
            node = node->next;
        }
    }
    return (char*)result;
}

/* 销毁哈希表：释放所有节点内存，重置计数器 */
void ramfs_hashmap_destroy(ramfs_hashmap* map) {
    int i;
    ramfs_hash_node* node;
    ramfs_hash_node* next;

    if (!map) return;
    for (i = 0; i < RAMFS_HASH_BUCKET_SIZE; ++i) {
        node = map->buckets[i];
        while (node) {
            next = node->next;
            free(node);
            node = next;
        }
        map->buckets[i] = NULL;
    }
    map->count = 0;
}
