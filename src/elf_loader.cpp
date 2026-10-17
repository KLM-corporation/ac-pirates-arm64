#include "elf_loader.h"

#include <elf.h>
#include <sys/mman.h>

#include <algorithm>
#include <mutex>

#include "cpu.h"
#include "mem.h"

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif

namespace zb::elf {
namespace {

std::mutex g_lock;
std::vector<Module*> g_modules;
std::vector<PatchHook> g_patch_hooks;

std::string symbolize_impl(gaddr a) {
    Module* m = module_for_addr(a);
    char buf[64];
    if (!m) {
        snprintf(buf, sizeof(buf), "0x%08x", a);
        return buf;
    }
    auto& f = m->sorted_funcs;
    auto it = std::upper_bound(f.begin(), f.end(), a,
                               [](gaddr v, const std::pair<gaddr, std::string>& e) { return v < e.first; });
    if (it == f.begin()) {
        snprintf(buf, sizeof(buf), "%s+0x%x", m->name.c_str(), a - m->bias);
        return buf;
    }
    --it;
    snprintf(buf, sizeof(buf), "+0x%x (%s+0x%x)", a - it->first, m->name.c_str(), a - m->bias);
    return it->second + buf;
}

}  // namespace

void add_patch_hook(PatchHook hook) { g_patch_hooks.push_back(hook); }

Module* load(const std::string& name, const u8* data, size_t size, Resolver resolve) {
    if (size < sizeof(Elf32_Ehdr)) fatal("%s: file too small", name.c_str());
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        fatal("%s: not an ARM32 shared object", name.c_str());
    }
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(data + eh->e_phoff);

