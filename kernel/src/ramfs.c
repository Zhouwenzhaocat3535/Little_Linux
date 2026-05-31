/*
 * ramfs.c - 极简内存文件系统实现
 * 裸金属专用，不依赖任何标准库（除用户提供的 malloc/free/printf 和三个字符串函数）
 */
 
#include "ramfs.h"

ramfs_foder root;

ramfs_foder *current_dir;
char current_path[512];

static void build_absolute_path(ramfs_foder *dir, char *out, size_t out_size) {
    if (dir->parent && dir != &root) {
        build_absolute_path(dir->parent, out, out_size);
        size_t len = strlen(out);
        if (out[len-1] != '/') strcat(out, "/");
        strcat(out, dir->name);
    } else {
        strcpy(out, "/");
    }
}

/* 从路径中提取最后一个组件，并返回父目录路径（修改输入缓冲区） */
/*
static char* _basename(char* path) {
    char* last_slash = NULL;
    char* p = path;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }
    if (last_slash) {
        *last_slash = '\0';
        return last_slash + 1;
    }
    return path;
}
*/

static void lazy_load_directory(ramfs_foder *dir) {
    if (!dir) return;
    /* 仅当 type==DISK_CLEAN 且哈希表为空时才需要加载 */
    if (dir->type == RAMFS_FOLDER_TYPE_DISK_CLEAN && 
        dir->foder_map.count == 0 && dir->file_map.count == 0) {
        dfs_load_folder_children(dir);
    }
}

/* 根据路径获取目标目录指针（用于 cd），并自动懒加载磁盘目录内容 */
static ramfs_foder* _get_dir_by_path(ramfs_foder* base, const char* path) {
    char buf[512];
    char* part;
    ramfs_foder* cur = base;
    ramfs_foder_info* info;

    if (!path || !path[0]) return NULL;
    strcpy(buf, path);
    
    if (buf[0] == '/') {
        cur = &root;
        part = buf + 1;
    } else {
        part = buf;
    }
    
    while (1) {
        char* next_slash = part;
        while (*next_slash && *next_slash != '/') next_slash++;
        int is_last = (*next_slash == '\0');
        if (!is_last) *next_slash = '\0';
        
        if (part[0] != '\0') {
            if (streq(part, ".")) {
                // 忽略
            } else if (streq(part, "..")) {
                if (cur->parent) cur = cur->parent;
                else return cur;
            } else {
                info = ramfs_hashmap_find(&cur->foder_map, part);
                if (!info) return NULL;
                cur = info->ptr;
                
                // 懒加载磁盘目录
                if (cur->type == RAMFS_FOLDER_TYPE_DISK_CLEAN && 
                    cur->foder_map.count == 0 && cur->file_map.count == 0) {
					if (!info->ptr) {
						/* 1. 分配内存 */
						ramfs_foder *sub = (ramfs_foder*)malloc(sizeof(ramfs_foder));
						if (!sub) return NULL;  /* 立即检查 */
    
						/* 2. 初始化结构体（清零 + 设置字段）*/
						memset(sub, 0, sizeof(ramfs_foder));
						strcpy(sub->name, info->name);
						sub->type = RAMFS_FOLDER_TYPE_DISK_CLEAN;
						sub->drive_addr = info->drive_addr;
						sub->is_mount_root = 0;
						sub->parent = cur;
						sub->mount = cur->mount;
						ramfs_hashmap_init(&sub->foder_map);
						ramfs_file_hashmap_init(&sub->file_map);
    
						/* 3. 从磁盘加载子项 */
						int ret = dfs_load_folder_children(sub);
						if (ret != 0) {
							/* 加载失败，清理并返回 */
							free(sub);
							return NULL;
						}
    
						/* 4. 更新父目录哈希表 */
						info->ptr = sub;
					}
                }
            }
        }
        
        if (is_last) break;
        part = next_slash + 1;
    }
    return cur;
}

