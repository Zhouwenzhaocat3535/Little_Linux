/*
 * ramfs_file_hash.c - 文件信息哈希表（以 name 为键）
 * 依赖：strlen, memcmp, memset, malloc, free（用户已实现）
 */

#include "ramfs_hash_file.h"

/* ========== 内部辅助函数（基于用户提供的库函数） ========== */

/* 安全字符串复制（限制长度） */
void _file_strcpy(char* dest, const char* src) {
    size_t i = 0;
    while (src[i] && i < 127) {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
}

/* 字符串比较：相等返回1，不等返回0 */
int _file_streq(const char* a, const char* b) {
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) return 0;
    return memcmp(a, b, len_a) == 0;
}

/* 哈希函数（djb2） */
unsigned int _file_hash(const char* key) {
    unsigned long hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c;
    return (unsigned int)(hash % RAMFS_FILE_BUCKET_SIZE);
}

/* ========== 对外 API ========== */

/* 初始化哈希表 */
void ramfs_file_hashmap_init(ramfs_file_hashmap* map) {
    int i;
    if (!map) return;
    for (i = 0; i < RAMFS_FILE_BUCKET_SIZE; ++i)
        map->buckets[i] = NULL;
    map->count = 0;
}

/* 插入或更新键值对，成功返回1，内存不足返回0 */
int ramfs_file_hashmap_insert(ramfs_file_hashmap* map, const char* key, const ramfs_file_info* value) {
    unsigned int idx;
    ramfs_file_hash_node* node;

    if (!map || !key || !value) return 0;

    idx = _file_hash(key);
    node = map->buckets[idx];

    while (node) {
        if (_file_streq(node->key, key)) {
            node->value = *value;
            _file_strcpy(node->value.name, key);
            return 1;
        }
        node = node->next;
    }

    node = (ramfs_file_hash_node*)malloc(sizeof(ramfs_file_hash_node));
    if (!node) return 0;

    _file_strcpy(node->key, key);
    node->value = *value;
    _file_strcpy(node->value.name, key);
    node->next = map->buckets[idx];
    map->buckets[idx] = node;
    map->count++;
    return 1;
}

/* 查找：返回指向值的指针，未找到返回 NULL */
ramfs_file_info* ramfs_file_hashmap_find(ramfs_file_hashmap* map, const char* key) {
    unsigned int idx;
    ramfs_file_hash_node* node;

    if (!map || !key) return NULL;

    idx = _file_hash(key);
    node = map->buckets[idx];
    while (node) {
        if (_file_streq(node->key, key))
            return &node->value;
        node = node->next;
    }
    return NULL;
}

/* 删除键值对，成功返回1，失败返回0 */
int ramfs_file_hashmap_delete(ramfs_file_hashmap* map, const char* key) {
    unsigned int idx;
    ramfs_file_hash_node* node;
    ramfs_file_hash_node* prev;

    if (!map || !key) return 0;

    idx = _file_hash(key);
    node = map->buckets[idx];
    prev = NULL;
    while (node) {
        if (_file_streq(node->key, key)) {
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

/* 返回条目总数 */
int ramfs_file_hashmap_count(ramfs_file_hashmap* map) {
    return map ? map->count : 0;
}

/* 列出所有文件名（键），返回 char[n][128]，需调用者 free */
char* ramfs_file_hashmap_list(ramfs_file_hashmap* map) {
    int i, pos;
    ramfs_file_hash_node* node;
    char (*result)[128];

    if (!map || map->count == 0) return NULL;

    result = (char(*)[128])malloc(map->count * 128);
    if (!result) return NULL;

    pos = 0;
    for (i = 0; i < RAMFS_FILE_BUCKET_SIZE; ++i) {
        node = map->buckets[i];
        while (node) {
            _file_strcpy(result[pos], node->key);
            pos++;
            node = node->next;
        }
    }
    return (char*)result;
}

/* 销毁哈希表：释放所有节点内存 */
void ramfs_file_hashmap_destroy(ramfs_file_hashmap* map) {
    int i;
    ramfs_file_hash_node* node;
    ramfs_file_hash_node* next;

    if (!map) return;
    for (i = 0; i < RAMFS_FILE_BUCKET_SIZE; ++i) {
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
