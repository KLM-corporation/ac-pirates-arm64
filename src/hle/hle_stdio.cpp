// stdio: FILE* mapping, printf/scanf engines working on guest va_lists.
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <mutex>
#include <unordered_map>

#include "../mem.h"
#include "hle.h"

#pragma clang diagnostic ignored "-Wformat-nonliteral"

namespace zb::hle {
namespace {

// Old bionic (LP32) struct __sFILE is 84 bytes; _flags at +12, _file at +14.
constexpr u32 kGuestFileSize = 84;
constexpr u16 kSEOF = 0x0020;
constexpr u16 kSERR = 0x0040;

std::mutex g_files_lock;
std::unordered_map<gaddr, FILE*> g_files;
gaddr g_sF = 0;

struct LineLog {
    std::mutex lock;
    std::string buf;
};
LineLog g_out_log, g_err_log;

void log_stream(LineLog& l, const char* tag, const void* data, size_t n) {
    std::lock_guard lk(l.lock);
    l.buf.append(static_cast<const char*>(data), n);
    size_t pos;
    while ((pos = l.buf.find('\n')) != std::string::npos) {
        __android_log_print(ANDROID_LOG_INFO, tag, "%s", l.buf.substr(0, pos).c_str());
        l.buf.erase(0, pos + 1);
    }
    if (l.buf.size() > 4096) {
        __android_log_print(ANDROID_LOG_INFO, tag, "%s", l.buf.c_str());
        l.buf.clear();
    }
}

void update_flags(gaddr gf, FILE* f) {
    if (gf < 0x1000) return;
    u16 flags = rd<u16>(gf + 12) & ~(kSEOF | kSERR);
    if (feof(f)) flags |= kSEOF;
    if (ferror(f)) flags |= kSERR;
    wr<u16>(gf + 12, flags);
}

size_t do_write(gaddr gf, FILE* f, const void* data, size_t n) {
    if (f == stdout) log_stream(g_out_log, "ACP64-stdout", data, n);
    if (f == stderr) log_stream(g_err_log, "ACP64-stderr", data, n);
    size_t r = fwrite(data, 1, n, f);
    update_flags(gf, f);
    return (f == stdout || f == stderr) ? n : r;
}

// ------------------------------------------------------------ guest va_list

struct GVa {
    u32 ap;
    u32 next32() {
        u32 v = rd<u32>(ap);
        ap += 4;
        return v;
    }
    u64 next64() {
        ap = (ap + 7) & ~7u;
        u64 v = rd<u64>(ap);
        ap += 8;
        return v;
    }
    double nextd() {
        u64 b = next64();
        double d;
        memcpy(&d, &b, 8);
        return d;
    }
};

enum class Len { None, HH, H, L, LL, J, Z, T, BigL };

template <typename C>
const C* parse_len(const C* p, Len& len) {
    len = Len::None;
    if (*p == 'h') {
        p++;
        if (*p == 'h') {
            p++;
            len = Len::HH;
        } else {
            len = Len::H;
        }
    } else if (*p == 'l') {
        p++;
        if (*p == 'l') {
            p++;
            len = Len::LL;
        } else {
            len = Len::L;
        }
    } else if (*p == 'q') {
        p++;
        len = Len::LL;
    } else if (*p == 'j') {
        p++;
        len = Len::J;
    } else if (*p == 'z') {
        p++;
        len = Len::Z;
    } else if (*p == 't') {
        p++;
        len = Len::T;
    } else if (*p == 'L') {
        p++;
        len = Len::BigL;
    }
    return p;
}

template <typename C>
struct Fmt;
template <>
struct Fmt<char> {
    template <typename T>
    static void one(std::string& out, const std::string& spec, T v) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), spec.c_str(), v);
        if (n < 0) return;
        if (n < static_cast<int>(sizeof(buf))) {
            out.append(buf, n);
            return;
        }
        std::string big(n + 1, '\0');
        snprintf(big.data(), n + 1, spec.c_str(), v);
        out.append(big.data(), n);
    }
};
template <>
struct Fmt<wchar_t> {
    template <typename T>
    static void one(std::wstring& out, const std::wstring& spec, T v) {
        for (size_t cap = 256; cap <= (1u << 22); cap *= 4) {
            std::wstring buf(cap, L'\0');
            int n = swprintf(buf.data(), cap, spec.c_str(), v);
            if (n >= 0) {
                out.append(buf.data(), n);
                return;
            }
        }
    }
};

