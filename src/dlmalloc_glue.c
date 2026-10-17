/* dlmalloc configured as a single mspace whose segments live in guest memory. */
#include <stddef.h>

void* zb_dl_mmap(size_t n);
int zb_dl_munmap(void* p, size_t n);

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP(s) zb_dl_mmap(s)
#define DIRECT_MMAP(s) zb_dl_mmap(s)
#define MUNMAP(a, s) zb_dl_munmap((a), (s))
#define DEFAULT_GRANULARITY ((size_t)4U * 1024U * 1024U)
#define DEFAULT_MMAP_THRESHOLD ((size_t)1U * 1024U * 1024U)
#define MALLOC_ALIGNMENT ((size_t)16U)
#define NO_MALLOC_STATS 1

#include "../third_party/dlmalloc/malloc.c"
