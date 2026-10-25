#include "guest.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

uint32_t hle_thunk_for(const char* name);

static std::unordered_map<std::string, Handler>& registry() {
    static std::unordered_map<std::string, Handler> m;
    return m;
}
void hle_register(const char* name, Handler h) { registry()[name] = h; }
Handler hle_lookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

#define R(name, ...) hle_register(name, [](Cpu& c) __VA_ARGS__)

// ---------------------------------------------------------------- data imports
static uint32_t s_sF, s_stack_guard, s_locale_c;
constexpr uint32_t FILE_SIZE = 84;  // sizeof(FILE) on 32-bit bionic

uint32_t hle_data_symbol(const char* name) {
    if (!strcmp(name, "__sF")) return s_sF;
    if (!strcmp(name, "__stack_chk_guard")) return s_stack_guard;
    return 0;
}

// ---------------------------------------------------------------- stdio
// Guest FILE* -> host FILE*. stdout/stderr go to logcat line by line.
struct GFile {
    FILE* f;
    bool log;
    std::string line;
};
static std::mutex s_file_lock;
static std::unordered_map<uint32_t, GFile*> s_files;

static GFile* gfile(uint32_t a) {
    std::lock_guard<std::mutex> g(s_file_lock);
    auto it = s_files.find(a);
    return it == s_files.end() ? nullptr : it->second;
}

static void log_write(GFile* f, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            __android_log_print(ANDROID_LOG_INFO, "StardunkGuest", "%s", f->line.c_str());
            f->line.clear();
        } else {
            f->line += s[i];
        }
    }
}

static size_t file_write(uint32_t fa, const void* p, size_t n) {
    GFile* f = gfile(fa);
    if (!f) return 0;
    if (f->log) { log_write(f, (const char*)p, n); return n; }
    return fwrite(p, 1, n, f->f);
}

static void stdio_init() {
    s_sF = data_alloc(FILE_SIZE * 3, 8);
    s_files[s_sF] = new GFile{stdin, false, {}};
    s_files[s_sF + FILE_SIZE] = new GFile{nullptr, true, {}};
    s_files[s_sF + FILE_SIZE * 2] = new GFile{nullptr, true, {}};
}

static FILE* hostf(uint32_t a) {
    GFile* f = gfile(a);
    return f ? f->f : nullptr;
}

// ---------------------------------------------------------------- pthread
// Guest mutex/cond words (4 bytes on 32-bit bionic) hold a tag + index into host objects.
constexpr uint32_t MUTEX_TAG = 0xa0000000, COND_TAG = 0xb0000000, TAG_MASK = 0xfff00000;
constexpr uint32_t MAX_SYNC = 1 << 16;
static pthread_mutex_t* s_mutexes[MAX_SYNC];
static pthread_cond_t* s_conds[MAX_SYNC];
static std::mutex s_sync_lock;
static std::vector<uint32_t> s_free_mutex, s_free_cond;
static uint32_t s_next_mutex = 1, s_next_cond = 1;

static uint32_t new_mutex(int type) {
    std::lock_guard<std::mutex> g(s_sync_lock);
    uint32_t i;
    if (!s_free_mutex.empty()) { i = s_free_mutex.back(); s_free_mutex.pop_back(); }
    else if (s_next_mutex < MAX_SYNC) i = s_next_mutex++;
    else { LOGE("out of mutexes"); abort(); }
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, type);
    s_mutexes[i] = new pthread_mutex_t;
    pthread_mutex_init(s_mutexes[i], &at);
    pthread_mutexattr_destroy(&at);
    return i;
}

static uint32_t new_cond() {
    std::lock_guard<std::mutex> g(s_sync_lock);
    uint32_t i;
    if (!s_free_cond.empty()) { i = s_free_cond.back(); s_free_cond.pop_back(); }
    else if (s_next_cond < MAX_SYNC) i = s_next_cond++;
    else { LOGE("out of condvars"); abort(); }
    s_conds[i] = new pthread_cond_t;
    pthread_cond_init(s_conds[i], nullptr);
    return i;
}

