#ifndef MMAN_H
#define MMAN_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_READ 0x01
#define MAP_SHARED 0x01
#define MAP_FAILED ((void*)-1)

static inline void* mmap(void* addr, size_t len, int prot, int flags, int fd,
                         off_t offset) {
  return MAP_FAILED;
}

static inline int munmap(void* addr, size_t len) { return 0; }

#ifdef __cplusplus
};
#endif

#endif // MMAN_H
