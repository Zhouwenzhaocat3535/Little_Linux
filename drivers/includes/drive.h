#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern int drive_count;
extern void ahci_table_update(void);
extern void *ahci_read(int drive, uint64_t addr, uint64_t length);
extern int ahci_write(int drive, uint64_t addr, uint64_t length, void *data);