template <typename C>
std::basic_string<C> vformat_impl(const C* fmt, u32 ap, u32* end_ap) {
    using S = std::basic_string<C>;
    S out;
    GVa va{ap};
    auto num = [](int v) {
        S s;
        for (char c : std::to_string(v)) s += static_cast<C>(c);
        return s;
    };
    for (const C* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const C* start = p++;
        if (*p == '%') {
            out += static_cast<C>('%');
            p++;
            continue;
        }
        S spec(1, static_cast<C>('%'));
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' || *p == '\'') spec += *p++;
        if (*p == '*') {
            spec += num(static_cast<i32>(va.next32()));
            p++;
        } else {
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                i32 prec = static_cast<i32>(va.next32());
                if (prec < 0) spec.pop_back();
                else spec += num(prec);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') spec += *p++;
            }
        }
        Len len;
        p = parse_len(p, len);
        C conv = *p ? *p++ : 0;
        auto with = [&](const char* mod) {
            S s = spec;
            for (const char* m = mod; *m; ++m) s += static_cast<C>(*m);
            s += conv;
            return s;
        };
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (len == Len::LL || len == Len::J) {
                v = static_cast<long long>(va.next64());
            } else {
                i32 w = static_cast<i32>(va.next32());
                v = len == Len::HH ? static_cast<signed char>(w) : len == Len::H ? static_cast<short>(w) : w;
            }
            Fmt<C>::one(out, with("ll"), v);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;
            if (len == Len::LL || len == Len::J) {
                v = va.next64();
            } else {
                u32 w = va.next32();
                v = len == Len::HH ? static_cast<unsigned char>(w) : len == Len::H ? static_cast<unsigned short>(w) : w;
            }
            Fmt<C>::one(out, with("ll"), v);
            break;
        }
        case 'c':
            if (len == Len::L) Fmt<C>::one(out, with("l"), static_cast<wint_t>(va.next32()));
            else Fmt<C>::one(out, with(""), static_cast<int>(va.next32()));
            break;
        case 's': {
            u32 a = va.next32();
            if (len == Len::L) Fmt<C>::one(out, with("l"), a ? hp<const wchar_t>(a) : L"(null)");
            else Fmt<C>::one(out, with(""), a ? hp<const char>(a) : "(null)");
            break;
        }
        case 'p':
            Fmt<C>::one(out, with(""), reinterpret_cast<void*>(static_cast<uintptr_t>(va.next32())));
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            Fmt<C>::one(out, with(""), va.nextd());
            break;
        case 'n': {
            gaddr dst = va.next32();
            u64 count = out.size();
            if (len == Len::HH) wr<u8>(dst, static_cast<u8>(count));
            else if (len == Len::H) wr<u16>(dst, static_cast<u16>(count));
            else if (len == Len::LL || len == Len::J) wr<u64>(dst, count);
            else wr<u32>(dst, static_cast<u32>(count));
            break;
        }
        default:
            out.append(start, p - start);
            break;
        }
    }
    if (end_ap) *end_ap = va.ap;
    return out;
}

// ------------------------------------------------------------ scanf

struct Src {
    virtual ~Src() = default;
    virtual int get() = 0;
    virtual void unget(int c) = 0;
    size_t count = 0;
};

struct StrSrc final : Src {
    const unsigned char* s;
    size_t i = 0;
    explicit StrSrc(const char* str) : s(reinterpret_cast<const unsigned char*>(str)) {}
    int get() override {
        if (!s[i]) return EOF;
        count++;
        return s[i++];
    }
    void unget(int c) override {
        if (c != EOF) {
            i--;
            count--;
        }
    }
};

