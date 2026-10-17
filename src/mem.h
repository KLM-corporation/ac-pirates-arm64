// Guest address space and guest heap.
//
// Guest pages are host pages at the same address (identity mapping inside the
// low 4 GiB). The Dynarmic page table maps each 4 KiB guest page to itself, or
// to nullptr when the page is not owned by the guest.
#pragma once

#include <array>

#include "common.h"

namespace zb::mem {

constexpr u32 kGuestPageBits = 12;
constexpr u32 kGuestPageSize = 1u << kGuestPageBits;
using PageTable = std::array<u8*, (1u << (32 - kGuestPageBits))>;

void init();
PageTable* page_table();
u32 host_page_size();

// Anonymous guest memory for runtime use (stacks, images, heap segments).
gaddr map_anon(u32 size, int prot, const char* tag);
void unmap_anon(gaddr addr, u32 size);

// Guest-visible mmap family. Return values follow the libc conventions
// (0xFFFFFFFF on mmap failure, -1 otherwise) and set *err to an errno value.
gaddr guest_mmap(gaddr addr, u32 len, int prot, int flags, int fd, i64 offset, int* err);
int guest_munmap(gaddr addr, u32 len, int* err);
int guest_mprotect(gaddr addr, u32 len, int prot, int* err);
gaddr guest_mremap(gaddr old_addr, u32 old_len, u32 new_len, int flags, int* err);

bool is_mapped(gaddr a);
bool is_range_mapped(gaddr a, u32 len);

// Ranges whose contents never change (code and read-only data). Dynarmic may
// constant-fold loads from them.
void add_readonly_range(gaddr lo, gaddr hi);
bool is_readonly(gaddr a);

// Guest heap (dlmalloc mspace living in guest memory).
gaddr gmalloc(u32 n);
gaddr gcalloc(u32 n, u32 size);
gaddr grealloc(gaddr p, u32 n);
void gfree(gaddr p);
gaddr gmemalign(u32 align, u32 n);
u32 gusable_size(gaddr p);

struct HeapInfo {
    u32 arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
HeapInfo heap_info();

gaddr gstrdup(const char* s);
gaddr gdup(const void* p, u32 n);

}  // namespace zb::mem