/* ========== 核心文件系统函数 ========== */

/* 创建目录（不支持递归） */
int fs_mkdir(const char* name, const char* from) {
    ramfs_foder* parent;
    ramfs_foder* new_dir;
    ramfs_foder_info info;
    
    if (!name || strlen(name) == 0 || strlen(name) >= 128)
        return -1;
    
    /* 确定父目录 */
    if (from) {
        parent = _get_dir_by_path(current_dir, from);
        if (!parent) return -2;
    } else {
        parent = current_dir;
    }
    
    /* 懒加载父目录内容（保证检查时已有子项列表）*/
    lazy_load_directory(parent);
    
    /* 检查是否已存在同名目录或文件 */
    if (ramfs_hashmap_find(&parent->foder_map, name))
        return -3;
    if (ramfs_file_hashmap_find(&parent->file_map, name))
        return -4;
    
    /* 创建新目录节点（纯内存，不分配磁盘）*/
    new_dir = (ramfs_foder*)malloc(sizeof(ramfs_foder));
    if (!new_dir) return -5;
    memset(new_dir, 0, sizeof(ramfs_foder));
    strcpy(new_dir->name, name);
    if (parent->mount == NULL) {
		new_dir->type = RAMFS_FOLDER_TYPE_MEM;   // 纯内存目录
	} else {
		new_dir->type = RAMFS_FOLDER_TYPE_NEW;   // 需要同步的目录
	}
    new_dir->drive_addr = 0;                 // 尚未分配磁盘槽位
    new_dir->is_mount_root = 0;
    new_dir->parent = parent;                // 设置父指针
    new_dir->mount = parent->mount;          // 继承父目录的挂载点（可能为 NULL）
    ramfs_hashmap_init(&new_dir->foder_map);
    ramfs_file_hashmap_init(&new_dir->file_map);
    
    /* 插入父目录哈希表 */
    strcpy(info.name, name);
    info.type = new_dir->type;
    info.ptr = new_dir;
    info.drive_addr = 0;
    if (!ramfs_hashmap_insert(&parent->foder_map, name, &info)) {
        free(new_dir);
        return -5;
    }
    
    /* 标记父目录为“子项有修改”（如果它原本是 DISK_CLEAN）*/
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;
    
    return 0;
}

/* 切换当前工作目录 */
int fs_cd(const char* path) {
    ramfs_foder* target;
    
    if (!path) return -1;
    
    if (streq(path, "/")) {
        target = &root;
    } else if (path[0] == '/') {
        target = _get_dir_by_path(&root, path);
        if (!target) return -2;
    } else {
        target = _get_dir_by_path(current_dir, path);
        if (!target) return -2;
    }
    
    // 使用父指针链构建绝对路径
    build_absolute_path(target, current_path, sizeof(current_path));
    current_dir = target;
    env_set("pwd", current_path);
    return 0;
}

/* 删除文件（支持路径） */
int fs_rm(const char* name, const char* from) {
    if (!name || strlen(name) == 0) return -1;

    char path_copy[512];
    strcpy(path_copy, name);
    
    char *last_slash = strrchr(path_copy, '/');
    char *file_name;
    ramfs_foder *parent;
    
    if (last_slash == NULL) {
        // 没有路径，父目录为当前目录或 from 指定目录
        if (from) {
            parent = _get_dir_by_path(current_dir, from);
            if (!parent) return -2;
        } else {
            parent = current_dir;
        }
        file_name = (char*)name;
    } else {
        *last_slash = '\0';
        file_name = last_slash + 1;
        if (file_name[0] == '\0') return -1; // 空文件名
        
        if (path_copy[0] == '/') {
            parent = _get_dir_by_path(&root, path_copy);
        } else {
            if (from) {
                ramfs_foder *base = _get_dir_by_path(current_dir, from);
                if (!base) return -2;
                parent = _get_dir_by_path(base, path_copy);
            } else {
                parent = _get_dir_by_path(current_dir, path_copy);
            }
        }
        if (!parent) return -2;
    }
    
    lazy_load_directory(parent);
    
    // 查找文件
    ramfs_file_info *info = ramfs_file_hashmap_find(&parent->file_map, file_name);
    if (!info) return -3;  // 文件不存在
    
    // 释放文件数据内存
    if (info->ptr) free(info->ptr);
    
    // 从哈希表中删除
    if (!ramfs_file_hashmap_delete(&parent->file_map, file_name))
        return -4;
    
    // 父目录标记为 dirty
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;
    
    return 0;
}