struct FileSrc final : Src {
    FILE* f;
    explicit FileSrc(FILE* file) : f(file) {}
    int get() override {
        int c = fgetc(f);
        if (c != EOF) count++;
        return c;
    }
    void unget(int c) override {
        if (c != EOF) {
            ungetc(c, f);
            count--;
        }
    }
};

void store_int(gaddr dst, Len len, u64 v) {
    switch (len) {
    case Len::HH: wr<u8>(dst, static_cast<u8>(v)); break;
    case Len::H: wr<u16>(dst, static_cast<u16>(v)); break;
    case Len::LL:
    case Len::J: wr<u64>(dst, v); break;
    default: wr<u32>(dst, static_cast<u32>(v)); break;
    }
}

int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

void skip_ws(Src& in) {
    int c;
    do c = in.get();
    while (c != EOF && isspace(c));
    in.unget(c);
}

int vscan(Src& in, const char* fmt, u32 ap) {
    GVa va{ap};
    int assigned = 0;
    bool converted = false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(fmt);
    auto input_failure = [&]() { return converted ? assigned : EOF; };
    while (*p) {
        if (isspace(*p)) {
            while (isspace(*p)) p++;
            skip_ws(in);
            continue;
        }
        if (*p != '%') {
            int c = in.get();
            if (c != *p) {
                in.unget(c);
                return c == EOF ? input_failure() : assigned;
            }
            p++;
            continue;
        }
        p++;
        if (*p == '%') {
            skip_ws(in);
            int c = in.get();
            if (c != '%') {
                in.unget(c);
                return c == EOF ? input_failure() : assigned;
            }
            p++;
            continue;
        }
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            p++;
        }
        int width = 0;
        while (isdigit(*p)) width = width * 10 + (*p++ - '0');
        Len len;
        p = reinterpret_cast<const unsigned char*>(parse_len(reinterpret_cast<const char*>(p), len));
        int conv = *p ? *p++ : 0;
        int limit = width ? width : INT_MAX;

        if (conv == 'n') {
            if (!suppress) store_int(va.next32(), len, in.count);
            continue;
        }
        if (conv == 'c') {
            int w = width ? width : 1;
            std::string tmp;
            for (int i = 0; i < w; ++i) {
                int c = in.get();
                if (c == EOF) break;
                tmp += static_cast<char>(c);
            }
            if (tmp.empty()) return input_failure();
            if (!suppress) {
                memcpy(hp(va.next32()), tmp.data(), tmp.size());
                assigned++;
            }
            converted = true;
            continue;
        }
        if (conv == '[') {
            bool table[256] = {};
            bool negate = false;
            if (*p == '^') {
                negate = true;
                p++;
            }
            if (*p == ']') table[*p++] = true;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (int c = p[0]; c <= p[2]; ++c) table[c] = true;
                    p += 3;
                } else {
                    table[*p++] = true;
                }
            }
            if (*p == ']') p++;
            std::string tmp;
            int c;
            while (limit-- > 0 && (c = in.get()) != EOF) {
                if (table[c] == negate) {
                    in.unget(c);
                    break;
                }
                tmp += static_cast<char>(c);
            }
            if (tmp.empty()) return c == EOF ? input_failure() : assigned;
            if (!suppress) {
                gaddr dst = va.next32();
                memcpy(hp(dst), tmp.c_str(), tmp.size() + 1);
                assigned++;
            }
            converted = true;
            continue;
        }

        skip_ws(in);
        if (conv == 's') {
            std::string tmp;
            int c = EOF;
            while (limit-- > 0 && (c = in.get()) != EOF && !isspace(c)) tmp += static_cast<char>(c);
            if (c != EOF && isspace(c)) in.unget(c);
            if (tmp.empty()) return input_failure();
            if (!suppress) {
                gaddr dst = va.next32();
                memcpy(hp(dst), tmp.c_str(), tmp.size() + 1);
                assigned++;
            }
            converted = true;
            continue;
        }

        if (strchr("diuoxXp", conv)) {
            int base = (conv == 'd' || conv == 'u') ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
            std::string tok;
            int c = in.get();
            if ((c == '+' || c == '-') && limit > 0) {
                tok += static_cast<char>(c);
                limit--;
                c = in.get();
            }
            int digits = 0;
            if ((base == 0 || base == 16) && c == '0' && limit > 0) {
                tok += '0';
                digits++;
                limit--;
                c = in.get();
                if ((c == 'x' || c == 'X') && limit > 0) {
                    tok += static_cast<char>(c);
                    limit--;
                    c = in.get();
                    base = 16;
                } else if (base == 0) {
                    base = 8;
                }
            }
            if (base == 0) base = 10;
            while (limit > 0 && c != EOF && digit_value(c) < base) {
                tok += static_cast<char>(c);
                digits++;
                limit--;
                c = in.get();
            }
            in.unget(c);
            if (!digits) return c == EOF && tok.empty() ? input_failure() : assigned;
            u64 v = (conv == 'd' || conv == 'i') ? static_cast<u64>(strtoll(tok.c_str(), nullptr, base))
                                                 : static_cast<u64>(strtoull(tok.c_str(), nullptr, base));
            if (!suppress) {
                store_int(va.next32(), conv == 'p' ? Len::None : len, v);
                assigned++;
            }
            converted = true;
            continue;
        }

        if (strchr("aAeEfFgG", conv)) {
            std::string tok;
            int c = in.get();
            auto take = [&]() {
                tok += static_cast<char>(c);
                limit--;
                c = in.get();
            };
            if ((c == '+' || c == '-') && limit > 0) take();
            if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
                while (limit > 0 && c != EOF && isalpha(c)) take();
            } else {
                bool hex = false;
                if (c == '0' && limit > 0) {
                    take();
                    if ((c == 'x' || c == 'X') && limit > 0) {
                        take();
                        hex = true;
                    }
                }
                auto is_digit = [&](int ch) { return hex ? isxdigit(ch) : isdigit(ch); };
                while (limit > 0 && c != EOF && is_digit(c)) take();
                if (c == '.' && limit > 0) {
                    take();
                    while (limit > 0 && c != EOF && is_digit(c)) take();
                }
                int e1 = hex ? 'p' : 'e', e2 = hex ? 'P' : 'E';
                if ((c == e1 || c == e2) && limit > 0) {
                    take();
                    if ((c == '+' || c == '-') && limit > 0) take();
                    while (limit > 0 && c != EOF && isdigit(c)) take();
                }
            }
            in.unget(c);
            char* end;
            double d = strtod(tok.c_str(), &end);
            if (tok.empty() || end == tok.c_str()) return c == EOF && tok.empty() ? input_failure() : assigned;
            if (!suppress) {
                gaddr dst = va.next32();
                if (len == Len::L || len == Len::BigL) {
                    wr<double>(dst, d);
                } else {
                    wr<float>(dst, static_cast<float>(d));
                }
                assigned++;
            }
            converted = true;
            continue;
        }
        ZLOGW("scanf: unsupported conversion '%c' in \"%s\"", conv, fmt);
        return assigned;
    }
    return assigned;
}

