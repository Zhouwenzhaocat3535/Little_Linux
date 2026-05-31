/*
 * stdlib.c
 * 
 * Copyright 2026 WenzhaoZhou <wzzhou@wzzhou-Inspiron-5493>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * 
 */


#include "stdlib.h"
#include "stdio.h"

void exit(int code) {
    printf("System rebooting. Code: %d\n", code);
    
    // x86: 通过键盘控制器触发软重启
    // 或者跳转到 BIOS 入口
    __asm__ __volatile__(
        "mov $0xfe, %al\n"   // 键盘控制器命令：脉冲 CPU RESET 线
        "out %al, $0x64\n"
    );
    
    // 如果上面的没生效，fallback 到死循环
    while (1) { __asm__("hlt"); }
}

size_t strnlen(const char *s, size_t maxlen)
{
    const char *p = s;
    size_t count = 0;

    while (count < maxlen && *p != '\0') {
        p++;
        count++;
    }

    return count;
}