    u32 min_v = 0xFFFFFFFFu, max_v = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        min_v = std::min(min_v, ph[i].p_vaddr & ~0xFFFu);
        max_v = std::max(max_v, ph[i].p_vaddr + ph[i].p_memsz);
    }
    u32 span = max_v - min_v;
    std::string tag = "zb:" + name;
    gaddr base = mem::map_anon(span, PROT_READ | PROT_WRITE, tag.c_str());
    if (!base) fatal("%s: cannot map %u bytes", name.c_str(), span);

    auto* m = new Module();
    m->name = name;
    m->bias = base - min_v;
    m->lo = base;
    m->hi = base + span;

    gaddr dyn = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            if (p.p_offset + p.p_filesz > size) fatal("%s: truncated segment", name.c_str());
            memcpy(hp(m->bias + p.p_vaddr), data + p.p_offset, p.p_filesz);
            if (!(p.p_flags & PF_W)) {
                m->rx_lo = m->bias + p.p_vaddr;
                m->rx_hi = m->bias + p.p_vaddr + p.p_memsz;
            }
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = m->bias + p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            m->exidx = m->bias + p.p_vaddr;
            m->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dyn) fatal("%s: no dynamic section", name.c_str());

    gaddr symtab = 0, strtab = 0, hash = 0, gnu_hash = 0, rel = 0, jmprel = 0, init_array = 0;
    u32 relsz = 0, pltrelsz = 0, init_arraysz = 0;
    std::vector<u32> needed;
    for (gaddr d = dyn;; d += sizeof(Elf32_Dyn)) {
        auto tag32 = rd<i32>(d);
        u32 val = rd<u32>(d + 4);
        if (tag32 == DT_NULL) break;
        switch (tag32) {
        case DT_SYMTAB: symtab = m->bias + val; break;
        case DT_STRTAB: strtab = m->bias + val; break;
        case DT_HASH: hash = m->bias + val; break;
        case DT_GNU_HASH: gnu_hash = m->bias + val; break;
        case DT_REL: rel = m->bias + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = m->bias + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT: m->init = m->bias + val; break;
        case DT_INIT_ARRAY: init_array = m->bias + val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = val; break;
        case DT_NEEDED: needed.push_back(val); break;
        case DT_RELA:
        case DT_TEXTREL: fatal("%s: unsupported dynamic tag %d", name.c_str(), tag32);
        default: break;
        }
    }
    if (!symtab || !strtab) fatal("%s: missing symbol tables", name.c_str());

    u32 nsyms = 0;
    if (hash) {
        nsyms = rd<u32>(hash + 4);  // nchain
    } else if (gnu_hash) {
        u32 nbuckets = rd<u32>(gnu_hash), symoffset = rd<u32>(gnu_hash + 4), bloom = rd<u32>(gnu_hash + 8);
        gaddr buckets = gnu_hash + 16 + bloom * 4;
        gaddr chains = buckets + nbuckets * 4;
        u32 last = 0;
        for (u32 b = 0; b < nbuckets; ++b) last = std::max(last, rd<u32>(buckets + 4 * b));
        if (last >= symoffset) {
            while (!(rd<u32>(chains + 4 * (last - symoffset)) & 1)) last++;
            nsyms = last + 1;
        } else {
            nsyms = symoffset;
        }
    }
    auto sym_at = [&](u32 i) { return hp<const Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    auto str_at = [&](u32 off) { return hp<const char>(strtab + off); };

    for (u32 i = 1; i < nsyms; ++i) {
        const Elf32_Sym* s = sym_at(i);
        unsigned bind = ELF32_ST_BIND(s->st_info), type = ELF32_ST_TYPE(s->st_info);
        if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK)) continue;
        std::string sname = str_at(s->st_name);
        gaddr v = m->bias + s->st_value;
        m->exports.emplace(sname, v);
        if (type == STT_FUNC) m->sorted_funcs.emplace_back(v & ~1u, sname);
    }
    std::sort(m->sorted_funcs.begin(), m->sorted_funcs.end());

    std::string dep_list;
    for (u32 off : needed) dep_list += std::string(str_at(off)) + " ";
    ZLOGI("%s: loaded at 0x%08x-0x%08x (bias 0x%08x), %u symbols, needs: %s", name.c_str(), m->lo, m->hi, m->bias,
          nsyms, dep_list.c_str());

    std::unordered_map<u32, gaddr> resolved;
    auto resolve_sym = [&](u32 si, bool is_func) -> gaddr {
        if (auto it = resolved.find(si); it != resolved.end()) return it->second;
        const Elf32_Sym* s = sym_at(si);
        gaddr v;
        if (s->st_shndx != SHN_UNDEF) {
            v = m->bias + s->st_value;
        } else {
            std::string sname = str_at(s->st_name);
            bool func = is_func || ELF32_ST_TYPE(s->st_info) == STT_FUNC;
            v = resolve(sname, func);
            if (!v) {
                if (ELF32_ST_BIND(s->st_info) == STB_WEAK) {
                    v = 0;
                } else if (func || ELF32_ST_TYPE(s->st_info) == STT_NOTYPE) {
                    v = cpu::missing_import_stub(sname);
                    m->missing.push_back(sname);
                } else {
                    ZLOGE("%s: unresolved data symbol %s", name.c_str(), sname.c_str());
                    v = mem::gcalloc(1, 256);
                    m->missing.push_back(sname);
                }
            }
        }
        resolved.emplace(si, v);
        return v;
    };

    auto apply = [&](gaddr table, u32 bytes) {
        for (gaddr r = table; r < table + bytes; r += sizeof(Elf32_Rel)) {
            u32 off = rd<u32>(r), info = rd<u32>(r + 4);
            u32 type = ELF32_R_TYPE(info), si = ELF32_R_SYM(info);
            gaddr where = m->bias + off;
            switch (type) {
            case R_ARM_RELATIVE: wr<u32>(where, rd<u32>(where) + m->bias); break;
            case R_ARM_ABS32: wr<u32>(where, rd<u32>(where) + (si ? resolve_sym(si, false) : m->bias)); break;
            case R_ARM_GLOB_DAT: wr<u32>(where, resolve_sym(si, false)); break;
            case R_ARM_JUMP_SLOT: wr<u32>(where, resolve_sym(si, true)); break;
            case 0: break;
            default: fatal("%s: unsupported relocation type %u at 0x%x", name.c_str(), type, off);
            }
        }
    };
    if (rel) apply(rel, relsz);
    if (jmprel) apply(jmprel, pltrelsz);

    if (!m->missing.empty()) {
        std::string list;
        for (auto& s : m->missing) list += s + " ";
        ZLOGW("%s: %zu imports without implementation: %s", name.c_str(), m->missing.size(), list.c_str());
    }

    for (u32 i = 0; i < init_arraysz / 4; ++i) m->init_array.push_back(rd<u32>(init_array + 4 * i));

    for (PatchHook h : g_patch_hooks) h(m);

    // Code and read-only data never change from here on.
    if (m->rx_hi > m->rx_lo) {
        u32 page = mem::host_page_size();
        gaddr plo = (m->rx_lo + page - 1) & ~(page - 1);
        gaddr phi = m->rx_hi & ~(page - 1);
        if (phi > plo) mprotect(hp(plo), phi - plo, PROT_READ);
        mem::add_readonly_range(m->rx_lo, m->rx_hi);
    }

    {
        std::lock_guard lk(g_lock);
        g_modules.push_back(m);
    }
    cpu::set_symbolizer(&symbolize_impl);
    return m;
}

void run_initializers(Module* m) {
    if (m->init) cpu::call(m->init);
    size_t n = 0;
    for (gaddr f : m->init_array) {
        if (f == 0 || f == 0xFFFFFFFFu) continue;
        ZLOGD("%s: init_array[%zu] -> %s", m->name.c_str(), n, cpu::symbolize(f & ~1u).c_str());
        cpu::call(f);
        n++;
    }
    ZLOGI("%s: ran %zu constructors", m->name.c_str(), n);
}

Module* find_module(const std::string& name) {
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        if (m->name == name) return m;
    }
    return nullptr;
}

Module* module_for_addr(gaddr a) {
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        if (a >= m->lo && a < m->hi) return m;
    }
    return nullptr;
}

gaddr lookup(const std::string& name) {
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        if (auto it = m->exports.find(name); it != m->exports.end()) return it->second;
    }
    return 0;
}

}  // namespace zb::elf
