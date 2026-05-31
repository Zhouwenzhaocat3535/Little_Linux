#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "tss.h"
#include "tty.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"

void my_own_os_init(uint32_t multiboot_info);

int main(unsigned long multiboot_info) {
	initTerminal();
	printf("[NOTICE] Basic system init done\n");
	printf("[NOTICE] Init TSS\n");

	uint32_t tss_vadr = initTSS();
	initGDT(tss_vadr);
	printf("[NOTICE] Init PIC\n");
	initPIC();	// Re-map PIC
	printf("[NOTICE] Init KBD\n");
	initKBD();
	printf("[NOTICE] Init IDT\n");
	initIDT();	// Load IDT
	printf("[NOTICE] Start Kernel!\n");
	my_own_os_init(multiboot_info);
	return 0;
}
