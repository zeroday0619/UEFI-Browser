#ifndef BROWSER_RUNTIME_H
#define BROWSER_RUNTIME_H
#include "../uefi.h"
void runtime_initialize(SystemTable *table);
size_t runtime_memory_used(void);
size_t runtime_memory_limit(void);
size_t runtime_memory_committed(void);
#endif
