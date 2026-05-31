#pragma once
#include "uthash.h"
typedef struct {
    const char *name;           // 命令名（字符串指针，指向静态字符串）
    const char *desc;           // 描述
    int (*func)(int, char**);   // 函数指针
    UT_hash_handle hh;          // uthash 句柄
} CommandEntry;

extern void register_command(const char *name, const char *desc, int (*func)(int, char**));

extern CommandEntry* find_command(const char *cmd_name);

void foreach_command(void (*callback)(const char *name, const char *desc));
