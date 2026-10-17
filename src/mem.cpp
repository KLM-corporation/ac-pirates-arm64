#include "mem.h"

#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <malloc.h>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern "C" {
void* create_mspace(size_t capacity, int locked);
void* mspace_malloc(void* msp, size_t bytes);
void mspace_free(void* msp, void* mem);
void* mspace_realloc(void* msp, void* mem, size_t newsize);
void* mspace_calloc(void* msp, size_t n_elements, size_t elem_size);
void* mspace_memalign(void* msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
struct mallinfo mspace_mallinfo(void* msp);
void* zb_dl_mmap(size_t n);
int zb_dl_munmap(void* p, size_t n);
}

namespace zb::mem {
namespace {

// Lowest and highest guest addresses we hand out. The first MiB stays unmapped
// so null-pointer dereferences fault; the top page is kept for the ARM kernel
// helper page (0xffff0000).
constexpr u64 kLow = 0x00100000;
constexpr u64 kHigh = 0xFFFF0000;

PageTable* g_pt = nullptr;
u32 g_host_page = 4096;
std::mutex g_lock;
std::map<u64, u64> g_free;  // start -> end (exclusive); a hint, verified by MAP_FIXED_NOREPLACE
std::vector<std::pair<gaddr, gaddr>> g_ro;
void* g_msp = nullptr;

u64 round_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

void set_pages(u64 a, u64 len, bool present) {
    u64 first = a >> kGuestPageBits;
    u64 last = (a + len + kGuestPageSize - 1) >> kGuestPageBits;
    for (u64 p = first; p < last && p < g_pt->size(); ++p) {
        (*g_pt)[p] = present ? reinterpret_cast<u8*>(static_cast<uintptr_t>(p << kGuestPageBits)) : nullptr;
    }
}

void rebuild_free_locked() {
    std::vector<std::pair<u64, u64>> used;
    if (FILE* f = fopen("/proc/self/maps", "re")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long s, e;
            if (sscanf(line, "%llx-%llx", &s, &e) == 2 && s < 0x100000000ull) used.emplace_back(s, e);
        }
        fclose(f);
    }
    std::sort(used.begin(), used.end());
    g_free.clear();
    u64 cursor = kLow;
    for (auto [s, e] : used) {
        if (e <= cursor) continue;
        if (s >= kHigh) break;
        if (s > cursor) g_free[cursor] = std::min<u64>(s, kHigh);
        cursor = std::max(cursor, e);
    }
    if (cursor < kHigh) g_free[cursor] = kHigh;
}

void carve_locked(u64 a, u64 len) {
    u64 end = a + len;
    auto it = g_free.upper_bound(a);
    if (it != g_free.begin()) --it;
    while (it != g_free.end() && it->first < end) {
        u64 s = it->first, e = it->second;
        if (e <= a) {
            ++it;
            continue;
        }
        it = g_free.erase(it);
        if (s < a) g_free[s] = a;
        if (e > end) g_free[end] = e;
    }
}

void release_locked(u64 a, u64 len) {
    u64 s = a, e = a + len;
    auto next = g_free.lower_bound(s);
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->second >= s) {
            s = prev->first;
            e = std::max(e, prev->second);
            g_free.erase(prev);
        }
    }
    next = g_free.lower_bound(s);
    while (next != g_free.end() && next->first <= e) {
        e = std::max(e, next->second);
        next = g_free.erase(next);
    }
    g_free[s] = e;
}

// Finds free guest VA and maps it. Prefers high addresses: ART's own low-4GB
// allocator (large objects, compressed-reference spaces) grows from the bottom.
gaddr alloc_locked(u64 size, int prot, int flags, int fd, i64 off, int* err) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool stale = false;
        for (auto it = g_free.rbegin(); it != g_free.rend(); ++it) {
            u64 s = it->first, e = it->second;
            if (e - s < size) continue;
            u64 cand = (e - size) & ~static_cast<u64>(g_host_page - 1);
            if (cand < s) continue;
            void* r = mmap(reinterpret_cast<void*>(cand), size, prot, flags | MAP_FIXED_NOREPLACE, fd, off);
            if (r == MAP_FAILED) {
                if (errno == EEXIST) {
                    stale = true;
                    break;
                }
                if (err) *err = errno;
                return 0;
            }
            if (reinterpret_cast<u64>(r) != cand) {
                munmap(r, size);
                stale = true;
                break;
            }
            carve_locked(cand, size);
            return static_cast<gaddr>(cand);
        }
        if (!stale && attempt > 0) break;
        rebuild_free_locked();
    }
    if (err) *err = ENOMEM;
    return 0;
}

void name_vma(gaddr a, u64 len, const char* tag) {
    if (tag) prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, static_cast<uintptr_t>(a), len, tag);
}

}  // namespace

