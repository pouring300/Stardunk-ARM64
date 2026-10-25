#include "guest.h"

#include <elf.h>
#include <string>
#include <unordered_map>

GuestImage g_image;

static const Elf32_Sym* s_symtab;
static const char* s_strtab;
static uint32_t s_nsyms;
static std::unordered_map<std::string, uint32_t> s_import_thunks;
static uint32_t s_next_thunk = 1;

static void hle_unimplemented(Cpu& c);

static uint32_t import_thunk(const char* name) {
    auto it = s_import_thunks.find(name);
    if (it != s_import_thunks.end()) return it->second;
    uint32_t idx = s_next_thunk++;
    if (idx >= SVC_JNI_BASE) {
        LOGE("too many imports");
        abort();
    }
    Handler h = hle_lookup(name);
    const char* stable = strdup(name);
    if (!h) {
        LOGW("import '%s' has no HLE implementation", name);
        h = hle_unimplemented;
    }
    svc_register(idx, h, stable);
    uint32_t addr = THUNK_BASE + idx * 8;
    s_import_thunks[name] = addr;
    return addr;
}

// Thunk address for an HLE function by name (used by dlsym).
uint32_t hle_thunk_for(const char* name) {
    if (!hle_lookup(name)) return 0;
    return import_thunk(name);
}

static void hle_unimplemented(Cpu& c) {
    LOGE("call to unimplemented import (r0=%08x r1=%08x)", c.reg(0), c.reg(1));
    guest_backtrace("unimplemented import");
    abort();
}

static uint32_t resolve(uint32_t symidx, bool* is_data) {
    const Elf32_Sym& s = s_symtab[symidx];
    const char* name = s_strtab + s.st_name;
    *is_data = false;
    if (s.st_shndx != SHN_UNDEF) return g_image.base + s.st_value;
    if (uint32_t d = hle_data_symbol(name)) {
        *is_data = true;
        return d;
    }
    if (ELF32_ST_TYPE(s.st_info) == STT_OBJECT) {
        LOGE("unresolved data import '%s'", name);
        return 0;
    }
    return import_thunk(name);
}

uint32_t guest_sym(const char* name) {
    for (uint32_t i = 1; i < s_nsyms; i++) {
        const Elf32_Sym& s = s_symtab[i];
        if (s.st_shndx != SHN_UNDEF && strcmp(s_strtab + s.st_name, name) == 0) return g_image.base + s.st_value;
    }
    return 0;
}

bool load_guest(const uint8_t* elf, size_t len) {
    auto* eh = (const Elf32_Ehdr*)elf;
    if (len < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_ARM) {
        LOGE("guest image is not an ARM ELF");
        return false;
    }
    auto* ph = (const Elf32_Phdr*)(elf + eh->e_phoff);
    uint32_t base = IMAGE_BASE, hi = 0;
    const Elf32_Phdr* dynph = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            if (base + p.p_vaddr + p.p_memsz > IMAGE_END || p.p_offset + p.p_filesz > len) return false;
            memcpy(g2h(base + p.p_vaddr), elf + p.p_offset, p.p_filesz);
            memset(g2h(base + p.p_vaddr + p.p_filesz), 0, p.p_memsz - p.p_filesz);
            hi = std::max(hi, p.p_vaddr + p.p_memsz);
        } else if (p.p_type == PT_DYNAMIC) {
            dynph = &p;
        } else if (p.p_type == PT_ARM_EXIDX) {
            g_image.exidx = base + p.p_vaddr;
            g_image.exidx_count = p.p_memsz / 8;
        }
    }
    g_image.base = base;
    g_image.size = (hi + 0xfff) & ~0xfffu;
    if (!dynph) return false;

    // Dynamic section is read from the loaded image so pointers are image-relative.
    auto* dyn = (const Elf32_Dyn*)g2h(base + dynph->p_vaddr);
    uint32_t rel = 0, relsz = 0, jmprel = 0, jmprelsz = 0, hash = 0;
    uint32_t init_array = 0, init_arraysz = 0, init = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        uint32_t v = dyn->d_un.d_val;
        switch (dyn->d_tag) {
            case DT_REL: rel = v; break;
            case DT_RELSZ: relsz = v; break;
            case DT_JMPREL: jmprel = v; break;
            case DT_PLTRELSZ: jmprelsz = v; break;
            case DT_SYMTAB: s_symtab = (const Elf32_Sym*)g2h(base + v); break;
            case DT_STRTAB: s_strtab = (const char*)g2h(base + v); break;
            case DT_HASH: hash = v; break;
            case DT_INIT_ARRAY: init_array = v; break;
            case DT_INIT_ARRAYSZ: init_arraysz = v; break;
            case DT_INIT: init = v; break;
        }
    }
    if (!s_symtab || !s_strtab || !hash) return false;
    s_nsyms = rd32(base + hash + 4);

    int counts[4] = {};
    auto apply = [&](uint32_t off, uint32_t sz) {
        auto* r = (const Elf32_Rel*)g2h(base + off);
        for (uint32_t i = 0; i < sz / sizeof(Elf32_Rel); i++) {
            uint32_t where = base + r[i].r_offset;
            uint32_t type = ELF32_R_TYPE(r[i].r_info), sym = ELF32_R_SYM(r[i].r_info);
            bool is_data;
            switch (type) {
                case R_ARM_RELATIVE:
                    wr32(where, rd32(where) + base);
                    counts[0]++;
                    break;
                case R_ARM_ABS32:
                    wr32(where, rd32(where) + resolve(sym, &is_data));
                    counts[1]++;
                    break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
                    wr32(where, resolve(sym, &is_data));
                    counts[2]++;
                    break;
                default:
                    LOGE("unsupported relocation type %u at 0x%x", type, r[i].r_offset);
                    counts[3]++;
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, jmprelsz);
    LOGI("guest image at 0x%08x size 0x%x: %u syms, reloc rel=%d abs=%d slot=%d bad=%d, %u imports",
         base, g_image.size, s_nsyms, counts[0], counts[1], counts[2], counts[3], (unsigned)s_import_thunks.size());

    if (init) call_guest(base + init, {});
    for (uint32_t i = 0; i < init_arraysz / 4; i++) {
        uint32_t fn = rd32(base + init_array + i * 4);
        if (fn && fn != 0xffffffffu) call_guest(fn, {});
    }
    LOGI("ran %u guest constructors", init_arraysz / 4);
    return true;
}
