#include "multiboot.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "command_system.h"
#include "stdio.h"
#include "env_var.h"
#include "stdlib.h"
#include "ramfs.h"
#include "drive.h"
#include "partition.h"
#include "dfs.h"

extern uint64_t drive_size_bytes[];

void ahci_init(void);
char getchar_from_buffer(void);
void my_own_os_main();
int my_os_command_run(CommandEntry *cmd, int fun_argc, char *fun_argv[]);
int my_os_command_help(int argc, char* argv[]);

#define MAX_ARGS 64

char cpumodel[512];
int memory_size_mb;

static unsigned long my_os_get_memory_size_mb(unsigned long multiboot_info_ptr)
{
    struct multiboot_info *info = (struct multiboot_info*)multiboot_info_ptr;
    if (!(info->flags & MULTIBOOT_INFO_MEM_MAP)) {
        // 没有内存映射，降级使用 mem_lower/mem_upper
        return((info->mem_lower + info->mem_upper) / 1024);
    }

    uint64_t total_bytes = 0;
    struct multiboot_mmap_entry *mmap = (struct multiboot_mmap_entry*)info->mmap_addr;
    while ((uint32_t)mmap < info->mmap_addr + info->mmap_length) {
        if (mmap->type == 1) {  // 可用 RAM
            total_bytes += mmap->len;
        }
        mmap = (struct multiboot_mmap_entry*)((uint32_t)mmap + mmap->size + sizeof(mmap->size));
    }
    return(total_bytes / (1024 * 1024));
}

static inline char my_os_getch(unsigned int timeout_ms)
{
	return(getchar_from_buffer());
}

static int my_os_get_cpu(char *buf, size_t buflen)
{
    // 品牌字符串需要三次 CPUID 调用 (0x80000002 ~ 0x80000004)
    uint32_t brand[12] = {0};  // 48 字节 = 12 个 uint32_t
    uint32_t eax, ebx, ecx, edx;

    for (int i = 0; i < 3; i++) {
        eax = 0x80000002 + i;
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(eax));
        brand[i * 4 + 0] = eax;
        brand[i * 4 + 1] = ebx;
        brand[i * 4 + 2] = ecx;
        brand[i * 4 + 3] = edx;
    }

    // 品牌字符串可能包含尾部空格，但直接拷贝即可
    size_t src_len = strnlen((char*)brand, sizeof(brand));
    if (buflen == 0)
        return 1;

    if (src_len >= buflen) {
        src_len = buflen - 1;
        memcpy(buf, brand, src_len);
        buf[src_len] = '\0';
        return 1;  // 截断
    } else {
        memcpy(buf, brand, src_len);
        buf[src_len] = '\0';
        return 0;  // 完整拷贝
    }
}

int my_os_command_search(char *command) {
    // 提取命令名（第一个空格前的部分）
    int cmd_len = 0;
    while (command[cmd_len] && command[cmd_len] != ' ') cmd_len++;
    char cmd_name[64];
    if (cmd_len >= sizeof(cmd_name)) cmd_len = sizeof(cmd_name)-1;
    memcpy(cmd_name, command, cmd_len);
    cmd_name[cmd_len] = '\0';

    // 哈希查找命令
    CommandEntry *cmd = find_command(cmd_name);
    if (!cmd) {
        printf("Command not found: %s\n", cmd_name);
        return -1;
    }

    // 构造 argv：第一个元素为命令名
    int argc = 1;
    char *argv[MAX_ARGS];
    // 复制命令名到局部缓冲区（命令执行期间有效）
    char cmd_name_copy[64];
    memcpy(cmd_name_copy, cmd_name, cmd_len + 1);
    argv[0] = cmd_name_copy;

    // 解析参数部分
    char *p = command + cmd_len;
    while (*p == ' ') p++;
    if (*p != '\0') {
        while (*p != '\0' && argc < MAX_ARGS) {
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ') p++;
            if (*p == ' ') {
                *p = '\0';          // 分隔参数
                p++;
                while (*p == ' ') p++;
            }
        }
    }

    // 调用命令运行函数
    return my_os_command_run(cmd, argc, argv);
}

// 对单个参数进行环境变量替换（支持 $VAR 形式）
int my_os_command_expand_env_var(char *arg, size_t out_size)
{
    if (arg[0] != '$') {
        return(0);
    }

    // 变量名去掉 '$'
    char *var_name = arg + 1;
    const char *value = env_get(var_name);
    if (value) {
        strncpy(arg, value, out_size-1);
        arg[out_size-1] = '\0';
        return(0);
    } else {
        // 未定义变量，保持原样不替换
        return(0);
    }
}

int my_os_command_run(CommandEntry *cmd, int fun_argc, char *fun_argv[])
{
    int i;
    for (i = 0; i < fun_argc; i++) {
        my_os_command_expand_env_var(fun_argv[i], 256);
    }

    // 调用实际命令函数
    return cmd->func(fun_argc, fun_argv);
}

void my_own_os_init(unsigned long multiboot_info)
{
	printf("Little Linux is Running\n");
	printf("multiboot_info = 0x%x\n", (unsigned int)multiboot_info);
	if (multiboot_info == 0) {
		printf("ERROR: multiboot_info is NULL! Bootloader did not pass it correctly.\n");
		while(1);
	}
	my_os_get_cpu(cpumodel,512);
	printf("CPU:%s\n", cpumodel);
	memory_size_mb=my_os_get_memory_size_mb(multiboot_info);
	printf("RAM:%dMB\n", memory_size_mb);
	printf("calling malloc_init");
	malloc_init(memory_size_mb);
	register_command("help","Show commands",my_os_command_help);
	env_set("pwd","/");
	fs_init();
	ahci_init();
	update_partition_tables();   // 解析分区表
	printf("Detected %d drive(s)\n", drive_count);
	for (int i = 0; i < drive_count; i++) {
		printf("  drive %d: %llu MB\n", i, drive_size_bytes[i] / (1024*1024));
		printf("    partitions: %d\n", get_partition_count(i));
	}
	dfs_init();
	my_own_os_main();
}

void my_own_os_main()
{
	int buf_len = 0;
	char buffer[512],input,* pwd;
	while(1)
	{
		pwd = (char *)env_get("pwd");
		printf("%s$ ",pwd);
		while(1)
		{
			input=0;
			while(input==0)
				input=my_os_getch(0);
			if(input=='\n'||input=='\r')
			{
				buffer[buf_len]='\0';
				printf("\nrunning command\n");
				my_os_command_search(buffer);
				buf_len=0;
				buffer[0]='\0';
				break;
			}
			else if(input=='\b')
			{
				if(buf_len > 0)
				{
					printf("\b \b");
					buf_len--;
					buffer[buf_len]='\0';
				}
			}
			else
			{
				printf("%c",input);
				buffer[buf_len]=input;
				buf_len++;
			}
		}
	}
}

static void help_callback(const char *name, const char *desc) {
    printf("  %s", name);
    if (desc && desc[0]) {
        printf(" - %s", desc);
    }
    printf("\n");
}

int my_os_command_help(int argc, char* argv[]) {
    printf("Welcome to Little Linux\nAvailable commands:\n");
    foreach_command(help_callback);
    return 0;
}