// ------------------------------------------------------------ FILE thunks

FILE* need_file(JitCtx& j, gaddr gf) {
    FILE* f = host_file(gf);
    if (!f) {
        ZLOGW("unknown guest FILE* 0x%08x", gf);
        j.set_errno(EBADF);
    }
    return f;
}

gaddr s_fopen(JitCtx& j, gcstr path, gcstr mode) {
    FILE* f = fopen(path, mode);
    ZLOGD("fopen(%s, %s) -> %p", path.get(), mode.get(), f);
    if (!f) {
        sync_errno(j);
        return 0;
    }
    return guest_file(f);
}

gaddr s_fdopen(JitCtx& j, i32 fd, gcstr mode) {
    FILE* f = fdopen(fd, mode);
    if (!f) {
        sync_errno(j);
        return 0;
    }
    return guest_file(f);
}

gaddr s_tmpfile(JitCtx& j) {
    FILE* f = tmpfile();
    if (!f) {
        sync_errno(j);
        return 0;
    }
    return guest_file(f);
}

gaddr s_popen(JitCtx& j, gcstr cmd, gcstr mode) {
    ZLOGW("popen(%s)", cmd.get());
    FILE* f = popen(cmd, mode);
    if (!f) {
        sync_errno(j);
        return 0;
    }
    return guest_file(f);
}

