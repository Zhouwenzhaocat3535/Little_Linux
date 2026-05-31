/* 简易 malloc 实现 - 裸金属版 */
/* 无任何标准库依赖，以 8 字节为单位分配，支持自动扩容 */
#include "malloc.h"
/* 全局空闲链表头 */
static free_block_t *free_list = (free_block_t*)0;

/* ------------------------------------------------------------
 * 内存申请接口（代替 sbrk / 系统调用）
 * 在裸金属环境中，我们需要知道 RAM 的起始和结束地址。
 * 这里演示通过链接器符号 _end (BSS 结束) 和 预设的 RAM_SIZE 来管理。
 * 你也可直接使用固定的数组或物理地址。
 * ------------------------------------------------------------ */

/* 声明由链接脚本提供的堆起始符号（通常 _end 表示 BSS 结束后的第一个字节） */
extern char _end;
static char *heap_brk = &_end;          /* 当前堆顶指针 */
static char *heap_max; /* 堆上限 */

/* 从底层获得一块连续内存（失败返回 NULL） */
void *request_memory(size_t size) {
    /* 对齐请求大小到 8 字节，保证块边界整齐 */
    size = ALIGN_UP(size);
    if (heap_brk + size > heap_max) {
        printf("request_memory failed:out of memory\nsize=%d\n",(int)size);
        return (void*)0;   /* 内存耗尽 */
    }
    void *ptr = (void*)heap_brk;
    heap_brk += size;
    return ptr;
}

/* ------------------------------------------------------------
 * 初始化：申请一块 64MB 的内存池，并初始化为一个空闲块
 * ------------------------------------------------------------ */
void malloc_init(uint32_t total_mb) {
	printf("malloc initing");
	printf("_end address = 0x%x\n", (unsigned int)&_end);
    uint32_t total_bytes = total_mb * 1024 * 1024;   // 总内存字节数（32位）
    heap_max = (char*)((uint32_t)&_end + total_bytes);
    printf("heap_max = 0x%x\n", (unsigned int)heap_max);
    
    const size_t POOL_SIZE = 16 * 1024 * 1024;  // 初始16MB
    char *mem = (char*)request_memory(POOL_SIZE);
    if (mem == (char*)0) {
        printf("malloc_init_error\n");
        return;
    }
    /* 将这块内存变成空闲块 */
    free_block_t *block = (free_block_t*)mem;
    block->size = POOL_SIZE - sizeof(free_block_t);
    block->next = free_list;   /* 插入链表头部 */
    free_list = block;
    printf("\nmalloc init succeess\n");
}

/* ------------------------------------------------------------
 * 扩容：再申请一个 64MB 的内存池，并添加到空闲链表
 * ------------------------------------------------------------ */
int expand_heap(void) {
    const size_t POOL_SIZE = 16ULL * 1024 * 1024;
    char *mem = (char*)request_memory(POOL_SIZE);
    if (mem == (char*)0) {
		printf("expand_heap error\n");
        return -1;   /* 扩容失败 */
    }
    free_block_t *new_block = (free_block_t*)mem;
    new_block->size = POOL_SIZE - sizeof(free_block_t);
    new_block->next = free_list;
    free_list = new_block;
    printf("expand_heap succeess");
    return 0;
}

/* ------------------------------------------------------------
 * malloc 核心分配函数
 * 返回 8 字节对齐的用户内存块，若失败则返回 0
 * ------------------------------------------------------------ */
void *malloc(size_t size) {
    if (size == 0) return (void*)0;
    /* 用户请求的大小按 8 字节对齐，并且至少 8 字节 */
    size_t req_size = ALIGN_UP(size);
    if (req_size < 8) req_size = 8;

    free_block_t *prev = (free_block_t*)0;
    free_block_t *curr = free_list;

    /* 首次适应算法：查找第一个足够大的空闲块 */
    while (curr != (free_block_t*)0) {
        if (curr->size >= req_size) {
            /* 找到了足够大的空闲块，尝试分裂（剩余空间还能再形成一个空闲块） */
            size_t remain = curr->size - req_size;
            /* 剩余空间至少能容纳一个空闲块头部 + 8 字节数据区 */
            if (remain >= sizeof(free_block_t) + 8) {
                /* 分裂出尾部剩余块 */
                free_block_t *new_free = (free_block_t*)((char*)curr + sizeof(free_block_t) + req_size);
                new_free->size = remain - sizeof(free_block_t);
                new_free->next = curr->next;
                /* 当前块缩小 */
                curr->size = req_size;
                /* 更新链表：用 new_free 替换 curr 的位置 */
                if (prev == (free_block_t*)0) {
                    free_list = new_free;
                } else {
                    prev->next = new_free;
                }
            } else {
                /* 剩余空间太小，不再分裂，直接将整个块从链表中移除 */
                if (prev == (free_block_t*)0) {
                    free_list = curr->next;
                } else {
                    prev->next = curr->next;
                }
            }
            /* 返回用户区域指针（跳过头部） */
            return (void*)((char*)curr + sizeof(free_block_t));
        }
        prev = curr;
        curr = curr->next;
    }

    /* 没有找到合适大小的空闲块，尝试扩容 */
    if (expand_heap() == 0) {
        /* 扩容成功，递归调用自己重新分配（注意此时新块已在链表头部） */
        return malloc(size);
    }
    /* 扩容失败，无可用内存 */
    return (void*)0;
}

/* ------------------------------------------------------------
 * free：释放内存块，将其重新插入空闲链表
 * 简单实现：不合并相邻空闲块（避免复杂逻辑）
 * 若需要合并，可在此处增加合并相邻块的代码
 * ------------------------------------------------------------ */
void free(void *ptr) {
    if (ptr == (void*)0) return;
    /* 获取块头部地址 */
    free_block_t *block = (free_block_t*)((char*)ptr - sizeof(free_block_t));
    /* 将释放的块直接插回链表头部（不合并） */
    block->next = free_list;
    free_list = block;
}
