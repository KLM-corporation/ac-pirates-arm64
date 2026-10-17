// Memory, string, ctype, stdlib, locale and wide-character functions.
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include <dynarmic/interface/A32/a32.h>

#include "../elf_loader.h"
#include "../mem.h"
#include "hle.h"

namespace zb::hle {
namespace {

std::mutex g_static_lock;
std::unordered_map<std::string, gaddr> g_static_strings;
std::mutex g_env_lock;
std::unordered_map<std::string, gaddr> g_env_cache;

// ---------------------------------------------------------------- memory

gaddr hle_malloc(gsize n) { return mem::gmalloc(n); }
gaddr hle_calloc(gsize n, gsize s) { return mem::gcalloc(n, s); }
gaddr hle_realloc(gaddr p, gsize n) { return mem::grealloc(p, n); }
void hle_free(gaddr p) { mem::gfree(p); }
gaddr hle_memalign(gsize a, gsize n) { return mem::gmemalign(a, n); }
gsize hle_usable_size(gaddr p) { return mem::gusable_size(p); }

void hle_mallinfo(gp<u32> out) {  // struct returned through r0
    mem::HeapInfo hi = mem::heap_info();
    memcpy(out.get(), &hi, sizeof(hi));
}

gaddr hle_memcpy(gaddr d, gaddr s, gsize n) {
    memcpy(hp(d), hp(s), n);
    return d;
}
gaddr hle_memmove(gaddr d, gaddr s, gsize n) {
    memmove(hp(d), hp(s), n);
    return d;
}
gaddr hle_memset(gaddr d, i32 c, gsize n) {
    memset(hp(d), c, n);
    return d;
}
i32 hle_memcmp(gaddr a, gaddr b, gsize n) { return memcmp(hp(a), hp(b), n); }
gaddr hle_memchr(gaddr s, i32 c, gsize n) { return ga(memchr(hp(s), c, n)); }

// ---------------------------------------------------------------- strings

gsize hle_strlen(gcstr s) { return static_cast<gsize>(strlen(s)); }
i32 hle_strcmp(gcstr a, gcstr b) { return strcmp(a, b); }
i32 hle_strncmp(gcstr a, gcstr b, gsize n) { return strncmp(a, b, n); }
i32 hle_strcasecmp(gcstr a, gcstr b) { return strcasecmp(a, b); }
i32 hle_strncasecmp(gcstr a, gcstr b, gsize n) { return strncasecmp(a, b, n); }
i32 hle_strcoll(gcstr a, gcstr b) { return strcoll(a, b); }
gsize hle_strxfrm(gstr d, gcstr s, gsize n) { return static_cast<gsize>(strxfrm(d, s, n)); }
gaddr hle_strcpy(gstr d, gcstr s) {
    strcpy(d, s);
    return d.a;
}
gaddr hle_strncpy(gstr d, gcstr s, gsize n) {
    strncpy(d, s, n);
    return d.a;
}
gaddr hle_strcat(gstr d, gcstr s) {
    strcat(d, s);
    return d.a;
}
gaddr hle_strchr(gcstr s, i32 c) { return ga(strchr(s, c)); }
gaddr hle_strrchr(gcstr s, i32 c) { return ga(strrchr(s, c)); }
gaddr hle_strstr(gcstr h, gcstr n) { return ga(strstr(h, n)); }
gaddr hle_strpbrk(gcstr s, gcstr a) { return ga(strpbrk(s, a)); }
gsize hle_strcspn(gcstr s, gcstr r) { return static_cast<gsize>(strcspn(s, r)); }
gaddr hle_strdup(gcstr s) { return mem::gstrdup(s); }

gaddr hle_strtok(gstr s, gcstr delim) { return ga(strtok(s.a ? s.get() : nullptr, delim)); }

gaddr hle_strtok_r(gstr s, gcstr delim, gp<u32> save) {
    char* host_save = hp<char>(*save.get());
    char* r = strtok_r(s.a ? s.get() : nullptr, delim, &host_save);
    *save.get() = ga(host_save);
    return ga(r);
}

gaddr hle_strerror(JitCtx& j, i32 e) {
    gaddr buf = tls_buffer(j, tls::kStrerror);
    snprintf(hp<char>(buf), 256, "%s", strerror(e));
    return buf;
}

// The guest links against the POSIX (int-returning) strerror_r.
i32 hle_strerror_r(i32 e, gstr buf, gsize n) {
    if (n == 0) return ERANGE;
    int len = snprintf(buf, n, "%s", strerror(e));
    return len >= static_cast<int>(n) ? ERANGE : 0;
}

gaddr hle_strsignal(JitCtx& j, i32 sig) {
    gaddr buf = tls_buffer(j, tls::kStrsignal);
    snprintf(hp<char>(buf), 64, "%s", strsignal(sig));
    return buf;
}

// POSIX basename() into a per-thread buffer, as old bionic did.
gaddr hle_basename(JitCtx& j, gcstr path) {
    gaddr buf = tls_buffer(j, tls::kScratch);
    std::string p = path.a ? path.get() : "";
    std::string r;
    if (p.empty()) {
        r = ".";
    } else {
        size_t end = p.find_last_not_of('/');
        if (end == std::string::npos) {
            r = "/";
        } else {
            size_t start = p.rfind('/', end);
            r = p.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
        }
    }
    snprintf(hp<char>(buf), 1024, "%s", r.c_str());
    return buf;
}

// ---------------------------------------------------------------- ctype

bool in_ascii(i32 c) { return c >= 0 && c < 128; }
i32 hle_isalnum(i32 c) { return in_ascii(c) && isalnum(c); }
i32 hle_isalpha(i32 c) { return in_ascii(c) && isalpha(c); }
i32 hle_isspace(i32 c) { return in_ascii(c) && isspace(c); }
i32 hle_isupper(i32 c) { return in_ascii(c) && isupper(c); }
i32 hle_isxdigit(i32 c) { return in_ascii(c) && isxdigit(c); }
i32 hle_tolower(i32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
i32 hle_toupper(i32 c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

void install_ctype_tables() {
    enum { U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    gaddr ctype = mem::gcalloc(1, 257 + 3);
    gaddr lower = mem::gcalloc(257, 2);
    gaddr upper = mem::gcalloc(257, 2);
    u8* t = hp<u8>(ctype) + 1;
    for (int c = 0; c < 128; ++c) {
        u8 f = 0;
        if (c < 32 || c == 127) f |= C;
        if ((c >= 9 && c <= 13)) f |= S;
        if (c == ' ') f |= S | B;
        if (c >= '0' && c <= '9') f |= N;
        if (c >= 'A' && c <= 'Z') f |= U;
        if (c >= 'a' && c <= 'z') f |= L;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= X;
        if (c > 32 && c < 127 && !(f & (U | L | N))) f |= P;
        t[c] = f;
    }
    i16* lo = hp<i16>(lower);
    i16* up = hp<i16>(upper);
    lo[0] = up[0] = -1;
    for (int c = 0; c < 256; ++c) {
        lo[c + 1] = static_cast<i16>(hle_tolower(c));
        up[c + 1] = static_cast<i16>(hle_toupper(c));
    }
    auto ptr_var = [](const char* name, gaddr target) {
        gaddr v = mem::gmalloc(4);
        wr<u32>(v, target);
        cpu::register_data(name, v);
    };
    ptr_var("_ctype_", ctype);
    ptr_var("_tolower_tab_", lower);
    ptr_var("_toupper_tab_", upper);
}

// ---------------------------------------------------------------- stdlib

i32 hle_atoi(gcstr s) { return atoi(s); }
glong hle_atol(gcstr s) { return static_cast<glong>(strtol(s, nullptr, 10)); }
i64 hle_atoll(gcstr s) { return atoll(s); }

glong hle_strtol(JitCtx& j, gcstr s, gaddr endp, i32 base) {
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, base);
    write_ptr(endp, end);
    if (v > INT32_MAX) {
        v = INT32_MAX;
        errno = ERANGE;
    } else if (v < INT32_MIN) {
        v = INT32_MIN;
        errno = ERANGE;
    }
    if (errno) sync_errno(j);
    return static_cast<glong>(v);
}

gulong hle_strtoul(JitCtx& j, gcstr s, gaddr endp, i32 base) {
    char* end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, base);
    write_ptr(endp, end);
    const char* p = s.get();
    while (isspace(static_cast<unsigned char>(*p))) p++;
    bool neg = *p == '-';
    u32 r;
    if (errno == ERANGE) {
        r = 0xFFFFFFFFu;
    } else if (neg) {
        unsigned long long mag = 0ull - v;
        if (mag > 0xFFFFFFFFull) {
            r = 0xFFFFFFFFu;
            errno = ERANGE;
        } else {
            r = static_cast<u32>(0u - static_cast<u32>(mag));
        }
    } else if (v > 0xFFFFFFFFull) {
        r = 0xFFFFFFFFu;
        errno = ERANGE;
    } else {
        r = static_cast<u32>(v);
    }
    if (errno) sync_errno(j);
    return r;
}

i64 hle_strtoll(JitCtx& j, gcstr s, gaddr endp, i32 base) {
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, base);
    write_ptr(endp, end);
    if (errno) sync_errno(j);
    return v;
}

double hle_strtod(JitCtx& j, gcstr s, gaddr endp) {
    char* end;
    errno = 0;
    double v = strtod(s, &end);
    write_ptr(endp, end);
    if (errno) sync_errno(j);
    return v;
}

glong hle_lrand48() { return static_cast<glong>(lrand48()); }
void hle_srand48(glong seed) { srand48(seed); }

gaddr hle_getenv(gcstr name) {
    const char* v = getenv(name);
    if (!v) return 0;
    std::lock_guard lk(g_env_lock);
    std::string key = std::string(name.get()) + "=" + v;
    auto it = g_env_cache.find(key);
    if (it != g_env_cache.end()) return it->second;
    gaddr g = mem::gstrdup(v);
    g_env_cache.emplace(key, g);
    return g;
}

i32 hle_setenv(JitCtx& j, gcstr name, gcstr value, i32 overwrite) {
    int r = setenv(name, value, overwrite);
    if (r) sync_errno(j);
    return r;
}

// Merge sort over element indices: robust against inconsistent comparators.
void hle_qsort(gaddr base, gsize n, gsize size, gaddr compar) {
    if (n < 2 || size == 0) return;
    std::vector<u32> idx(n), tmp(n);
    for (u32 i = 0; i < n; ++i) idx[i] = i;
    auto less_eq = [&](u32 a, u32 b) {
        u64 r = cpu::call(compar, cpu::Args().u32v(base + a * size).u32v(base + b * size));
        return static_cast<i32>(r) <= 0;
    };
    for (u32 width = 1; width < n; width *= 2) {
        for (u32 lo = 0; lo < n; lo += 2 * width) {
            u32 mid = std::min(lo + width, static_cast<u32>(n)), hi = std::min(lo + 2 * width, static_cast<u32>(n));
            u32 i = lo, k = mid, o = lo;
            while (i < mid && k < hi) tmp[o++] = less_eq(idx[i], idx[k]) ? idx[i++] : idx[k++];
            while (i < mid) tmp[o++] = idx[i++];
            while (k < hi) tmp[o++] = idx[k++];
        }
        idx.swap(tmp);
    }
    std::vector<u8> sorted(static_cast<size_t>(n) * size);
    for (u32 i = 0; i < n; ++i) memcpy(&sorted[static_cast<size_t>(i) * size], hp(base + idx[i] * size), size);
    memcpy(hp(base), sorted.data(), sorted.size());
}

// ---------------------------------------------------------------- locale / wide chars

gaddr hle_setlocale(i32 cat, gcstr locale) {
    const char* r = setlocale(cat, locale.a ? locale.get() : nullptr);
    return r ? static_string(r) : 0;
}

u32 hle_btowc(i32 c) { return btowc(c); }
i32 hle_wctob(u32 c) { return wctob(c); }
u32 hle_towlower(u32 c) { return towlower(c); }
u32 hle_towupper(u32 c) { return towupper(c); }
gulong hle_wctype(gcstr name) { return static_cast<gulong>(wctype(name)); }
i32 hle_iswctype(u32 c, gulong desc) { return iswctype(c, static_cast<wctype_t>(desc)); }

gsize hle_mbrtowc(gp<wchar_t> pwc, gcstr s, gsize n, gp<u8> ps) {
    mbstate_t st{};
    if (ps.a) memcpy(&st, ps.get(), 4);
    size_t r = mbrtowc(pwc.a ? pwc.get() : nullptr, s.a ? s.get() : nullptr, n, ps.a ? &st : nullptr);
    if (ps.a) memcpy(ps.get(), &st, 4);
    return static_cast<gsize>(r);
}

gsize hle_wcrtomb(gstr s, u32 wc, gp<u8> ps) {
    mbstate_t st{};
    if (ps.a) memcpy(&st, ps.get(), 4);
    size_t r = wcrtomb(s.a ? s.get() : nullptr, static_cast<wchar_t>(wc), ps.a ? &st : nullptr);
    if (ps.a) memcpy(ps.get(), &st, 4);
    return static_cast<gsize>(r);
}

gsize hle_mbstowcs(gp<wchar_t> d, gcstr s, gsize n) {
    return static_cast<gsize>(mbstowcs(d.a ? d.get() : nullptr, s, n));
}

i32 hle_wcscmp(gp<const wchar_t> a, gp<const wchar_t> b) { return wcscmp(a, b); }
i32 hle_wcscoll(gp<const wchar_t> a, gp<const wchar_t> b) { return wcscoll(a, b); }
gaddr hle_wcscpy(gp<wchar_t> d, gp<const wchar_t> s) { return ga(wcscpy(d, s)); }
gsize hle_wcslen(gp<const wchar_t> s) { return static_cast<gsize>(wcslen(s)); }
gsize hle_wcsxfrm(gp<wchar_t> d, gp<const wchar_t> s, gsize n) { return static_cast<gsize>(wcsxfrm(d, s, n)); }
gaddr hle_wmemchr(gp<const wchar_t> s, u32 c, gsize n) { return ga(wmemchr(s, static_cast<wchar_t>(c), n)); }
i32 hle_wmemcmp(gp<const wchar_t> a, gp<const wchar_t> b, gsize n) { return wmemcmp(a, b, n); }
gaddr hle_wmemcpy(gp<wchar_t> d, gp<const wchar_t> s, gsize n) { return ga(wmemcpy(d, s, n)); }
gaddr hle_wmemmove(gp<wchar_t> d, gp<const wchar_t> s, gsize n) { return ga(wmemmove(d, s, n)); }
gaddr hle_wmemset(gp<wchar_t> d, u32 c, gsize n) { return ga(wmemset(d, static_cast<wchar_t>(c), n)); }

// ---------------------------------------------------------------- misc

[[noreturn]] void hle_abort(JitCtx& j) {
    cpu::dump_state(j, "abort()");
    fatal("guest called abort()");
}

[[noreturn]] void hle_stack_chk_fail(JitCtx& j) {
    cpu::dump_state(j, "__stack_chk_fail");
    fatal("guest stack corruption detected");
}

void hle_exit(i32 code) {
    ZLOGW("guest called exit(%d)", code);
    exit(code);
}

[[noreturn]] void hle_assert(JitCtx& j, gcstr file, i32 line, gcstr msg) {
    cpu::dump_state(j, "__assert");
    fatal("guest assertion failed: %s:%d: %s", file.get(), line, msg.get());
}

[[noreturn]] void hle_assert2(JitCtx& j, gcstr file, i32 line, gcstr func, gcstr msg) {
    cpu::dump_state(j, "__assert2");
    fatal("guest assertion failed: %s:%d (%s): %s", file.get(), line, func.get(), msg.get());
}

gaddr hle_errno_location(JitCtx& j) { return j.thread->tls + tls::kErrno; }

i32 hle_cxa_atexit(gaddr, gaddr, gaddr) { return 0; }  // the process is killed, never exits
void hle_cxa_finalize(gaddr) {}
void hle_noop() {}

// Lets the guest's statically linked unwinder find the exception index table.
gaddr hle_find_exidx(gaddr pc, gp<i32> count) {
    elf::Module* m = elf::module_for_addr(pc);
    if (!m || !m->exidx) {
        *count.get() = 0;
        return 0;
    }
    *count.get() = static_cast<i32>(m->exidx_count);
    return m->exidx;
}

i32 hle_isfinitef(float f) { return std::isfinite(f) ? 1 : 0; }

// ---------------------------------------------------------------- setjmp / longjmp
//
// The thunk runs inside the guest call, so the registers seen here are the
// caller's: r4-r11, sp and lr (the return address) are what must be restored.
// jmp_buf layout (our own, 256 bytes available on LP32 bionic):
//   [0..7] r4-r11, [8] sp, [9] lr, [10..25] d8-d15, [26] fpscr, [27] magic
constexpr u32 kJmpMagic = 0x5A424A42;

void save_jmp(JitCtx& j, gaddr buf) {
    auto& r = j.r();
    for (int i = 0; i < 8; ++i) wr<u32>(buf + 4 * i, r[4 + i]);
    wr<u32>(buf + 32, r[13]);
    wr<u32>(buf + 36, r[14]);
    auto& ext = j.jit->ExtRegs();
    for (int i = 0; i < 16; ++i) wr<u32>(buf + 40 + 4 * i, ext[16 + i]);
    wr<u32>(buf + 104, j.jit->Fpscr());
    wr<u32>(buf + 108, kJmpMagic);
}

void hle_setjmp(JitCtx& j) {
    save_jmp(j, j.r()[0]);
    j.set_ret(0);
}

void hle_longjmp(JitCtx& j) {
    gaddr buf = j.r()[0];
    u32 val = j.r()[1];
    if (rd<u32>(buf + 108) != kJmpMagic) {
        cpu::dump_state(j, "longjmp");
        fatal("longjmp to a jmp_buf that was not filled by setjmp");
    }
    auto& r = j.r();
    for (int i = 0; i < 8; ++i) r[4 + i] = rd<u32>(buf + 4 * i);
    r[13] = rd<u32>(buf + 32);
    u32 target = rd<u32>(buf + 36);
    auto& ext = j.jit->ExtRegs();
    for (int i = 0; i < 16; ++i) ext[16 + i] = rd<u32>(buf + 40 + 4 * i);
    j.jit->SetFpscr(rd<u32>(buf + 104));
    r[0] = val ? val : 1;
    r[15] = target & ~1u;
    u32 cpsr = j.jit->Cpsr();
    j.jit->SetCpsr((target & 1) ? (cpsr | 0x20) : (cpsr & ~0x20u));
}

// ---------------------------------------------------------------- zlib (uLong is 32-bit in the guest)

i32 z_compress(gaddr dst, gp<u32> dst_len, gaddr src, u32 src_len) {
    uLongf n = *dst_len.get();
    int r = compress(hp<Bytef>(dst), &n, hp<const Bytef>(src), src_len);
    *dst_len.get() = static_cast<u32>(n);
    return r;
}

i32 z_uncompress(gaddr dst, gp<u32> dst_len, gaddr src, u32 src_len) {
    uLongf n = *dst_len.get();
    int r = uncompress(hp<Bytef>(dst), &n, hp<const Bytef>(src), src_len);
    *dst_len.get() = static_cast<u32>(n);
    return r;
}

}  // namespace

gaddr static_string(const std::string& s) {
    std::lock_guard lk(g_static_lock);
    auto it = g_static_strings.find(s);
    if (it != g_static_strings.end()) return it->second;
    gaddr g = mem::gstrdup(s.c_str());
    g_static_strings.emplace(s, g);
    return g;
}

gaddr tls_buffer(JitCtx& j, u32 off) { return j.thread->tls + off; }

void register_libc() {
    REG(malloc, hle_malloc);
    REG(calloc, hle_calloc);
    REG(realloc, hle_realloc);
    REG(free, hle_free);
    REG(memalign, hle_memalign);
    REG(dlmalloc_usable_size, hle_usable_size);
    REG(malloc_usable_size, hle_usable_size);
    REG(mallinfo, hle_mallinfo);
    REG(memcpy, hle_memcpy);
    REG(memmove, hle_memmove);
    REG(memset, hle_memset);
    REG(memcmp, hle_memcmp);
    REG(memchr, hle_memchr);

    REG(strlen, hle_strlen);
    REG(strcmp, hle_strcmp);
    REG(strncmp, hle_strncmp);
    REG(strcasecmp, hle_strcasecmp);
    REG(strncasecmp, hle_strncasecmp);
    REG(strcoll, hle_strcoll);
    REG(strxfrm, hle_strxfrm);
    REG(strcpy, hle_strcpy);
    REG(strncpy, hle_strncpy);
    REG(strcat, hle_strcat);
    REG(strchr, hle_strchr);
    REG(strrchr, hle_strrchr);
    REG(strstr, hle_strstr);
    REG(strpbrk, hle_strpbrk);
    REG(strcspn, hle_strcspn);
    REG(strdup, hle_strdup);
    REG(strtok, hle_strtok);
    REG(strtok_r, hle_strtok_r);
    REG(strerror, hle_strerror);
    REG(strerror_r, hle_strerror_r);
    REG(strsignal, hle_strsignal);
    REG(basename, hle_basename);

    REG(isalnum, hle_isalnum);
    REG(isalpha, hle_isalpha);
    REG(isspace, hle_isspace);
    REG(isupper, hle_isupper);
    REG(isxdigit, hle_isxdigit);
    REG(tolower, hle_tolower);
    REG(toupper, hle_toupper);
    install_ctype_tables();

    REG(atoi, hle_atoi);
    REG(atol, hle_atol);
    REG(atoll, hle_atoll);
    REG(strtol, hle_strtol);
    REG(strtoul, hle_strtoul);
    REG(strtoll, hle_strtoll);
    REG(strtod, hle_strtod);
    REG(lrand48, hle_lrand48);
    REG(srand48, hle_srand48);
    REG(getenv, hle_getenv);
    REG(setenv, hle_setenv);
    REG(qsort, hle_qsort);

    REG(setlocale, hle_setlocale);
    REG(btowc, hle_btowc);
    REG(wctob, hle_wctob);
    REG(towlower, hle_towlower);
    REG(towupper, hle_towupper);
    REG(wctype, hle_wctype);
    REG(iswctype, hle_iswctype);
    REG(mbrtowc, hle_mbrtowc);
    REG(wcrtomb, hle_wcrtomb);
    REG(mbstowcs, hle_mbstowcs);
    REG(wcscmp, hle_wcscmp);
    REG(wcscoll, hle_wcscoll);
    REG(wcscpy, hle_wcscpy);
    REG(wcslen, hle_wcslen);
    REG(wcsxfrm, hle_wcsxfrm);
    REG(wmemchr, hle_wmemchr);
    REG(wmemcmp, hle_wmemcmp);
    REG(wmemcpy, hle_wmemcpy);
    REG(wmemmove, hle_wmemmove);
    REG(wmemset, hle_wmemset);

    REG(abort, hle_abort);
    REG(__stack_chk_fail, hle_stack_chk_fail);
    REG(exit, hle_exit);
    REG(__assert, hle_assert);
    REG(__assert2, hle_assert2);
    REG(__errno, hle_errno_location);
    REG(__cxa_atexit, hle_cxa_atexit);
    REG(__cxa_finalize, hle_cxa_finalize);
    REG(__google_potentially_blocking_region_begin, hle_noop);
    REG(__google_potentially_blocking_region_end, hle_noop);
    REG(__gnu_Unwind_Find_exidx, hle_find_exidx);
    REG(__isfinitef, hle_isfinitef);
    REG_RAW(setjmp, hle_setjmp);
    REG_RAW(sigsetjmp, hle_setjmp);
    REG_RAW(_setjmp, hle_setjmp);
    REG_RAW(longjmp, hle_longjmp);
    REG_RAW(siglongjmp, hle_longjmp);
    REG_RAW(_longjmp, hle_longjmp);
    REG(compress, z_compress);
    REG(uncompress, z_uncompress);

    gaddr guard = mem::gmalloc(4);
    std::random_device rd;
    wr<u32>(guard, rd() | 0x01000000u);
    cpu::register_data("__stack_chk_guard", guard);
}

}  // namespace zb::hle
