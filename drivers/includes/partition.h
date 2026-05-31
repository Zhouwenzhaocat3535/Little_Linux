void update_partition_tables(void);
int partition_read(int drive, int part, uint64_t offset, int length, void *buffer);
int partition_write(int drive, int part, uint64_t offset, int length, const void *buffer);
int get_partition_count(int drive);
void partition_init(void);
uint64_t partition_get_sector_count(int drive, int part);
