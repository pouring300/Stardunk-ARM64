#include "guest.h"

#include <array>
#include <memory>
#include <mutex>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"

using namespace Dynarmic;

constexpr size_t MAX_PROCESSORS = 128;

static std::array<uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> s_page_table;
static std::unique_ptr<ExclusiveMonitor> s_monitor;
static std::mutex s_alloc_lock;
static bool s_proc_used[MAX_PROCESSORS];
static bool s_stack_used[STACK_SLOTS];

struct SvcEntry {
    Handler fn = nullptr;
    const char* name = nullptr;
};
static SvcEntry s_svc[THUNK_COUNT];

void svc_register(uint32_t num, Handler h, const char* name) {
    if (num >= THUNK_COUNT) abort();
    s_svc[num] = {h, name};
}

static void log_bad_access(const char* what, uint32_t addr);

// CP15: TPIDRURO (c13,c0,3) returns the thread's TLS block; c7 barrier ops are no-ops.
class Cp15 final : public A32::Coprocessor {
public:
    uint32_t tls = 0;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C7) {
            return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { std::atomic_thread_fence(std::memory_order_seq_cst); return 0; }, std::nullopt};
        }
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 3) return &tls;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
};

struct JitBox;

class Callbacks final : public A32::UserCallbacks {
public:
    JitBox* box = nullptr;