static pthread_mutex_t* get_mutex(uint32_t a) {
    auto* w = (uint32_t*)g2h(a);
    for (;;) {
        uint32_t v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if ((v & TAG_MASK) == MUTEX_TAG) return s_mutexes[v & ~TAG_MASK];
        int type = (v & 0x4000) ? PTHREAD_MUTEX_RECURSIVE : (v & 0x8000) ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
        uint32_t i = new_mutex(type);
        if (__atomic_compare_exchange_n(w, &v, MUTEX_TAG | i, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return s_mutexes[i];
        std::lock_guard<std::mutex> g(s_sync_lock);
        s_free_mutex.push_back(i);
    }
}

static pthread_cond_t* get_cond(uint32_t a) {
    auto* w = (uint32_t*)g2h(a);
    for (;;) {
        uint32_t v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if ((v & TAG_MASK) == COND_TAG) return s_conds[v & ~TAG_MASK];
        uint32_t i = new_cond();
        if (__atomic_compare_exchange_n(w, &v, COND_TAG | i, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return s_conds[i];
        std::lock_guard<std::mutex> g(s_sync_lock);
        s_free_cond.push_back(i);
    }
}

constexpr int MAX_KEYS = 128;
static std::atomic<uint32_t> s_next_key{0};
static thread_local uint32_t t_specific[MAX_KEYS];

// ---------------------------------------------------------------- mmap
static std::mutex s_map_lock;
static std::unordered_map<uint32_t, uint32_t> s_maps;

// ---------------------------------------------------------------- helpers
static void guest_fatal(const char* what) {
    LOGE("guest fatal: %s", what);
    guest_backtrace(what);
    abort();
}

static void write_guest_stat(uint32_t a, const struct stat& st) {
    uint8_t b[104] = {};
    auto put32 = [&](int off, uint32_t v) { memcpy(b + off, &v, 4); };
    auto put64 = [&](int off, uint64_t v) { memcpy(b + off, &v, 8); };
    put64(0, st.st_dev);
    put32(12, (uint32_t)st.st_ino);
    put32(16, st.st_mode);
    put32(20, (uint32_t)st.st_nlink);
    put32(24, st.st_uid);
    put32(28, st.st_gid);
    put64(32, st.st_rdev);
    put64(48, st.st_size);
    put32(56, (uint32_t)st.st_blksize);
    put64(64, st.st_blocks);
    put32(72, (uint32_t)st.st_atim.tv_sec); put32(76, (uint32_t)st.st_atim.tv_nsec);
    put32(80, (uint32_t)st.st_mtim.tv_sec); put32(84, (uint32_t)st.st_mtim.tv_nsec);
    put32(88, (uint32_t)st.st_ctim.tv_sec); put32(92, (uint32_t)st.st_ctim.tv_nsec);
    put64(96, st.st_ino);
    memcpy(g2h(a), b, sizeof b);
}

static void tm_from_guest(uint32_t a, struct tm* t) {
    memset(t, 0, sizeof *t);
    int32_t* g = (int32_t*)g2h(a);
    t->tm_sec = g[0]; t->tm_min = g[1]; t->tm_hour = g[2]; t->tm_mday = g[3]; t->tm_mon = g[4];
    t->tm_year = g[5]; t->tm_wday = g[6]; t->tm_yday = g[7]; t->tm_isdst = g[8];
}

static void tm_to_guest(uint32_t a, const struct tm* t) {
    int32_t* g = (int32_t*)g2h(a);
    g[0] = t->tm_sec; g[1] = t->tm_min; g[2] = t->tm_hour; g[3] = t->tm_mday; g[4] = t->tm_mon;
    g[5] = t->tm_year; g[6] = t->tm_wday; g[7] = t->tm_yday; g[8] = t->tm_isdst;
}

static uint32_t clamp_long(long v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return (uint32_t)INT32_MIN;
    return (uint32_t)(int32_t)v;
}

void hle_libc_register() {
    stdio_init();
    s_stack_guard = data_alloc(4, 4);
    wr32(s_stack_guard, (uint32_t)arc4random());
    s_locale_c = data_strdup("C");

    // ---- memory / strings
    R("__aeabi_memclr", { memset(g2h(c.arg(0)), 0, c.arg(1)); });
    R("__aeabi_memclr4", { memset(g2h(c.arg(0)), 0, c.arg(1)); });
    R("__aeabi_memclr8", { memset(g2h(c.arg(0)), 0, c.arg(1)); });
    R("__aeabi_memcpy", { memcpy(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("__aeabi_memcpy4", { memcpy(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("__aeabi_memcpy8", { memcpy(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("__aeabi_memmove", { memmove(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("__aeabi_memmove4", { memmove(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("__aeabi_memset", { memset(g2h(c.arg(0)), (int)c.arg(2), c.arg(1)); });
    R("__aeabi_memset4", { memset(g2h(c.arg(0)), (int)c.arg(2), c.arg(1)); });
    R("memcpy", { memcpy(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("memmove", { memmove(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2)); });
    R("memset", { memset(g2h(c.arg(0)), (int)c.arg(1), c.arg(2)); });
    R("memcmp", { c.ret((uint32_t)memcmp(g2h(c.arg(0)), g2h(c.arg(1)), c.arg(2))); });
    R("memchr", { c.ret(h2g(memchr(g2h(c.arg(0)), (int)c.arg(1), c.arg(2)))); });
    R("strlen", { c.ret((uint32_t)strlen(gstr(c.arg(0)))); });
    R("strcmp", { c.ret((uint32_t)strcmp(gstr(c.arg(0)), gstr(c.arg(1)))); });
    R("strncmp", { c.ret((uint32_t)strncmp(gstr(c.arg(0)), gstr(c.arg(1)), c.arg(2))); });
    R("strcpy", { strcpy((char*)g2h(c.arg(0)), gstr(c.arg(1))); });
    R("strncpy", { strncpy((char*)g2h(c.arg(0)), gstr(c.arg(1)), c.arg(2)); });
    R("strchr", { c.ret(h2g(strchr(gstr(c.arg(0)), (int)c.arg(1)))); });
    R("strrchr", { c.ret(h2g(strrchr(gstr(c.arg(0)), (int)c.arg(1)))); });
    R("strpbrk", { c.ret(h2g(strpbrk(gstr(c.arg(0)), gstr(c.arg(1))))); });
    R("strstr", { c.ret(h2g(strstr(gstr(c.arg(0)), gstr(c.arg(1))))); });
    R("strdup", { c.ret(gstrdup(gstr(c.arg(0)))); });
    R("wcslen", { c.ret((uint32_t)wcslen((const wchar_t*)g2h(c.arg(0)))); });
    R("wcscmp", { c.ret((uint32_t)wcscmp((const wchar_t*)g2h(c.arg(0)), (const wchar_t*)g2h(c.arg(1)))); });
    R("wcsncpy", { wcsncpy((wchar_t*)g2h(c.arg(0)), (const wchar_t*)g2h(c.arg(1)), c.arg(2)); });
    R("wmemcpy", { wmemcpy((wchar_t*)g2h(c.arg(0)), (const wchar_t*)g2h(c.arg(1)), c.arg(2)); });
    R("wmemmove", { wmemmove((wchar_t*)g2h(c.arg(0)), (const wchar_t*)g2h(c.arg(1)), c.arg(2)); });
    R("wmemset", { wmemset((wchar_t*)g2h(c.arg(0)), (wchar_t)c.arg(1), c.arg(2)); });

    // ---- heap
    R("malloc", { c.ret(gmalloc(c.arg(0))); });
    R("calloc", { c.ret(gcalloc(c.arg(0), c.arg(1))); });
    R("realloc", { c.ret(grealloc(c.arg(0), c.arg(1))); });
    R("free", { gfree(c.arg(0)); });
    R("memalign", { c.ret(gmemalign(c.arg(0), c.arg(1))); });

    // ---- ctype
    R("isalnum", { c.ret(isalnum((int)c.arg(0)) != 0); });
    R("isalpha", { c.ret(isalpha((int)c.arg(0)) != 0); });
    R("iscntrl", { c.ret(iscntrl((int)c.arg(0)) != 0); });
    R("islower", { c.ret(islower((int)c.arg(0)) != 0); });
    R("isprint", { c.ret(isprint((int)c.arg(0)) != 0); });
    R("ispunct", { c.ret(ispunct((int)c.arg(0)) != 0); });
    R("isspace", { c.ret(isspace((int)c.arg(0)) != 0); });
    R("isupper", { c.ret(isupper((int)c.arg(0)) != 0); });
    R("isdigit", { c.ret(isdigit((int)c.arg(0)) != 0); });
    R("isxdigit", { c.ret(isxdigit((int)c.arg(0)) != 0); });
    R("tolower", { c.ret((uint32_t)tolower((int)c.arg(0))); });
    R("toupper", { c.ret((uint32_t)toupper((int)c.arg(0))); });
    R("iswalpha", { c.ret(iswalpha(c.arg(0)) != 0); });
    R("iswcntrl", { c.ret(iswcntrl(c.arg(0)) != 0); });
    R("iswdigit", { c.ret(iswdigit(c.arg(0)) != 0); });
    R("iswlower", { c.ret(iswlower(c.arg(0)) != 0); });
    R("iswprint", { c.ret(iswprint(c.arg(0)) != 0); });
    R("iswpunct", { c.ret(iswpunct(c.arg(0)) != 0); });
    R("iswspace", { c.ret(iswspace(c.arg(0)) != 0); });
    R("iswupper", { c.ret(iswupper(c.arg(0)) != 0); });
    R("iswxdigit", { c.ret(iswxdigit(c.arg(0)) != 0); });
    R("towlower", { c.ret(towlower(c.arg(0))); });
    R("towupper", { c.ret(towupper(c.arg(0))); });

    // ---- conversions
    R("atoi", { c.ret((uint32_t)atoi(gstr(c.arg(0)))); });
    R("strtol", {
        char* end;
        const char* s = gstr(c.arg(0));
        long v = strtol(s, &end, (int)c.arg(2));
        if (c.arg(1)) wr32(c.arg(1), h2g(end));
        c.ret(clamp_long(v));
    });
    R("strtod", {
        char* end;
        double v = strtod(gstr(c.arg(0)), &end);
        if (c.arg(1)) wr32(c.arg(1), h2g(end));
        c.retd(v);
    });

    // ---- math (softfp: floats/doubles in core registers)
    R("sin", { c.retd(sin(c.argd(0))); });
    R("cos", { c.retd(cos(c.argd(0))); });
    R("tan", { c.retd(tan(c.argd(0))); });
    R("atan2", { c.retd(atan2(c.argd(0), c.argd(2))); });
    R("fmod", { c.retd(fmod(c.argd(0), c.argd(2))); });
    R("ldexp", { c.retd(ldexp(c.argd(0), (int)c.arg(2))); });
    R("sinf", { c.retf(sinf(c.argf(0))); });
    R("cosf", { c.retf(cosf(c.argf(0))); });
    R("asinf", { c.retf(asinf(c.argf(0))); });
    R("exp2f", { c.retf(exp2f(c.argf(0))); });
    R("floorf", { c.retf(floorf(c.argf(0))); });
    R("truncf", { c.retf(truncf(c.argf(0))); });
    R("sqrtf", { c.retf(sqrtf(c.argf(0))); });
    R("fmodf", { c.retf(fmodf(c.argf(0), c.argf(1))); });
    R("powf", { c.retf(powf(c.argf(0), c.argf(1))); });

    // ---- stdio
    R("fopen", {
        const char* path = gstr(c.arg(0));
        const char* mode = gstr(c.arg(1));
        FILE* f = fopen(path, mode);
        LOGI("fopen(\"%s\", \"%s\") -> %s", path, mode, f ? "ok" : strerror(errno));
        if (!f) { c.ret(0); return; }
        uint32_t ga = gcalloc(1, FILE_SIZE);
        std::lock_guard<std::mutex> g(s_file_lock);
        s_files[ga] = new GFile{f, false, {}};
        c.ret(ga);
    });
    R("fclose", {
        uint32_t ga = c.arg(0);
        GFile* f;
        {
            std::lock_guard<std::mutex> g(s_file_lock);
            auto it = s_files.find(ga);
            if (it == s_files.end() || ga - s_sF < FILE_SIZE * 3) { c.ret(0); return; }
            f = it->second;
            s_files.erase(it);
        }
        int r = fclose(f->f);
        delete f;
        gfree(ga);
        c.ret((uint32_t)r);
    });
    R("fread", {
        FILE* f = hostf(c.arg(3));
        c.ret(f ? (uint32_t)fread(g2h(c.arg(0)), c.arg(1), c.arg(2), f) : 0);
    });
    R("fwrite", {
        size_t n = (size_t)c.arg(1) * c.arg(2);
        size_t w = file_write(c.arg(3), g2h(c.arg(0)), n);
        c.ret(c.arg(1) ? (uint32_t)(w / c.arg(1)) : 0);
    });
    R("fputs", { const char* s = gstr(c.arg(0)); file_write(c.arg(1), s, strlen(s)); c.ret(1); });
    R("fputc", { char ch = (char)c.arg(0); file_write(c.arg(1), &ch, 1); c.ret(c.arg(0) & 0xff); });
    R("putc", { char ch = (char)c.arg(0); file_write(c.arg(1), &ch, 1); c.ret(c.arg(0) & 0xff); });
    R("putchar", { char ch = (char)c.arg(0); file_write(s_sF + FILE_SIZE, &ch, 1); c.ret(c.arg(0) & 0xff); });
    R("puts", { const char* s = gstr(c.arg(0)); file_write(s_sF + FILE_SIZE, s, strlen(s)); file_write(s_sF + FILE_SIZE, "\n", 1); c.ret(1); });
    R("getc", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)getc(f) : (uint32_t)EOF); });
    R("ungetc", { FILE* f = hostf(c.arg(1)); c.ret(f ? (uint32_t)ungetc((int)c.arg(0), f) : (uint32_t)EOF); });
    R("fflush", { FILE* f = c.arg(0) ? hostf(c.arg(0)) : nullptr; c.ret(f ? (uint32_t)fflush(f) : 0); });
    R("ferror", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)ferror(f) : 0); });
    R("feof", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)feof(f) : 0); });
    R("fseek", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)fseek(f, (int32_t)c.arg(1), (int)c.arg(2)) : (uint32_t)-1); });
    R("ftell", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)ftell(f) : (uint32_t)-1); });
    R("fgetpos", { FILE* f = hostf(c.arg(0)); if (!f) { c.ret((uint32_t)-1); return; } wr32(c.arg(1), (uint32_t)ftell(f)); c.ret(0); });
    R("fsetpos", { FILE* f = hostf(c.arg(0)); c.ret(f ? (uint32_t)fseek(f, (int32_t)rd32(c.arg(1)), SEEK_SET) : (uint32_t)-1); });
    R("setvbuf", { c.ret(0); });
    R("printf", {
        VarArgs va = VarArgs::regs(c, 1);
        std::string s = guest_format(gstr(c.arg(0)), va);
        file_write(s_sF + FILE_SIZE, s.data(), s.size());
        c.ret((uint32_t)s.size());
    });
    R("fprintf", {
        VarArgs va = VarArgs::regs(c, 2);
        std::string s = guest_format(gstr(c.arg(1)), va);
        c.ret((uint32_t)file_write(c.arg(0), s.data(), s.size()));
    });
    R("sprintf", {
        VarArgs va = VarArgs::regs(c, 2);
        std::string s = guest_format(gstr(c.arg(1)), va);
        memcpy(g2h(c.arg(0)), s.c_str(), s.size() + 1);
        c.ret((uint32_t)s.size());
    });
    R("snprintf", {
        VarArgs va = VarArgs::regs(c, 3);
        std::string s = guest_format(gstr(c.arg(2)), va);
        uint32_t n = c.arg(1);
        if (n) {
            size_t k = std::min<size_t>(s.size(), n - 1);
            memcpy(g2h(c.arg(0)), s.data(), k);
            ((char*)g2h(c.arg(0)))[k] = 0;
        }
        c.ret((uint32_t)s.size());
    });
    R("sscanf", {
        VarArgs va = VarArgs::regs(c, 2);
        c.ret((uint32_t)guest_scan(gstr(c.arg(0)), gstr(c.arg(1)), va, nullptr));
    });
    R("fscanf", {
        VarArgs va = VarArgs::regs(c, 2);
        FILE* f = hostf(c.arg(0));
        c.ret(f ? (uint32_t)guest_scan(nullptr, gstr(c.arg(1)), va, f) : (uint32_t)EOF);
    });
    R("__android_log_print", {
        VarArgs va = VarArgs::regs(c, 3);
        std::string s = guest_format(gstr(c.arg(2)), va);
        const char* tag = c.arg(1) ? gstr(c.arg(1)) : "guest";
        __android_log_print((int)c.arg(0), tag, "%s", s.c_str());
        c.ret(1);
    });
    R("__android_log_write", {
        __android_log_write((int)c.arg(0), c.arg(1) ? gstr(c.arg(1)) : "guest", gstr(c.arg(2)));
        c.ret(1);
    });

    // ---- POSIX io
    R("open", {
        const char* path = gstr(c.arg(0));
        int fd = open(path, (int)c.arg(1), (mode_t)c.arg(2));
        LOGI("open(\"%s\", 0x%x) -> %d", path, c.arg(1), fd);
        c.ret((uint32_t)fd);
    });
    R("close", { c.ret((uint32_t)close((int)c.arg(0))); });
    R("read", { c.ret((uint32_t)read((int)c.arg(0), g2h(c.arg(1)), c.arg(2))); });
    R("write", {
        int fd = (int)c.arg(0);
        if (fd == 1 || fd == 2) { file_write(s_sF + FILE_SIZE * fd, g2h(c.arg(1)), c.arg(2)); c.ret(c.arg(2)); return; }
        c.ret((uint32_t)write(fd, g2h(c.arg(1)), c.arg(2)));
    });
    R("lseek", { c.ret((uint32_t)lseek((int)c.arg(0), (int32_t)c.arg(1), (int)c.arg(2))); });
    R("fcntl", { c.ret((uint32_t)fcntl((int)c.arg(0), (int)c.arg(1), (long)c.arg(2))); });
    R("fstat", {
        struct stat st;
        int r = fstat((int)c.arg(0), &st);
        if (r == 0) write_guest_stat(c.arg(1), st);
        c.ret((uint32_t)r);
    });
    R("stat", {
        struct stat st;
        int r = stat(gstr(c.arg(0)), &st);
        if (r == 0) write_guest_stat(c.arg(1), st);
        c.ret((uint32_t)r);
    });
    R("mmap", {
        uint32_t len = c.arg(1), flags = c.arg(3);
        int fd = (int)c.arg(4);
        uint32_t off = c.arg(5);
        uint32_t a = gmemalign(4096, (len + 4095) & ~4095u);
        if (!a) { c.ret(0xffffffffu); return; }
        memset(g2h(a), 0, len);
        if (!(flags & MAP_ANONYMOUS) && fd >= 0) pread(fd, g2h(a), len, off);
        std::lock_guard<std::mutex> g(s_map_lock);
        s_maps[a] = len;
        c.ret(a);
    });
    R("munmap", {
        std::lock_guard<std::mutex> g(s_map_lock);
        auto it = s_maps.find(c.arg(0));
        if (it != s_maps.end()) { gfree(it->first); s_maps.erase(it); }
        c.ret(0);
    });
    R("sysconf", { c.ret(clamp_long(sysconf((int)c.arg(0)))); });

    // ---- time
    R("time", { time_t t = time(nullptr); if (c.arg(0)) wr32(c.arg(0), (uint32_t)t); c.ret((uint32_t)t); });
    R("clock", { c.ret((uint32_t)clock()); });
    R("clock_gettime", {
        struct timespec ts;
        int r = clock_gettime((clockid_t)c.arg(0), &ts);
        wr32(c.arg(1), (uint32_t)ts.tv_sec);
        wr32(c.arg(1) + 4, (uint32_t)ts.tv_nsec);
        c.ret((uint32_t)r);
    });
    R("gettimeofday", {
        struct timeval tv;
        int r = gettimeofday(&tv, nullptr);
        if (c.arg(0)) { wr32(c.arg(0), (uint32_t)tv.tv_sec); wr32(c.arg(0) + 4, (uint32_t)tv.tv_usec); }
        if (c.arg(1)) { wr32(c.arg(1), 0); wr32(c.arg(1) + 4, 0); }
        c.ret((uint32_t)r);
    });
    R("mktime", {
        struct tm t;
        tm_from_guest(c.arg(0), &t);
        time_t r = mktime(&t);
        tm_to_guest(c.arg(0), &t);
        c.ret((uint32_t)r);
    });

    // ---- pthread
    R("pthread_mutex_init", {
        uint32_t attr = c.arg(1);
        int t = attr ? (int)(rd32(attr) & 3) : 0;
        int type = t == 1 ? PTHREAD_MUTEX_RECURSIVE : t == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
        wr32(c.arg(0), MUTEX_TAG | new_mutex(type));
        c.ret(0);
    });
    R("pthread_mutex_destroy", {
        uint32_t v = rd32(c.arg(0));
        if ((v & TAG_MASK) == MUTEX_TAG) {
            uint32_t i = v & ~TAG_MASK;
            pthread_mutex_destroy(s_mutexes[i]);
            delete s_mutexes[i];
            s_mutexes[i] = nullptr;
            std::lock_guard<std::mutex> g(s_sync_lock);
            s_free_mutex.push_back(i);
        }
        wr32(c.arg(0), 0);
        c.ret(0);
    });
    R("pthread_mutex_lock", { c.ret((uint32_t)pthread_mutex_lock(get_mutex(c.arg(0)))); });
    R("pthread_mutex_trylock", { c.ret((uint32_t)pthread_mutex_trylock(get_mutex(c.arg(0)))); });
    R("pthread_mutex_unlock", { c.ret((uint32_t)pthread_mutex_unlock(get_mutex(c.arg(0)))); });
    R("pthread_cond_init", { wr32(c.arg(0), COND_TAG | new_cond()); c.ret(0); });
    R("pthread_cond_destroy", {
        uint32_t v = rd32(c.arg(0));
        if ((v & TAG_MASK) == COND_TAG) {
            uint32_t i = v & ~TAG_MASK;
            pthread_cond_destroy(s_conds[i]);
            delete s_conds[i];
            s_conds[i] = nullptr;
            std::lock_guard<std::mutex> g(s_sync_lock);
            s_free_cond.push_back(i);
        }
        wr32(c.arg(0), 0);
        c.ret(0);
    });
    R("pthread_cond_wait", { c.ret((uint32_t)pthread_cond_wait(get_cond(c.arg(0)), get_mutex(c.arg(1)))); });
    R("pthread_cond_signal", { c.ret((uint32_t)pthread_cond_signal(get_cond(c.arg(0)))); });
    R("pthread_cond_broadcast", { c.ret((uint32_t)pthread_cond_broadcast(get_cond(c.arg(0)))); });
    R("pthread_key_create", {
        uint32_t k = s_next_key++;
        if (k >= MAX_KEYS) { c.ret(EAGAIN); return; }
        wr32(c.arg(0), k);
        c.ret(0);
    });
    R("pthread_key_delete", { c.ret(0); });
    R("pthread_getspecific", { uint32_t k = c.arg(0); c.ret(k < MAX_KEYS ? t_specific[k] : 0); });
    R("pthread_setspecific", { uint32_t k = c.arg(0); if (k < MAX_KEYS) t_specific[k] = c.arg(1); c.ret(0); });
    R("pthread_self", { c.ret((uint32_t)gettid()); });

    // ---- dl
    R("dlopen", { LOGI("dlopen(\"%s\")", c.arg(0) ? gstr(c.arg(0)) : "(self)"); c.ret(0x1001); });
    R("dlclose", { c.ret(0); });
    R("dlerror", { c.ret(0); });
    R("dlsym", {
        const char* name = gstr(c.arg(1));
        uint32_t a = hle_thunk_for(name);
        if (!a) a = guest_sym(name);
        LOGI("dlsym(\"%s\") -> 0x%08x", name, a);
        c.ret(a);
    });

    // ---- runtime
    R("__gnu_Unwind_Find_exidx", {
        uint32_t pc = c.arg(0);
        if (pc >= g_image.base && pc < g_image.base + g_image.size) {
            wr32(c.arg(1), g_image.exidx_count);
            c.ret(g_image.exidx);
        } else {
            wr32(c.arg(1), 0);
            c.ret(0);
        }
    });
    R("__cxa_atexit", { c.ret(0); });
    R("__cxa_finalize", { });
    R("abort", { guest_fatal("abort()"); });
    R("exit", { LOGE("guest exit(%d)", (int)c.arg(0)); guest_fatal("exit()"); });
    R("__stack_chk_fail", { guest_fatal("stack corruption detected"); });
    R("__assert2", {
        LOGE("assert failed: %s:%d %s: %s", gstr(c.arg(0)), (int)c.arg(1), gstr(c.arg(2)), gstr(c.arg(3)));
        guest_fatal("assert");
    });
    R("raise", {
        int sig = (int)c.arg(0);
        LOGE("guest raise(%d)", sig);
        guest_backtrace("raise");
        if (sig != SIGFPE) abort();
        c.ret(0);
    });
    R("setlocale", { c.ret(s_locale_c); });
    R("lrand48", { c.ret((uint32_t)lrand48()); });
    R("srand48", { srand48((long)(int32_t)c.arg(0)); });
    R("rand", { c.ret((uint32_t)rand()); });
    R("srand", { srand(c.arg(0)); });
}
