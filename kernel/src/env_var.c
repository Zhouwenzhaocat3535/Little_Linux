#include "env_var.h"
#include <stddef.h>
#include "uthash.h"

static EnvVar *env_map = NULL;

// 设置环境变量
extern void env_set(const char *name, const char *value) {
    EnvVar *var;
    HASH_FIND_STR(env_map, name, var);
    if (var == NULL) {
        var = (EnvVar*)malloc(sizeof(EnvVar));
        strncpy(var->name, name, sizeof(var->name)-1);
        var->name[sizeof(var->name)-1] = '\0';
        HASH_ADD_STR(env_map, name, var);
    }
    strncpy(var->value, value, sizeof(var->value)-1);
    var->value[sizeof(var->value)-1] = '\0';
}

// 获取环境变量，返回 NULL 表示不存在
extern const char* env_get(const char *name) {
    EnvVar *var;
    HASH_FIND_STR(env_map, name, var);
    return var ? var->value : NULL;
}
