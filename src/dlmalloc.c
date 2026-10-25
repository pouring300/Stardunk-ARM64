// dlmalloc configured as a fixed-capacity, thread-safe mspace over the guest heap window.
#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_DL_PREFIX 1
#define LACKS_SYS_PARAM_H 0
#define MALLOC_INSPECT_ALL 0
#include "../third_party/dlmalloc/malloc.c"