i32 s_fclose(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    if (f == stdin || f == stdout || f == stderr) return 0;
    forget_file(gf);
    int r = fclose(f);
    if (r) sync_errno(j);
    return r;
}

i32 s_pclose(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    forget_file(gf);
    return pclose(f);
}

i32 s_fflush(JitCtx& j, gaddr gf) {
    if (!gf) return fflush(nullptr);
    FILE* f = need_file(j, gf);
    return f ? fflush(f) : EOF;
}

gsize s_fread(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f || size == 0) return 0;
    size_t r = fread(hp(buf), size, n, f);
    update_flags(gf, f);
    if (r < n && ferror(f)) sync_errno(j);
    return static_cast<gsize>(r);
}

gsize s_fwrite(JitCtx& j, gaddr buf, gsize size, gsize n, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f || size == 0) return 0;
    size_t w = do_write(gf, f, hp(buf), static_cast<size_t>(size) * n);
    return static_cast<gsize>(w / size);
}

i32 s_fgetc(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    int c = fgetc(f);
    update_flags(gf, f);
    return c;
}

gaddr s_fgets(JitCtx& j, gstr buf, i32 n, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return 0;
    char* r = fgets(buf, n, f);
    update_flags(gf, f);
    return r ? buf.a : 0;
}

i32 s_fputc(JitCtx& j, i32 c, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    unsigned char ch = static_cast<unsigned char>(c);
    return do_write(gf, f, &ch, 1) == 1 ? ch : EOF;
}

i32 s_fputs(JitCtx& j, gcstr s, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    size_t n = strlen(s);
    return do_write(gf, f, s.get(), n) == n ? 0 : EOF;
}

i32 s_puts(gcstr s) {
    std::string line = std::string(s.get()) + "\n";
    do_write(g_sF + kGuestFileSize, stdout, line.data(), line.size());
    return 0;
}

i32 s_putchar(i32 c) {
    unsigned char ch = static_cast<unsigned char>(c);
    do_write(g_sF + kGuestFileSize, stdout, &ch, 1);
    return ch;
}

i32 s_fseek(JitCtx& j, gaddr gf, glong off, i32 whence) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    int r = fseeko(f, off, whence);
    if (r) sync_errno(j);
    update_flags(gf, f);
    return r;
}

glong s_ftell(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    off_t r = ftello(f);
    if (r > INT32_MAX) {
        j.set_errno(EOVERFLOW);
        return -1;
    }
    if (r < 0) sync_errno(j);
    return static_cast<glong>(r);
}

void s_clearerr(JitCtx& j, gaddr gf) {
    if (FILE* f = need_file(j, gf)) {
        clearerr(f);
        update_flags(gf, f);
    }
}

i32 s_setvbuf(JitCtx& j, gaddr gf, gaddr buf, i32 mode, gsize size) {
    FILE* f = need_file(j, gf);
    if (!f) return -1;
    return setvbuf(f, buf ? hp<char>(buf) : nullptr, mode, size);
}

i32 s_ungetc(JitCtx& j, i32 c, gaddr gf) {
    FILE* f = need_file(j, gf);
    if (!f) return EOF;
    int r = ungetc(c, f);
    update_flags(gf, f);
    return r;
}

u32 s_getwc(JitCtx& j, gaddr gf) {
    FILE* f = need_file(j, gf);
    return f ? static_cast<u32>(fgetwc(f)) : static_cast<u32>(WEOF);
}