/* 删除空目录 */
int fs_rmdir(const char* name, const char* from) {
    ramfs_foder* parent;
    ramfs_foder_info* info;
    ramfs_foder* target;
    
    if (!name) return -1;
    
    if (from) {
        parent = _get_dir_by_path(current_dir, from);
        if (!parent) return -2;
    } else {
        parent = current_dir;
    }
    
    lazy_load_directory(parent);
    
    info = ramfs_hashmap_find(&parent->foder_map, name);
    if (!info) return -3;
    target = info->ptr;
    
    /* 检查目录是否为空 */
    if (target->foder_map.count != 0 || target->file_map.count != 0)
        return -4;
    
    /* 从父目录哈希表删除 */
    ramfs_hashmap_delete(&parent->foder_map, name);
    
    /* 释放子目录内部资源 */
    ramfs_hashmap_destroy(&target->foder_map);
    ramfs_file_hashmap_destroy(&target->file_map);
    free(target);
    
    /* 父目录标记为 dirty */
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;
    
    return 0;
}

/* 创建空文件 */
int fs_touch(const char* name, const char* from) {
    if (!name || strlen(name) == 0 || strlen(name) >= 128) return -1;

    char path_copy[512];
    strcpy(path_copy, name);
    
    char *last_slash = strrchr(path_copy, '/');
    char *file_name;
    ramfs_foder *parent;
    
    if (last_slash == NULL) {
        // 没有斜杠，父目录为当前目录（或 from 指定的目录）
        if (from) {
            parent = _get_dir_by_path(current_dir, from);
            if (!parent) return -2;
        } else {
            parent = current_dir;
        }
        file_name = (char*)name;   // 指向原字符串，但后续会复制，安全
    } else {
        // 有斜杠，分离出父目录路径和文件名
        *last_slash = '\0';
        file_name = last_slash + 1;
        if (file_name[0] == '\0') return -1;   // 禁止末尾斜杠
        
        // 解析父目录
        if (path_copy[0] == '/') {
            // 绝对路径
            parent = _get_dir_by_path(&root, path_copy);
        } else {
            // 相对路径
            if (from) {
                ramfs_foder *base = _get_dir_by_path(current_dir, from);
                if (!base) return -2;
                parent = _get_dir_by_path(base, path_copy);
            } else {
                parent = _get_dir_by_path(current_dir, path_copy);
            }
        }
        if (!parent) return -2;
    }
    
    // 懒加载父目录内容
    lazy_load_directory(parent);
    
    // 检查是否存在同名目录或文件
    if (ramfs_hashmap_find(&parent->foder_map, file_name))
        return -3;
    if (ramfs_file_hashmap_find(&parent->file_map, file_name))
        return -4;
    
    // 创建文件条目
    ramfs_file_info info;
    memset(&info, 0, sizeof(info));
    strcpy(info.name, file_name);
    info.type = (parent->mount == NULL) ? RAMFS_TYPE_MEM : RAMFS_TYPE_NEW;
    info.length = 0;
    info.ptr = NULL;
    info.drive_addr = 0;
    
    if (!ramfs_file_hashmap_insert(&parent->file_map, file_name, &info))
        return -5;
    
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;
    
    return 0;
}
/* 写入内容到文件（覆盖模式） */
int fs_write(const char* name, const char* data, size_t len, const char* from) {
    ramfs_foder* parent;
    ramfs_file_info* info;
    void* new_ptr;
    
    if (!name || !data) return -1;
    
    if (from) {
        parent = _get_dir_by_path(current_dir, from);
        if (!parent) return -2;
    } else {
        parent = current_dir;
    }
    
    lazy_load_directory(parent);
    
    info = ramfs_file_hashmap_find(&parent->file_map, name);
    if (!info) return -3;
    
    /* 释放原有内存数据 */
    if (info->ptr) free(info->ptr);
    
    /* 分配新内存并复制数据 */
    new_ptr = malloc(len);
    if (!new_ptr && len > 0) return -4;
    if (len > 0) {
        unsigned char* dst = (unsigned char*)new_ptr;
        const unsigned char* src = (const unsigned char*)data;
        for (size_t i = 0; i < len; i++) dst[i] = src[i];
    }
    info->ptr = new_ptr;
    info->length = len;
    /* 注意：drive_addr 保持不变（如果是新文件则为0，同步时再分配）*/
    if(parent->mount==NULL)
		info->type = RAMFS_TYPE_MEM;
    if (info->drive_addr == 0) {
        info->type = RAMFS_TYPE_NEW;   // 新文件，尚未分配磁盘块
    } else {
        info->type = RAMFS_TYPE_DIRTY; // 已有磁盘块，内容已修改
    }
    
    /* 父目录标记为 dirty */
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;
    
    return 0;
}

