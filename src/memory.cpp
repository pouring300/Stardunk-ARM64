#include "guest.h"

#include <sys/mman.h>
#include <atomic>
#include <mutex>

extern "C" {
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
}

uint8_t* g_mem = nullptr;
static mspace s_heap;
static std::atomic<uint32_t> s_data_top{DATA_BASE};

struct Region { uint32_t lo, hi; };
static const Region kRegions[] = {
    {THUNK_BASE, DATA_END},
    {IMAGE_BASE, IMAGE_END},
    {STACK_BASE, STACK_BASE + STACK_SIZE * STACK_SLOTS},
    {HEAP_BASE, HEAP_END},
    {COMMPAGE_BASE, COMMPAGE_BASE + 0x2000},
};

bool mem_mapped(uint32_t a) {
    for (const auto& r : kRegions)
        if (a >= r.lo && a < r.hi) return true;
    return false;
}

bool mem_init() {
    if (g_mem) return true;
    void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOGE("cannot reserve 4 GiB guest window: %s", strerror(errno));
        return false;
    }
    g_mem = (uint8_t*)p;
    for (const auto& r : kRegions) {
        if (mprotect(g_mem + r.lo, (size_t)r.hi - r.lo, PROT_READ | PROT_WRITE) != 0) {
            LOGE("mprotect 0x%08x-0x%08x failed: %s", r.lo, r.hi, strerror(errno));
            return false;
        }
    }
    s_heap = create_mspace_with_base(g_mem + HEAP_BASE, HEAP_END - HEAP_BASE, 1);
    LOGI("guest window at %p, heap %u MiB", g_mem, (HEAP_END - HEAP_BASE) >> 20);
    return s_heap != nullptr;
}

uint32_t data_alloc(uint32_t size, uint32_t align) {
    uint32_t cur = s_data_top.load(), start;
    do {
        start = (cur + align - 1) & ~(align - 1);
        if (start + size > DATA_END) {
            LOGE("guest data area exhausted");
            abort();
        }
    } while (!s_data_top.compare_exchange_weak(cur, start + size));
    return start;
}

uint32_t data_strdup(const char* s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = data_alloc(n, 1);
    memcpy(g2h(a), s, n);
    return a;
}

static uint32_t to_guest(void* p) {
    if (!p) return 0;
    return h2g(p);
}

uint32_t gmalloc(uint32_t size) { return to_guest(mspace_malloc(s_heap, size)); }
uint32_t gcalloc(uint32_t n, uint32_t size) { return to_guest(mspace_calloc(s_heap, n, size)); }
uint32_t grealloc(uint32_t p, uint32_t size) { return to_guest(mspace_realloc(s_heap, p ? g2h(p) : nullptr, size)); }
uint32_t gmemalign(uint32_t align, uint32_t size) { return to_guest(mspace_memalign(s_heap, align, size)); }

void gfree(uint32_t p) {
    if (!p) return;
    if (p < HEAP_BASE || p >= HEAP_END) {
        LOGW("free of non-heap pointer 0x%08x ignored", p);
        return;
    }
    mspace_free(s_heap, g2h(p));
}

uint32_t gstrdup(const char* s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = gmalloc(n);
    if (a) memcpy(g2h(a), s, n);
    return a;
}