u32 s_putwc(JitCtx& j, u32 wc, gaddr gf) {
    FILE* f = need_file(j, gf);
    return f ? static_cast<u32>(fputwc(static_cast<wchar_t>(wc), f)) : static_cast<u32>(WEOF);
}

u32 s_ungetwc(JitCtx& j, u32 wc, gaddr gf) {
    FILE* f = need_file(j, gf);
    return f ? static_cast<u32>(ungetwc(static_cast<wint_t>(wc), f)) : static_cast<u32>(WEOF);
}

// ------------------------------------------------------------ printf family

i32 copy_out(gaddr buf, u32 cap, const std::string& s) {
    if (cap > 0) {
        u32 n = std::min<u32>(cap - 1, static_cast<u32>(s.size()));
        memcpy(hp(buf), s.data(), n);
        wr<u8>(buf + n, 0);
    }
    return static_cast<i32>(s.size());
}

void s_vsnprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), n = r.w32(), fmt = r.w32(), ap = r.w32();
    j.set_ret(static_cast<u32>(copy_out(buf, n, vformat(hp<char>(fmt), ap))));
}

void s_snprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), n = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    j.set_ret(static_cast<u32>(copy_out(buf, n, vformat(hp<char>(fmt), ap))));
}

void s_vsprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), fmt = r.w32(), ap = r.w32();
    std::string s = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(copy_out(buf, static_cast<u32>(s.size()) + 1, s)));
}

void s_sprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string s = vformat(hp<char>(fmt), ap);
    j.set_ret(static_cast<u32>(copy_out(buf, static_cast<u32>(s.size()) + 1, s)));
}

void s_vfprintf(JitCtx& j) {
    ArgReader r(j);
    u32 gf = r.w32(), fmt = r.w32(), ap = r.w32();
    FILE* f = need_file(j, gf);
    std::string s = vformat(hp<char>(fmt), ap);
    if (f) do_write(gf, f, s.data(), s.size());
    j.set_ret(static_cast<u32>(s.size()));
}

void s_fprintf(JitCtx& j) {
    ArgReader r(j);
    u32 gf = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    FILE* f = need_file(j, gf);
    std::string s = vformat(hp<char>(fmt), ap);
    if (f) do_write(gf, f, s.data(), s.size());
    j.set_ret(static_cast<u32>(s.size()));
}

void s_vprintf(JitCtx& j) {
    ArgReader r(j);
    u32 fmt = r.w32(), ap = r.w32();
    std::string s = vformat(hp<char>(fmt), ap);
    do_write(g_sF + kGuestFileSize, stdout, s.data(), s.size());
    j.set_ret(static_cast<u32>(s.size()));
}

void s_printf(JitCtx& j) {
    ArgReader r(j);
    u32 fmt = r.w32();
    u32 ap = r.va_list_here();
    std::string s = vformat(hp<char>(fmt), ap);
    do_write(g_sF + kGuestFileSize, stdout, s.data(), s.size());
    j.set_ret(static_cast<u32>(s.size()));
}

void s_vswprintf(JitCtx& j) {
    ArgReader r(j);
    u32 buf = r.w32(), n = r.w32(), fmt = r.w32(), ap = r.w32();
    std::wstring s = vwformat(hp<wchar_t>(fmt), ap);
    if (n == 0) {
        j.set_ret(static_cast<u32>(-1));
        return;
    }
    if (s.size() >= n) {
        wmemcpy(hp<wchar_t>(buf), s.data(), n - 1);
        wr<u32>(buf + (n - 1) * 4, 0);
        j.set_ret(static_cast<u32>(-1));
        return;
    }
    wmemcpy(hp<wchar_t>(buf), s.data(), s.size());
    wr<u32>(buf + static_cast<u32>(s.size()) * 4, 0);
    j.set_ret(static_cast<u32>(s.size()));
}

void s_sscanf(JitCtx& j) {
    ArgReader r(j);
    u32 str = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    j.set_ret(static_cast<u32>(vscan_string(hp<char>(str), hp<char>(fmt), ap)));
}