/* 读取并打印文件内容 */

int fs_cat(const char* name, const char* from) {
    ramfs_foder* parent;
    ramfs_file_info* info;
    size_t i;
    unsigned char* p;
    
    if (!name) return -1;
    
    if (from) {
        parent = _get_dir_by_path(current_dir, from);
        if (!parent) return -2;
    } else {
        parent = current_dir;
    }
    
    lazy_load_directory(parent);
    
    info = ramfs_file_hashmap_find(&parent->file_map, name);
    if (!info) return -3;
    
    /* 通过 DFS 接口加载文件数据（如果需要）*/
    if (parent->mount) {   // 只有在挂载点内的文件才需要从磁盘加载
        int ret = dfs_load_file_data(info, parent->mount);
        if (ret != 0) return -4;
    }
    
    if (!info->ptr || info->length == 0) return 0;
    
    p = (unsigned char*)info->ptr;
    for (i = 0; i < info->length; i++) {
        printf("%c", p[i]);
    }
    return 0;
}
/* 列出当前目录内容 */
int fs_ls(void) {
    int i;
    ramfs_hash_node* node;
    ramfs_file_hash_node* fnode;
    
    printf("Directories:\n");
    for (i = 0; i < RAMFS_HASH_BUCKET_SIZE; i++) {
        node = current_dir->foder_map.buckets[i];
        while (node) {
            printf("  %s/\n", node->key);
            node = node->next;
        }
    }
    printf("Files:\n");
    for (i = 0; i < RAMFS_FILE_BUCKET_SIZE; i++) {
        fnode = current_dir->file_map.buckets[i];
        while (fnode) {
            printf("  %s (%lld bytes)\n", fnode->key, fnode->value.length);
            fnode = fnode->next;
        }
    }
    return 0;
}

