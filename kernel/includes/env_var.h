#include "uthash.h"

#pragma once

// 环境变量条目
typedef struct {
    char name[64];          // 变量名（固定长度，防止溢出）
    char value[256];        // 变量值
    UT_hash_handle hh;
} EnvVar;

extern void env_set(const char *name, const char *value);

extern const char* env_get(const char *name);