void s_fscanf(JitCtx& j) {
    ArgReader r(j);
    u32 gf = r.w32(), fmt = r.w32();
    u32 ap = r.va_list_here();
    FILE* f = need_file(j, gf);
    j.set_ret(f ? static_cast<u32>(vscan_file(f, hp<char>(fmt), ap)) : static_cast<u32>(EOF));
    if (f) update_flags(gf, f);
}

}  // namespace

std::string vformat(const char* fmt, u32 ap, int* consumed_ap) {
    u32 end;
    std::string s = vformat_impl<char>(fmt, ap, &end);
    if (consumed_ap) *consumed_ap = static_cast<int>(end - ap);
    return s;
}

std::wstring vwformat(const wchar_t* fmt, u32 ap) { return vformat_impl<wchar_t>(fmt, ap, nullptr); }

int vscan_string(const char* input, const char* fmt, u32 ap) {
    StrSrc src(input);
    return vscan(src, fmt, ap);
}

int vscan_file(FILE* f, const char* fmt, u32 ap) {
    FileSrc src(f);
    return vscan(src, fmt, ap);
}

FILE* host_file(gaddr gf) {
    if (gf >= g_sF && gf < g_sF + 3 * kGuestFileSize) {
        u32 idx = (gf - g_sF) / kGuestFileSize;
        return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
    }
    std::lock_guard lk(g_files_lock);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}

gaddr guest_file(FILE* f) {
    if (f == stdin) return g_sF;
    if (f == stdout) return g_sF + kGuestFileSize;
    if (f == stderr) return g_sF + 2 * kGuestFileSize;
    gaddr gf = mem::gcalloc(1, kGuestFileSize + 12);
    wr<i16>(gf + 14, static_cast<i16>(fileno(f)));
    std::lock_guard lk(g_files_lock);
    g_files[gf] = f;
    return gf;
}

void forget_file(gaddr gf) {
    {
        std::lock_guard lk(g_files_lock);
        if (!g_files.erase(gf)) return;
    }
    mem::gfree(gf);
}

void register_stdio() {
    g_sF = mem::gcalloc(3, kGuestFileSize);
    for (u32 i = 0; i < 3; ++i) wr<i16>(g_sF + i * kGuestFileSize + 14, static_cast<i16>(i));
    cpu::register_data("__sF", g_sF);

    REG(fopen, s_fopen);
    REG(fdopen, s_fdopen);
    REG(tmpfile, s_tmpfile);
    REG(popen, s_popen);
    REG(fclose, s_fclose);
    REG(pclose, s_pclose);
    REG(fflush, s_fflush);
    REG(fread, s_fread);
    REG(fwrite, s_fwrite);
    REG(fgetc, s_fgetc);
    REG(getc, s_fgetc);
    REG(fgets, s_fgets);
    REG(fputc, s_fputc);
    REG(putc, s_fputc);
    REG(fputs, s_fputs);
    REG(puts, s_puts);
    REG(putchar, s_putchar);
    REG(fseek, s_fseek);
    REG(fseeko, s_fseek);
    REG(ftell, s_ftell);
    REG(ftello, s_ftell);
    REG(clearerr, s_clearerr);
    REG(setvbuf, s_setvbuf);
    REG(ungetc, s_ungetc);
    REG(getwc, s_getwc);
    REG(putwc, s_putwc);
    REG(ungetwc, s_ungetwc);

    REG_RAW(vsnprintf, s_vsnprintf);
    REG_RAW(snprintf, s_snprintf);
    REG_RAW(vsprintf, s_vsprintf);
    REG_RAW(sprintf, s_sprintf);
    REG_RAW(vfprintf, s_vfprintf);
    REG_RAW(fprintf, s_fprintf);
    REG_RAW(vprintf, s_vprintf);
    REG_RAW(printf, s_printf);
    REG_RAW(vswprintf, s_vswprintf);
    REG_RAW(sscanf, s_sscanf);
    REG_RAW(fscanf, s_fscanf);
}

}  // namespace zb::hle
