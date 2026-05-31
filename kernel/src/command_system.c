#include "command_system.h"
#include "uthash.h"

// 全局哈希表指针（初始为 NULL）
static CommandEntry *command_map = NULL;

// 注册一个命令
extern void register_command(const char *name, const char *desc, int (*func)(int, char**)) {
    CommandEntry *entry;
    HASH_FIND_STR(command_map, name, entry);
    if (entry == NULL) {
        entry = (CommandEntry*)malloc(sizeof(CommandEntry));
        entry->name = name;
        entry->desc = desc;
        entry->func = func;
        HASH_ADD_STR(command_map, name, entry);
    } else {
        // 如果已存在，更新描述和函数（可选）
        entry->desc = desc;
        entry->func = func;
    }
}

// 查找命令，成功返回 CommandEntry*，失败返回 NULL
extern CommandEntry* find_command(const char *cmd_name) {
    CommandEntry *entry = NULL;
    HASH_FIND_STR(command_map, cmd_name, entry);
    return entry;
}

void foreach_command(void (*callback)(const char *name, const char *desc)) {
    CommandEntry *entry, *tmp;
    HASH_ITER(hh, command_map, entry, tmp) {
        callback(entry->name, entry->desc);
    }
}