/* 从文件末尾删除指定数量的字符 */
int fs_backspace(const char* name, int count, const char* from) {
    if (!name || count < 0) return -1;
    if (count == 0) return 0;  // 无操作

    // 解析路径，获取父目录和文件名
    char path_copy[512];
    strcpy(path_copy, name);
    char *last_slash = strrchr(path_copy, '/');
    char *file_name;
    ramfs_foder *parent;

    if (last_slash == NULL) {
        // 没有路径，父目录为当前目录或 from 指定目录
        if (from) {
            parent = _get_dir_by_path(current_dir, from);
            if (!parent) return -2;
        } else {
            parent = current_dir;
        }
        file_name = (char*)name;
    } else {
        *last_slash = '\0';
        file_name = last_slash + 1;
        if (file_name[0] == '\0') return -1;  // 空文件名

        if (path_copy[0] == '/') {
            parent = _get_dir_by_path(&root, path_copy);
        } else {
            if (from) {
                ramfs_foder *base = _get_dir_by_path(current_dir, from);
                if (!base) return -2;
                parent = _get_dir_by_path(base, path_copy);
            } else {
                parent = _get_dir_by_path(current_dir, path_copy);
            }
        }
        if (!parent) return -2;
    }

    lazy_load_directory(parent);
    ramfs_file_info *info = ramfs_file_hashmap_find(&parent->file_map, file_name);
    if (!info) return -3;  // 文件不存在

    // 若文件来自挂载点，需先加载数据
    if (parent->mount) {
        int ret = dfs_load_file_data(info, parent->mount);
        if (ret != 0) return -4;
    }

    if (info->length == 0) return -5;       // 空文件无法删除
    if (count > info->length) count = info->length;  // 最多删到空

    size_t new_len = info->length - count;
    if (new_len == 0) {
        // 文件变空，释放原数据
        if (info->ptr) free(info->ptr);
        info->ptr = NULL;
        info->length = 0;
    } else {
        void *new_ptr = malloc(new_len);
        if (!new_ptr) return -6;            // 内存不足
        memcpy(new_ptr, info->ptr, new_len);
        free(info->ptr);
        info->ptr = new_ptr;
        info->length = new_len;
    }

    // 标记文件为已修改
    info->type = (info->drive_addr == 0) ? RAMFS_TYPE_NEW : RAMFS_TYPE_DIRTY;
    if (parent->type == RAMFS_FOLDER_TYPE_DISK_CLEAN)
        parent->type = RAMFS_FOLDER_TYPE_DIRTY;

    return 0;
}

/* ========== API 包装函数（命令行解析） ========== */

int fs_mkdir_api(int argc, char** argv) {
    int opt_p = 0, opt_v = 0;
    int i = 1;
    char* dirname = NULL;
    
    if (argc == 1) {  /* 仅命令本身 */
        printf("mkdir: missing operand\n");
        printf("Try 'mkdir --help' for more information.\n");
        return -1;
    }
    
    for (; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (streq(argv[i], "--help")) {
                printf("Usage: mkdir [OPTION]... DIRECTORY...\n");
                printf("Create DIRECTORY(ies).\n");
                printf("  -p      create parent directories as needed\n");
                printf("  -v      print a message for each created directory\n");
                printf("  --version  output version information\n");
                return 0;
            } else if (streq(argv[i], "--version")) {
                printf("ramfs mkdir version 1.0.0\n");
                return 0;
            } else if (argv[i][1] == 'p') {
                opt_p = 1;
            } else if (argv[i][1] == 'v') {
                opt_v = 1;
            } else {
                printf("mkdir: invalid option %s\n", argv[i]);
                return -1;
            }
        } else {
            dirname = argv[i];
            break;
        }
    }
    
    if (!dirname) {
        printf("mkdir: missing directory operand\n");
        return -1;
    }
    
    if (opt_p) {
		// 复制路径，用于拆分
		char path_copy[512];
		strcpy(path_copy, dirname);
    
		// 确定起始目录：绝对路径从根开始，相对路径从当前目录开始
		ramfs_foder* cur_dir = current_dir;
		char* token;
    
		// 处理绝对路径：先 cd 到根（不改变全局 current_dir，仅逻辑上）
		if (dirname[0] == '/') {
			cur_dir = &root;
			token = path_copy + 1; // 跳过第一个 '/'
		} else {
			token = path_copy;
		}
    
		// 逐级处理
		while (token && token[0] != '\0') {
			// 提取当前分量
			char* slash = token;
			while (*slash && *slash != '/') slash++;
			int is_last = (*slash == '\0');
			if (*slash == '/') *slash = '\0';
			
			if (token[0] != '\0' && !streq(token, ".")) {
            // 检查在当前 cur_dir 下是否存在该子目录
				ramfs_foder_info* info = ramfs_hashmap_find(&cur_dir->foder_map, token);
				if (!info) {
					// 不存在，调用 fs_mkdir 创建
					// 注意：fs_mkdir 的 from 参数是相对于当前工作目录的路径
					// 而 cur_dir 可能不是 current_dir，因此我们需要临时改变 current_dir
					ramfs_foder* old_dir = current_dir;
					current_dir = cur_dir;   // 临时切换父目录
					int ret = fs_mkdir(token, NULL);  // 在当前临时目录下创建
					current_dir = old_dir;
					if (ret != 0) {
						printf("mkdir: failed to create '%s'\n", token);
						return -1;
					}
					if (opt_v) printf("mkdir: created directory '%s'\n", token);
					// 重新查找刚才创建的目录，更新 cur_dir
					info = ramfs_hashmap_find(&cur_dir->foder_map, token);
					if (!info) return -1;
				}
				// 进入该子目录，继续下一级
				cur_dir = info->ptr;
			}
        
			if (is_last) break;
			token = slash + 1;
		}
	} else {
        int ret = fs_mkdir(dirname, NULL);
        if (ret == -1) printf("mkdir: invalid name\n");
        else if (ret == -2) printf("mkdir: cannot create directory '%s': No such file or directory\n", dirname);
        else if (ret == -3 || ret == -4) printf("mkdir: cannot create directory '%s': File exists\n", dirname);
        else if (ret == -5) printf("mkdir: memory allocation failed\n");
        else if (opt_v) printf("mkdir: created directory '%s'\n", dirname);
    }
    return 0;
}