void init() {
    g_host_page = static_cast<u32>(sysconf(_SC_PAGESIZE));
    void* pt = mmap(nullptr, sizeof(PageTable), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pt == MAP_FAILED) fatal("cannot allocate the guest page table");
    g_pt = static_cast<PageTable*>(pt);
    {
        std::lock_guard lk(g_lock);
        rebuild_free_locked();
        u64 total = 0, largest = 0;
        for (auto [s, e] : g_free) {
            total += e - s;
            largest = std::max(largest, e - s);
        }
        ZLOGI("guest VA: host page %u, %llu MiB free below 4GiB, largest hole %llu MiB", g_host_page,
              (unsigned long long)(total >> 20), (unsigned long long)(largest >> 20));
    }
    g_msp = create_mspace(0, 1);
    if (!g_msp) fatal("cannot create the guest heap");
}

PageTable* page_table() { return g_pt; }
u32 host_page_size() { return g_host_page; }

gaddr map_anon(u32 size, int prot, const char* tag) {
    u64 len = round_up(size, g_host_page);
    std::lock_guard lk(g_lock);
    gaddr a = alloc_locked(len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0, nullptr);
    if (!a) return 0;
    set_pages(a, len, prot != PROT_NONE);
    name_vma(a, len, tag);
    return a;
}

void unmap_anon(gaddr addr, u32 size) {
    u64 len = round_up(size, g_host_page);
    std::lock_guard lk(g_lock);
    set_pages(addr, len, false);
    munmap(hp(addr), len);
    release_locked(addr, len);
}

gaddr guest_mmap(gaddr addr, u32 len32, int prot, int flags, int fd, i64 offset, int* err) {
    if (len32 == 0) {
        *err = EINVAL;
        return 0xFFFFFFFFu;
    }
    u64 len = round_up(len32, g_host_page);
    if (offset % g_host_page) {
        // 4 KiB-aligned offsets are legal for the guest but not on a 16 KiB-page host.
        ZLOGW("guest mmap: offset 0x%llx not aligned to host page size", (long long)offset);
        *err = EINVAL;
        return 0xFFFFFFFFu;
    }
    std::lock_guard lk(g_lock);
    if (flags & MAP_FIXED) {
        if (addr % g_host_page || static_cast<u64>(addr) + len > kHigh || addr < kLow) {
            *err = EINVAL;
            return 0xFFFFFFFFu;
        }
        // Only allow replacing memory that already belongs to the guest.
        for (u64 p = addr; p < addr + len; p += kGuestPageSize) {
            if (!(*g_pt)[p >> kGuestPageBits]) {
                // Might be free VA: claim it without clobbering host mappings.
                void* r = mmap(hp(addr), len, prot, (flags & ~MAP_FIXED) | MAP_FIXED_NOREPLACE, fd, offset);
                if (r == MAP_FAILED) {
                    *err = errno == EEXIST ? ENOMEM : errno;
                    return 0xFFFFFFFFu;
                }
                carve_locked(addr, len);
                set_pages(addr, len, prot != PROT_NONE);
                return addr;
            }
        }
        void* r = mmap(hp(addr), len, prot, flags, fd, offset);
        if (r == MAP_FAILED) {
            *err = errno;
            return 0xFFFFFFFFu;
        }
        set_pages(addr, len, prot != PROT_NONE);
        return addr;
    }
    // Honor the hint when that range is free, like the kernel does. LuaJIT
    // depends on it to place machine code within branch range of its VM.
    if (addr && addr % g_host_page == 0 && addr >= kLow && static_cast<u64>(addr) + len <= kHigh) {
        void* r = mmap(hp(addr), len, prot, (flags & ~MAP_FIXED) | MAP_FIXED_NOREPLACE, fd, offset);
        if (r != MAP_FAILED) {
            if (reinterpret_cast<u64>(r) == addr) {
                carve_locked(addr, len);
                set_pages(addr, len, prot != PROT_NONE);
                return addr;
            }
            munmap(r, len);
        }
    }
    gaddr a = alloc_locked(len, prot, flags & ~MAP_FIXED, fd, offset, err);
    if (!a) return 0xFFFFFFFFu;
    set_pages(a, len, prot != PROT_NONE);
    return a;
}

int guest_munmap(gaddr addr, u32 len32, int* err) {
    if (addr % g_host_page || len32 == 0) {
        *err = EINVAL;
        return -1;
    }
    u64 len = round_up(len32, g_host_page);
    std::lock_guard lk(g_lock);
    set_pages(addr, len, false);
    if (munmap(hp(addr), len) != 0) {
        *err = errno;
        return -1;
    }
    release_locked(addr, len);
    return 0;
}

