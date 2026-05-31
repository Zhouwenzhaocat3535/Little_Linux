#pragma once

#include "ramfs_types.h"
#include <stdbool.h>
#include "stdint.h"
#include "string.h"
#include "stdio.h"
#include "malloc.h"
#include "dfs.h"
#include "env_var.h"
#include "dfs.h"
#include "command_system.h"
#include "stdlib.h"

#define streq(a, b) (strcmp(a, b) == 0)

//APIs
int fs_mkdir(const char* name, const char* from);
int fs_cd(const char* path);
int fs_rmdir(const char* name, const char* from);
int fs_rm(const char* name, const char* from);
int fs_touch(const char* name, const char* from);
int fs_write(const char* name, const char* data, size_t len, const char* from);
int fs_cat(const char* name, const char* from);
int fs_ls(void);
int fs_backspace(const char* name, int count, const char* from);
//command APIs
int fs_mkdir_api(int argc, char** argv);
int fs_cd_api(int argc, char** argv);
int fs_rmdir_api(int argc, char** argv);
int fs_rm_api(int argc, char** argv);
int fs_touch_api(int argc, char** argv);
int fs_write_api(int argc, char** argv);
int fs_cat_api(int argc, char** argv);
int fs_ls_api(int argc, char** argv);
int fs_pwd_api(int argc, char** argv);
int fs_backspace_api(int argc, char** argv);

int fs_init(void);
ramfs_foder* ramfs_get_dir_by_abs_path(const char *path);