int fs_cd_api(int argc, char** argv) {
    if (argc == 1) {
        printf("cd: missing operand\n");
        return -1;
    }
    if (argc > 2) {
        printf("cd: too many arguments\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("cd [DIRECTORY]\n");
        printf("Change the current directory to DIRECTORY.\n");
        return 0;
    }
    if (streq(argv[1], "--version")) {
        printf("ramfs cd version 1.0.0\n");
        return 0;
    }
    int ret = fs_cd(argv[1]);
    if (ret == -1) printf("cd: invalid argument\n");
    else if (ret == -2) printf("cd: no such directory: %s\n", argv[1]);
    return 0;
}

int fs_rm_api(int argc, char** argv) {
    if (argc != 2) {
        printf("rm: missing file operand\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("rm FILE\nRemove a file.\n");
        return 0;
    }
    if (streq(argv[1], "--version")) {
        printf("ramfs rm version 1.0.0\n");
        return 0;
    }
    int ret = fs_rm(argv[1], NULL);
    if (ret == -1) printf("rm: invalid file name\n");
    else if (ret == -2) printf("rm: parent directory not found\n");
    else if (ret == -3) printf("rm: file not found\n");
    else if (ret == -4) printf("rm: deletion failed\n");
    return 0;
}

int fs_rmdir_api(int argc, char** argv) {
    if (argc != 2) {
        printf("rmdir: missing operand\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("rmdir DIRECTORY\n");
        printf("Remove empty directory.\n");
        return 0;
    }
    if (streq(argv[1], "--version")) {
        printf("ramfs rmdir version 1.0.0\n");
        return 0;
    }
    int ret = fs_rmdir(argv[1], NULL);
    if (ret == -1) printf("rmdir: invalid name\n");
    else if (ret == -2) printf("rmdir: failed to locate parent\n");
    else if (ret == -3) printf("rmdir: '%s' is not a directory\n", argv[1]);
    else if (ret == -4) printf("rmdir: directory not empty\n");
    return 0;
}

int fs_touch_api(int argc, char** argv) {
    if (argc != 2) {
        printf("touch: missing file operand\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("touch FILE\nCreate an empty file.\n");
        return 0;
    }
    if (streq(argv[1], "--version")) {
        printf("ramfs touch 1.0.0\n");
        return 0;
    }
    int ret = fs_touch(argv[1], NULL);
    if (ret == -1) printf("touch: invalid file name\n");
    else if (ret == -2) printf("touch: parent directory not found\n");
    else if (ret == -3 || ret == -4) printf("touch: file already exists\n");
    else if (ret == -5) printf("touch: memory error\n");
    return 0;
}

int fs_write_api(int argc, char** argv) {
    if (argc != 3) {
        printf("write: usage write FILE \"content\"\n");
        return -1;
    }
    int ret = fs_write(argv[1], argv[2], strlen(argv[2]), NULL);
    if (ret == -1) printf("write: invalid arguments\n");
    else if (ret == -2) printf("write: parent not found\n");
    else if (ret == -3) printf("write: file not found\n");
    else if (ret == -4) printf("write: out of memory\n");
    return 0;
}

int fs_cat_api(int argc, char** argv) {
    if (argc != 2) {
        printf("cat: missing file operand\n");
        return -1;
    }
    int ret = fs_cat(argv[1], NULL);
    if (ret == -1) printf("cat: invalid arguments\n");
    else if (ret == -2) printf("cat: cannot open parent\n");
    else if (ret == -3) printf("cat: file not found\n");
    return 0;
}

int fs_ls_api(int argc, char** argv) {
    if (argc > 1 && (streq(argv[1], "--help") || streq(argv[1], "-h"))) {
        printf("ls: list directory contents\n");
        return 0;
    }
    return fs_ls();
}

int fs_pwd_api(int argc, char** argv) {
    printf("%s\n", current_path);
    return 0;
}

int fs_backspace_api(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        printf("backspace: usage backspace FILE [COUNT]\n");
        return -1;
    }
    if (streq(argv[1], "--help")) {
        printf("backspace FILE [COUNT]\n");
        printf("Remove COUNT characters from the end of FILE (default 1).\n");
        return 0;
    }
    int count = 1;
    if (argc == 3) {
        count = atoi(argv[2],10);
        if (count < 0) {
            printf("backspace: count must be non-negative\n");
            return -1;
        }
    }
    int ret = fs_backspace(argv[1], count, NULL);
    if (ret == -1) printf("backspace: invalid file name\n");
    else if (ret == -2) printf("backspace: parent directory not found\n");
    else if (ret == -3) printf("backspace: file not found\n");
    else if (ret == -4) printf("backspace: failed to load file data\n");
    else if (ret == -5) printf("backspace: file is empty\n");
    else if (ret == -6) printf("backspace: out of memory\n");
    return 0;
}

/* ========== 初始化及全局根目录 ========== */
int fs_init(void) {
    strcpy(root.name, "/");
    root.type = 0;
    ramfs_hashmap_init(&root.foder_map);
    ramfs_file_hashmap_init(&root.file_map);
    
    current_dir = &root;
    const char* pwd = env_get("pwd");
    if (pwd) {
        strcpy(current_path, pwd);
    } else {
        strcpy(current_path, "/");
        env_set("pwd", "/");
    }
    /* 注册所有命令*/
    register_command("mkdir", "Create a directory", fs_mkdir_api);
    register_command("cd", "Change directory", fs_cd_api);
    register_command("rmdir", "Remove empty directory", fs_rmdir_api);
    register_command("touch", "Create an empty file", fs_touch_api);
    register_command("write", "Write content to a file", fs_write_api);
    register_command("cat", "Print file content", fs_cat_api);
    register_command("ls", "List directory contents", fs_ls_api);
    register_command("pwd", "Print working directory", fs_pwd_api);
    register_command("rm", "Remove a file", fs_rm_api);
    register_command("backspace", "Remove characters from end of file", fs_backspace_api);
    return 0;
}

ramfs_foder* ramfs_get_dir_by_abs_path(const char *path) {
    return _get_dir_by_path(&root, path);
}

