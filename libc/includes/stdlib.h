#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

char *itoa(int, char *, int);
int atoi(char *, int);
void exit(int code);
size_t strnlen(const char *s, size_t maxlen);

#endif // _STDLIB_H