int guest_mprotect(gaddr addr, u32 len32, int prot, int* err) {
    u64 len = round_up(len32, g_host_page);
    std::lock_guard lk(g_lock);
    if (mprotect(hp(addr), len, prot) != 0) {
        *err = errno;
        return -1;
    }
    set_pages(addr, len, prot != PROT_NONE);
    return 0;
}

gaddr guest_mremap(gaddr old_addr, u32 old_len32, u32 new_len32, int flags, int* err) {
    u64 old_len = round_up(old_len32, g_host_page);
    u64 new_len = round_up(new_len32, g_host_page);
    std::lock_guard lk(g_lock);
    if (new_len <= old_len) {
        if (new_len < old_len) {
            set_pages(old_addr + new_len, old_len - new_len, false);
            munmap(hp(old_addr + new_len), old_len - new_len);
            release_locked(old_addr + new_len, old_len - new_len);
        }
        return old_addr;
    }
    if (static_cast<u64>(old_addr) + new_len <= kHigh) {
        void* r = mremap(hp(old_addr), old_len, new_len, 0);
        if (r != MAP_FAILED) {
            carve_locked(old_addr + old_len, new_len - old_len);
            set_pages(old_addr, new_len, true);
            return old_addr;
        }
    }
    if (!(flags & 1 /* MREMAP_MAYMOVE */)) {
        *err = ENOMEM;
        return 0xFFFFFFFFu;
    }
    gaddr dst = alloc_locked(new_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0, err);
    if (!dst) return 0xFFFFFFFFu;
    void* r = mremap(hp(old_addr), old_len, new_len, MREMAP_MAYMOVE | MREMAP_FIXED, hp(dst));
    if (r == MAP_FAILED) {
        *err = errno;
        munmap(hp(dst), new_len);
        release_locked(dst, new_len);
        return 0xFFFFFFFFu;
    }
    set_pages(old_addr, old_len, false);
    release_locked(old_addr, old_len);
    set_pages(dst, new_len, true);
    return dst;
}

bool is_mapped(gaddr a) { return (*g_pt)[a >> kGuestPageBits] != nullptr; }

bool is_range_mapped(gaddr a, u32 len) {
    if (len == 0) return true;
    u64 end = static_cast<u64>(a) + len;
    for (u64 p = a & ~(kGuestPageSize - 1); p < end; p += kGuestPageSize) {
        if (p > 0xFFFFFFFFull || !(*g_pt)[p >> kGuestPageBits]) return false;
    }
    return true;
}

void add_readonly_range(gaddr lo, gaddr hi) {
    std::lock_guard lk(g_lock);
    g_ro.emplace_back(lo, hi);
}

bool is_readonly(gaddr a) {
    for (auto [lo, hi] : g_ro) {
        if (a >= lo && a < hi) return true;
    }
    return false;
}

gaddr gmalloc(u32 n) { return ga_checked(mspace_malloc(g_msp, n ? n : 1), "malloc"); }
gaddr gcalloc(u32 n, u32 size) { return ga_checked(mspace_calloc(g_msp, n, size), "calloc"); }
gaddr grealloc(gaddr p, u32 n) { return ga_checked(mspace_realloc(g_msp, p ? hp(p) : nullptr, n), "realloc"); }
void gfree(gaddr p) {
    if (p) mspace_free(g_msp, hp(p));
}
gaddr gmemalign(u32 align, u32 n) { return ga_checked(mspace_memalign(g_msp, align, n), "memalign"); }
u32 gusable_size(gaddr p) { return p ? static_cast<u32>(mspace_usable_size(hp(p))) : 0; }

HeapInfo heap_info() {
    struct mallinfo mi = mspace_mallinfo(g_msp);
    return HeapInfo{static_cast<u32>(mi.arena),    static_cast<u32>(mi.ordblks),  static_cast<u32>(mi.smblks),
                    static_cast<u32>(mi.hblks),    static_cast<u32>(mi.hblkhd),   static_cast<u32>(mi.usmblks),
                    static_cast<u32>(mi.fsmblks),  static_cast<u32>(mi.uordblks), static_cast<u32>(mi.fordblks),
                    static_cast<u32>(mi.keepcost)};
}

gaddr gstrdup(const char* s) {
    if (!s) return 0;
    return gdup(s, static_cast<u32>(strlen(s) + 1));
}

gaddr gdup(const void* p, u32 n) {
    gaddr a = gmalloc(n);
    if (a) memcpy(hp(a), p, n);
    return a;
}

}  // namespace zb::mem

extern "C" void* zb_dl_mmap(size_t n) {
    zb::gaddr a = zb::mem::map_anon(static_cast<zb::u32>(n), PROT_READ | PROT_WRITE, "zb-guest-heap");
    return a ? zb::hp(a) : reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
}

extern "C" int zb_dl_munmap(void* p, size_t n) {
    zb::mem::unmap_anon(zb::ga(p), static_cast<zb::u32>(n));
    return 0;
}