    template <typename T>
    T rd(uint32_t a) {
        if (!mem_mapped(a) || !mem_mapped(a + sizeof(T) - 1)) { log_bad_access("read", a); return 0; }
        T v; memcpy(&v, g_mem + a, sizeof(T)); return v;
    }
    template <typename T>
    void wr(uint32_t a, T v) {
        if (!mem_mapped(a) || !mem_mapped(a + sizeof(T) - 1)) { log_bad_access("write", a); return; }
        memcpy(g_mem + a, &v, sizeof(T));
    }
    template <typename T>
    bool wrx(uint32_t a, T v, T expected) {
        if (!mem_mapped(a)) { log_bad_access("exclusive write", a); return false; }
        return __atomic_compare_exchange_n((T*)(g_mem + a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    std::optional<uint32_t> MemoryReadCode(A32::VAddr a) override {
        if (!mem_mapped(a)) { log_bad_access("execute", a); return std::nullopt; }
        uint32_t v; memcpy(&v, g_mem + a, 4); return v;
    }
    uint8_t MemoryRead8(A32::VAddr a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(A32::VAddr a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(A32::VAddr a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(A32::VAddr a) override { return rd<uint64_t>(a); }
    void MemoryWrite8(A32::VAddr a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(A32::VAddr a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(A32::VAddr a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(A32::VAddr a, uint64_t v) override { wr(a, v); }
    bool MemoryWriteExclusive8(A32::VAddr a, uint8_t v, uint8_t e) override { return wrx(a, v, e); }
    bool MemoryWriteExclusive16(A32::VAddr a, uint16_t v, uint16_t e) override { return wrx(a, v, e); }
    bool MemoryWriteExclusive32(A32::VAddr a, uint32_t v, uint32_t e) override { return wrx(a, v, e); }
    bool MemoryWriteExclusive64(A32::VAddr a, uint64_t v, uint64_t e) override { return wrx(a, v, e); }

    void InterpreterFallback(A32::VAddr pc, size_t n) override {
        LOGE("InterpreterFallback pc=0x%08x n=%zu", pc, n);
        guest_backtrace("interpreter fallback");
        abort();
    }
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(A32::VAddr pc, A32::Exception ex) override;
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return UINT64_MAX; }
};

struct JitBox {
    Callbacks cb;
    std::shared_ptr<Cp15> cp15 = std::make_shared<Cp15>();
    std::unique_ptr<A32::Jit> jit;
    size_t proc_id = 0;
    Cpu cpu{};
};

struct ThreadCtx {
    int slot = -1;
    uint32_t stack_top = 0;
    uint32_t tls = 0;
    int depth = 0;
    std::vector<JitBox*> jits;
    JitBox* current = nullptr;
    ~ThreadCtx();
};

static thread_local ThreadCtx t_ctx;

static size_t alloc_processor() {
    std::lock_guard<std::mutex> g(s_alloc_lock);
    for (size_t i = 0; i < MAX_PROCESSORS; i++)
        if (!s_proc_used[i]) { s_proc_used[i] = true; return i; }
    LOGE("out of Dynarmic processor slots");
    abort();
}

ThreadCtx::~ThreadCtx() {
    std::lock_guard<std::mutex> g(s_alloc_lock);
    for (JitBox* b : jits) {
        s_monitor->ClearProcessor(b->proc_id);
        s_proc_used[b->proc_id] = false;
        delete b;
    }
    if (slot >= 0) s_stack_used[slot] = false;
}

static JitBox* make_jit(uint32_t tls) {
    auto* b = new JitBox();
    b->cb.box = b;
    b->cp15->tls = tls;
    b->proc_id = alloc_processor();
    A32::UserConfig conf;
    conf.callbacks = &b->cb;
    conf.processor_id = b->proc_id;
    conf.global_monitor = s_monitor.get();
    conf.arch_version = A32::ArchVersion::v7;
    conf.page_table = &s_page_table;
    conf.coprocessors[15] = b->cp15;
    conf.define_unpredictable_behaviour = true;
    conf.always_little_endian = true;
    conf.enable_cycle_counting = false;
    conf.code_cache_size = 32 * 1024 * 1024;
    b->jit = std::make_unique<A32::Jit>(conf);
    b->cpu.jit = b->jit.get();
    return b;
}

static ThreadCtx& thread_ctx() {
    ThreadCtx& t = t_ctx;
    if (t.slot < 0) {
        std::lock_guard<std::mutex> g(s_alloc_lock);
        for (uint32_t i = 0; i < STACK_SLOTS; i++)
            if (!s_stack_used[i]) { s_stack_used[i] = true; t.slot = (int)i; break; }
        if (t.slot < 0) { LOGE("out of guest stacks"); abort(); }
        t.stack_top = STACK_BASE + STACK_SIZE * (t.slot + 1) - 64;
        // Small bionic-like TLS block at the bottom of the stack slot.
        t.tls = STACK_BASE + STACK_SIZE * t.slot;
        memset(g2h(t.tls), 0, 256);
        wr32(t.tls, t.tls);
    }
    return t;
}

static A32::Jit* J(void* p) { return static_cast<A32::Jit*>(p); }

uint32_t& Cpu::reg(int i) { return J(jit)->Regs()[i]; }
uint32_t Cpu::arg(int i) { return i < 4 ? J(jit)->Regs()[i] : rd32(J(jit)->Regs()[13] + 4 * (i - 4)); }

VarArgs VarArgs::regs(Cpu& c, int first) {
    VarArgs v;
    v.cpu = &c;
    v.ncrn = first;
    v.nsaa = c.reg(13);
    return v;
}

uint32_t VarArgs::next32() {
    if (va) { uint32_t r = rd32(va); va += 4; return r; }
    if (ncrn < 4) return cpu->reg(ncrn++);
    uint32_t r = rd32(nsaa); nsaa += 4; return r;
}

uint64_t VarArgs::next64() {
    if (va) {
        va = (va + 7) & ~7u;
        uint64_t r = rd32(va) | ((uint64_t)rd32(va + 4) << 32);
        va += 8;
        return r;
    }
    if (ncrn < 4) {
        ncrn = (ncrn + 1) & ~1;
        if (ncrn <= 2) {
            uint64_t r = cpu->reg(ncrn) | ((uint64_t)cpu->reg(ncrn + 1) << 32);
            ncrn += 2;
            return r;
        }
        ncrn = 4;
    }
    nsaa = (nsaa + 7) & ~7u;
    uint64_t r = rd32(nsaa) | ((uint64_t)rd32(nsaa + 4) << 32);
    nsaa += 8;
    return r;
}

static void dump_regs(A32::Jit* j) {
    const auto& r = j->Regs();
    LOGE("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    LOGE("  r8=%08x r9=%08x r10=%08x r11=%08x ip=%08x sp=%08x lr=%08x pc=%08x cpsr=%08x", r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], j->Cpsr());
}

static bool in_image(uint32_t a) { return a >= g_image.base && a < g_image.base + g_image.size; }

void guest_backtrace(const char* why) {
    ThreadCtx& t = t_ctx;
    LOGE("=== guest backtrace (%s), depth %d ===", why, t.depth);
    if (!t.current) return;
    A32::Jit* j = J(t.current->cpu.jit);
    dump_regs(j);
    const auto& r = j->Regs();
    if (in_image(r[14])) LOGE("  lr  = libGame+0x%x", r[14] - g_image.base);
    uint32_t sp = r[13];
    int found = 0;
    for (uint32_t a = sp; a < sp + 4096 && found < 24 && mem_mapped(a); a += 4) {
        uint32_t v = rd32(a);
        if (in_image(v & ~1u) && (v - g_image.base) < 0x107f30) {
            LOGE("  [sp+0x%03x] libGame+0x%x", a - sp, v - g_image.base);
            found++;
        }
    }
}

static void log_bad_access(const char* what, uint32_t addr) {
    static std::atomic<int> count{0};
    int n = count++;
    if (n < 32) {
        LOGE("guest %s fault at 0x%08x", what, addr);
        guest_backtrace("bad access");
    }
    if (n == 2000) {
        LOGE("too many guest faults, aborting");
        abort();
    }
}

void Callbacks::CallSVC(uint32_t swi) {
    if (swi == SVC_RETURN) {
        box->jit->HaltExecution(HaltReason::UserDefined1);
        return;
    }
    if (swi < THUNK_COUNT && s_svc[swi].fn) {
        LOGT("svc %s", s_svc[swi].name);
        s_svc[swi].fn(box->cpu);
        return;
    }
    LOGE("unhandled svc 0x%x", swi);
    guest_backtrace("unhandled svc");
    abort();
}

void Callbacks::ExceptionRaised(A32::VAddr pc, A32::Exception ex) {
    LOGE("guest exception %d at pc=0x%08x (libGame+0x%x)", (int)ex, pc, pc - g_image.base);
    guest_backtrace("exception");
    abort();
}

bool cpu_init() {
    s_page_table.fill(nullptr);
    struct { uint32_t lo, hi; } maps[] = {
        {THUNK_BASE, DATA_END}, {IMAGE_BASE, IMAGE_END},
        {STACK_BASE, STACK_BASE + STACK_SIZE * STACK_SLOTS}, {HEAP_BASE, HEAP_END},
        {COMMPAGE_BASE, COMMPAGE_BASE + 0x2000},
    };
    for (auto& m : maps)
        for (uint64_t p = m.lo >> 12; p < ((uint64_t)m.hi >> 12); p++)
            s_page_table[p] = g_mem + (p << 12);
    s_monitor = std::make_unique<ExclusiveMonitor>(MAX_PROCESSORS);

    // Thunks: svc #n ; bx lr
    for (uint32_t i = 0; i < THUNK_COUNT; i++) {
        wr32(THUNK_BASE + i * 8, 0xef000000u | i);
        wr32(THUNK_BASE + i * 8 + 4, 0xe12fff1eu);
    }
    return true;
}

uint64_t call_guest(uint32_t fn, std::initializer_list<uint32_t> args) {
    ThreadCtx& t = thread_ctx();
    if ((size_t)t.depth >= t.jits.size()) t.jits.push_back(make_jit(t.tls));
    JitBox* box = t.jits[t.depth];
    A32::Jit* j = box->jit.get();

    uint32_t sp = t.depth == 0 ? t.stack_top : J(t.current->cpu.jit)->Regs()[13] - 128;
    sp &= ~7u;
    size_t n = args.size();
    if (n > 4) {
        sp -= (uint32_t)((n - 4) * 4);
        sp &= ~7u;
    }
    auto& r = j->Regs();
    size_t i = 0;
    for (uint32_t a : args) {
        if (i < 4) r[i] = a;
        else wr32(sp + 4 * (uint32_t)(i - 4), a);
        i++;
    }
    for (; i < 4; i++) r[i] = 0;
    r[13] = sp;
    r[14] = THUNK_BASE + SVC_RETURN * 8;
    r[15] = fn & ~1u;
    j->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));
    j->SetFpscr(0);

    JitBox* prev = t.current;
    t.current = box;
    t.depth++;
    j->ClearHalt(HaltReason::UserDefined1);
    HaltReason hr = j->Run();
    t.depth--;
    t.current = prev;
    if (hr != HaltReason::UserDefined1) {
        LOGE("guest call 0x%08x halted unexpectedly: 0x%x", fn, (unsigned)hr);
        dump_regs(j);
    }
    return (uint64_t)r[0] | ((uint64_t)r[1] << 32);
}
