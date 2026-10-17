// Files, directories, processes, signals, memory mappings and raw syscalls.
#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../mem.h"
#include "hle.h"

namespace zb::hle {

// ARM32 and arm64 disagree on four O_* bits (asm-arm vs asm-generic).
namespace oflags {
constexpr int kG_DIRECTORY = 040000, kG_NOFOLLOW = 0100000, kG_DIRECT = 0200000, kG_LARGEFILE = 0400000;
constexpr int kH_DIRECT = 040000, kH_LARGEFILE = 0100000, kH_DIRECTORY = 0200000, kH_NOFOLLOW = 0400000;
constexpr int kMask = 040000 | 0100000 | 0200000 | 0400000;
}  // namespace oflags

int open_flags_from_guest(int f) {
    using namespace oflags;
    int r = f & ~kMask;
    if (f & kG_DIRECTORY) r |= kH_DIRECTORY;
    if (f & kG_NOFOLLOW) r |= kH_NOFOLLOW;
    if (f & kG_DIRECT) r |= kH_DIRECT;
    if (f & kG_LARGEFILE) r |= kH_LARGEFILE;
    return r;
}

int open_flags_to_guest(int f) {
    using namespace oflags;
    int r = f & ~kMask;
    if (f & kH_DIRECTORY) r |= kG_DIRECTORY;
    if (f & kH_NOFOLLOW) r |= kG_NOFOLLOW;
    if (f & kH_DIRECT) r |= kG_DIRECT;
    return r;
}

namespace {

template <typename T>
T check(JitCtx& j, T r) {
    if (r < 0) sync_errno(j);
    return r;
}

// ------------------------------------------------------------ files

i32 x_open(JitCtx& j, gcstr path, i32 flags, u32 mode) {
    int fd = open(path, open_flags_from_guest(flags), mode);
    ZLOGD("open(%s, 0x%x) -> %d", path.get(), flags, fd);
    return check(j, fd);
}
i32 x_close(JitCtx& j, i32 fd) { return check(j, close(fd)); }
gssize x_read(JitCtx& j, i32 fd, gaddr buf, gsize n) { return check(j, static_cast<gssize>(read(fd, hp(buf), n))); }
gssize x_write(JitCtx& j, i32 fd, gaddr buf, gsize n) {
    return check(j, static_cast<gssize>(write(fd, hp(buf), n)));
}
goff x_lseek(JitCtx& j, i32 fd, goff off, i32 whence) {
    off_t r = lseek(fd, off, whence);
    if (r > INT32_MAX) {
        j.set_errno(EOVERFLOW);
        return -1;
    }
    return check(j, static_cast<goff>(r));
}
gssize x_writev(JitCtx& j, i32 fd, gaddr iov, i32 cnt) {
    if (cnt < 0 || cnt > 1024) {
        j.set_errno(EINVAL);
        return -1;
    }
    std::vector<iovec> v(cnt);
    for (i32 i = 0; i < cnt; ++i) {
        v[i].iov_base = hp(rd<u32>(iov + 8 * i));
        v[i].iov_len = rd<u32>(iov + 8 * i + 4);
    }
    return check(j, static_cast<gssize>(writev(fd, v.data(), cnt)));
}
i32 x_pipe(JitCtx& j, gp<i32> fds) { return check(j, pipe(fds.get())); }
i32 x_access(JitCtx& j, gcstr p, i32 mode) { return check(j, access(p, mode)); }
i32 x_chmod(JitCtx& j, gcstr p, u32 mode) { return check(j, chmod(p, mode)); }
i32 x_fchmod(JitCtx& j, i32 fd, u32 mode) { return check(j, fchmod(fd, mode)); }
i32 x_fchown(JitCtx& j, i32 fd, u32 uid, u32 gid) { return check(j, fchown(fd, uid, gid)); }
i32 x_mkdir(JitCtx& j, gcstr p, u32 mode) { return check(j, mkdir(p, mode)); }
i32 x_rmdir(JitCtx& j, gcstr p) { return check(j, rmdir(p)); }
i32 x_unlink(JitCtx& j, gcstr p) { return check(j, unlink(p)); }
i32 x_remove(JitCtx& j, gcstr p) { return check(j, remove(p)); }
i32 x_rename(JitCtx& j, gcstr a, gcstr b) { return check(j, rename(a, b)); }
u32 x_umask(u32 m) { return umask(m); }
gaddr x_getcwd(JitCtx& j, gstr buf, gsize size) {
    if (!buf.a) {
        j.set_errno(EINVAL);
        return 0;
    }
    char* r = getcwd(buf, size);
    if (!r) sync_errno(j);
    return r ? buf.a : 0;
}
i32 x_ftruncate(JitCtx& j, i32 fd, goff len) { return check(j, ftruncate(fd, len)); }
i32 x_fsync(JitCtx& j, i32 fd) { return check(j, fsync(fd)); }
i32 x_mkstemp(JitCtx& j, gstr tmpl) { return check(j, mkstemp(tmpl)); }
i32 x_utimes(JitCtx& j, gcstr p, gp<GTimeval> tv) {
    if (!tv.a) return check(j, utimes(p, nullptr));
    timeval h[2] = {{tv.get()[0].tv_sec, tv.get()[0].tv_usec}, {tv.get()[1].tv_sec, tv.get()[1].tv_usec}};
    return check(j, utimes(p, h));
}

// Bionic LP32 struct stat (kernel stat64 layout, 104 bytes).
void to_guest_stat(gaddr g, const struct stat& s) {
    u8 b[104] = {};
    auto w32 = [&](int off, u32 v) { memcpy(b + off, &v, 4); };
    auto w64 = [&](int off, u64 v) { memcpy(b + off, &v, 8); };
    w64(0, s.st_dev);
    w32(12, static_cast<u32>(s.st_ino));
    w32(16, s.st_mode);
    w32(20, static_cast<u32>(s.st_nlink));
    w32(24, s.st_uid);
    w32(28, s.st_gid);
    w64(32, s.st_rdev);
    w64(48, static_cast<u64>(s.st_size));
    w32(56, static_cast<u32>(s.st_blksize));
    w64(64, static_cast<u64>(s.st_blocks));
    w32(72, static_cast<u32>(s.st_atim.tv_sec));
    w32(76, static_cast<u32>(s.st_atim.tv_nsec));
    w32(80, static_cast<u32>(s.st_mtim.tv_sec));
    w32(84, static_cast<u32>(s.st_mtim.tv_nsec));
    w32(88, static_cast<u32>(s.st_ctim.tv_sec));
    w32(92, static_cast<u32>(s.st_ctim.tv_nsec));
    w64(96, s.st_ino);
    memcpy(hp(g), b, sizeof(b));
}

i32 x_stat(JitCtx& j, gcstr p, gaddr out) {
    struct stat s;
    int r = stat(p, &s);
    if (r) return check(j, r);
    to_guest_stat(out, s);
    return 0;
}
i32 x_lstat(JitCtx& j, gcstr p, gaddr out) {
    struct stat s;
    int r = lstat(p, &s);
    if (r) return check(j, r);
    to_guest_stat(out, s);
    return 0;
}
i32 x_fstat(JitCtx& j, i32 fd, gaddr out) {
    struct stat s;
    int r = fstat(fd, &s);
    if (r) return check(j, r);
    to_guest_stat(out, s);
    return 0;
}

i32 x_fcntl(JitCtx& j, i32 fd, i32 cmd, u32 arg) {
    switch (cmd) {
    case F_GETFL: {
        int r = fcntl(fd, F_GETFL);
        return r < 0 ? check(j, r) : open_flags_to_guest(r);
    }
    case F_SETFL: return check(j, fcntl(fd, F_SETFL, open_flags_from_guest(static_cast<int>(arg))));
    case 5:   // F_GETLK
    case 6:   // F_SETLK
    case 7:   // F_SETLKW
    case 12:  // F_GETLK64 (ARM32)
    case 13:  // F_SETLK64
    case 14: {  // F_SETLKW64
        bool is64 = cmd >= 12;
        struct flock fl{};
        fl.l_type = rd<i16>(arg);
        fl.l_whence = rd<i16>(arg + 2);
        fl.l_start = is64 ? rd<i64>(arg + 8) : rd<i32>(arg + 4);
        fl.l_len = is64 ? rd<i64>(arg + 16) : rd<i32>(arg + 8);
        int hcmd = (cmd == 12 || cmd == 5) ? F_GETLK : (cmd == 13 || cmd == 6) ? F_SETLK : F_SETLKW;
        int r = fcntl(fd, hcmd, &fl);
        if (r < 0) return check(j, r);
        if (hcmd == F_GETLK) {
            wr<i16>(arg, fl.l_type);
            wr<i16>(arg + 2, fl.l_whence);
            if (is64) {
                wr<i64>(arg + 8, fl.l_start);
                wr<i64>(arg + 16, fl.l_len);
                wr<i32>(arg + 24, fl.l_pid);
            } else {
                wr<i32>(arg + 4, static_cast<i32>(fl.l_start));
                wr<i32>(arg + 8, static_cast<i32>(fl.l_len));
                wr<i32>(arg + 12, fl.l_pid);
            }
        }
        return r;
    }
    default: return check(j, fcntl(fd, cmd, static_cast<unsigned long>(arg)));
    }
}

i32 x_ioctl(JitCtx& j, i32 fd, i32 req, u32 arg) {
    if (static_cast<u32>(req) == SIOCGIFCONF) {
        struct ifconf ifc{};
        ifc.ifc_len = rd<i32>(arg);
        ifc.ifc_buf = hp<char>(rd<u32>(arg + 4));
        int r = ioctl(fd, SIOCGIFCONF, &ifc);
        if (r < 0) return check(j, r);
        wr<i32>(arg, ifc.ifc_len);
        return r;
    }
    return check(j, ioctl(fd, static_cast<unsigned>(req), hp(arg)));
}

// ------------------------------------------------------------ directories

struct GuestDir {
    DIR* d;
};
std::mutex g_dir_lock;
std::unordered_map<gaddr, DIR*> g_dirs;  // guest handle (also its dirent buffer) -> DIR*

gaddr x_opendir(JitCtx& j, gcstr p) {
    DIR* d = opendir(p);
    if (!d) {
        sync_errno(j);
        return 0;
    }
    gaddr g = mem::gcalloc(1, 288);
    std::lock_guard lk(g_dir_lock);
    g_dirs[g] = d;
    return g;
}
DIR* host_dir(gaddr g) {
    std::lock_guard lk(g_dir_lock);
    auto it = g_dirs.find(g);
    return it == g_dirs.end() ? nullptr : it->second;
}
gaddr x_readdir(JitCtx& j, gaddr g) {
    DIR* d = host_dir(g);
    if (!d) {
        j.set_errno(EBADF);
        return 0;
    }
    errno = 0;
    dirent* e = readdir(d);
    if (!e) {
        if (errno) sync_errno(j);
        return 0;
    }
    // Bionic uses the same dirent layout on LP32 and LP64.
    wr<u64>(g, e->d_ino);
    wr<i64>(g + 8, e->d_off);
    wr<u16>(g + 16, e->d_reclen);
    wr<u8>(g + 18, e->d_type);
    snprintf(hp<char>(g + 19), 256, "%s", e->d_name);
    return g;
}
i32 x_closedir(JitCtx& j, gaddr g) {
    DIR* d;
    {
        std::lock_guard lk(g_dir_lock);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) {
            j.set_errno(EBADF);
            return -1;
        }
        d = it->second;
        g_dirs.erase(it);
    }
    mem::gfree(g);
    return check(j, closedir(d));
}
void x_rewinddir(gaddr g) {
    if (DIR* d = host_dir(g)) rewinddir(d);
}

// ------------------------------------------------------------ process

i32 x_getpid() { return getpid(); }
i32 x_gettid() { return gettid(); }
u32 x_getuid() { return getuid(); }
u32 x_geteuid() { return geteuid(); }
u32 x_getgid() { return getgid(); }
u32 x_getegid() { return getegid(); }

gaddr x_getpwuid(JitCtx& j, u32 uid) {
    passwd* p = getpwuid(uid);
    if (!p) return 0;
    gaddr base = tls_buffer(j, tls::kPasswd);
    gaddr strs = base + 24;
    auto put = [&](const char* s) {
        gaddr a = strs;
        size_t n = strlen(s ? s : "") + 1;
        if (strs + n > base + 0x260) return static_cast<gaddr>(0);
        memcpy(hp(a), s ? s : "", n);
        strs += static_cast<u32>(n);
        return a;
    };
    wr<u32>(base + 0, put(p->pw_name));
    wr<u32>(base + 4, put(p->pw_passwd));
    wr<u32>(base + 8, p->pw_uid);
    wr<u32>(base + 12, p->pw_gid);
    wr<u32>(base + 16, put(p->pw_dir));
    wr<u32>(base + 20, put(p->pw_shell));
    return base;
}

i32 x_system(gcstr cmd) {
    ZLOGW("system(%s)", cmd.a ? cmd.get() : "(null)");
    return cmd.a ? system(cmd) : 1;
}

glong x_sysconf(i32 name) { return static_cast<glong>(sysconf(name)); }

// ------------------------------------------------------------ signals (recorded, never installed)

struct GuestSigaction {
    u32 handler = 0, mask = 0;
    i32 flags = 0;
    u32 restorer = 0;
};
std::mutex g_sig_lock;
GuestSigaction g_sig[65];

i32 x_sigaction(JitCtx& j, i32 sig, gaddr act, gaddr oact) {
    if (sig <= 0 || sig > 64) {
        j.set_errno(EINVAL);
        return -1;
    }
    std::lock_guard lk(g_sig_lock);
    if (oact) memcpy(hp(oact), &g_sig[sig], 16);
    if (act) {
        memcpy(&g_sig[sig], hp(act), 16);
        ZLOGD("sigaction(%d) handler=0x%08x flags=0x%x (not installed)", sig, g_sig[sig].handler, g_sig[sig].flags);
    }
    return 0;
}

u32 x_bsd_signal(i32 sig, u32 handler) {
    if (sig <= 0 || sig > 64) return 0xFFFFFFFFu;  // SIG_ERR
    std::lock_guard lk(g_sig_lock);
    u32 old = g_sig[sig].handler;
    g_sig[sig].handler = handler;
    g_sig[sig].flags = 0;
    return old;
}

i32 raise_guest(JitCtx& j, i32 sig) {
    GuestSigaction a;
    {
        std::lock_guard lk(g_sig_lock);
        a = (sig > 0 && sig <= 64) ? g_sig[sig] : GuestSigaction{};
    }
    if (a.handler > 1) {
        ZLOGW("delivering signal %d to guest handler 0x%08x", sig, a.handler);
        if (a.flags & SA_SIGINFO) cpu::call(a.handler, cpu::Args().i32v(sig).u32v(0).u32v(0));
        else cpu::call(a.handler, cpu::Args().i32v(sig));
        return 0;
    }
    if (a.handler == 1) return 0;  // SIG_IGN
    switch (sig) {
    case SIGABRT:
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGKILL:
    case SIGTERM:
        cpu::dump_state(j, "fatal signal raised by guest");
        fatal("guest raised fatal signal %d", sig);
    default:
        ZLOGW("guest raised signal %d (default action ignored)", sig);
        return 0;
    }
}

i32 x_raise(JitCtx& j, i32 sig) { return raise_guest(j, sig); }

i32 x_kill(JitCtx& j, i32 pid, i32 sig) {
    if (pid == 0 || pid == getpid()) {
        if (sig == 0) return 0;
        return raise_guest(j, sig);
    }
    ZLOGW("kill(%d, %d)", pid, sig);
    return check(j, kill(pid, sig));
}

i32 x_sigaltstack(gaddr ss, gaddr old) {
    if (old) {
        wr<u32>(old, 0);
        wr<i32>(old + 4, 2 /* SS_DISABLE */);
        wr<u32>(old + 8, 0);
    }
    (void)ss;
    return 0;
}

i32 x_setitimer(i32 which, gaddr nv, gaddr ov) {
    if (nv && (rd<u32>(nv + 8) || rd<u32>(nv + 12))) ZLOGW("setitimer(%d) ignored", which);
    if (ov) memset(hp(ov), 0, 16);
    return 0;
}
u32 x_alarm(u32 s) {
    if (s) ZLOGW("alarm(%u) ignored", s);
    return 0;
}

// ------------------------------------------------------------ memory mappings

gaddr x_mmap(JitCtx& j, gaddr addr, gsize len, i32 prot, i32 flags, i32 fd, goff off) {
    int err = 0;
    gaddr r = mem::guest_mmap(addr, len, prot, flags, fd, off, &err);
    ZLOGD("mmap(0x%08x, %u, %d, 0x%x, %d, %d) -> 0x%08x", addr, len, prot, flags, fd, off, r);
    if (r == 0xFFFFFFFFu) j.set_errno(err);
    return r;
}
i32 x_munmap(JitCtx& j, gaddr addr, gsize len) {
    int err = 0;
    int r = mem::guest_munmap(addr, len, &err);
    if (r) j.set_errno(err);
    cpu::invalidate_code(addr, len);
    return r;
}
i32 x_mprotect(JitCtx& j, gaddr addr, gsize len, i32 prot) {
    int err = 0;
    int r = mem::guest_mprotect(addr, len, prot, &err);
    if (r) j.set_errno(err);
    return r;
}
gaddr x_mremap(JitCtx& j, gaddr old, gsize old_len, gsize new_len, i32 flags) {
    int err = 0;
    gaddr r = mem::guest_mremap(old, old_len, new_len, flags, &err);
    if (r == 0xFFFFFFFFu) j.set_errno(err);
    return r;
}

// ------------------------------------------------------------ raw syscalls (ARM EABI numbers)

i32 do_syscall(JitCtx& j, u32 nr, const u32* a) {
    switch (nr) {
    case 20: return getpid();
    case 224: return gettid();
    case 240: {  // futex
        int op = static_cast<int>(a[1]);
        timespec ts, *pts = nullptr;
        int cmd = op & FUTEX_CMD_MASK;
        if (a[3] && (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET)) {
            ts.tv_sec = rd<i32>(a[3]);
            ts.tv_nsec = rd<i32>(a[3] + 4);
            pts = &ts;
        }
        long r = syscall(SYS_futex, hp(a[0]), op, a[2], pts ? static_cast<void*>(pts) : hp(a[3]), hp(a[4]), a[5]);
        return r < 0 ? -errno : static_cast<i32>(r);
    }
    case 263: {  // clock_gettime
        timespec ts;
        if (clock_gettime(static_cast<clockid_t>(a[0]), &ts)) return -errno;
        wr<i32>(a[1], static_cast<i32>(ts.tv_sec));
        wr<i32>(a[1] + 4, static_cast<i32>(ts.tv_nsec));
        return 0;
    }
    case 384: {  // getrandom
        ssize_t r = getrandom(hp(a[0]), a[1], a[2]);
        return r < 0 ? -errno : static_cast<i32>(r);
    }
    case 241:  // sched_setaffinity
        return 0;
    case 0xF0002:  // __ARM_NR_cacheflush
        cpu::invalidate_code(a[0], a[1] - a[0]);
        return 0;
    case 0xF0005:  // __ARM_NR_set_tls
        j.tpidruro = a[0];
        return 0;
    default:
        ZLOGW("unimplemented guest syscall %u (0x%x, 0x%x, 0x%x)", nr, a[0], a[1], a[2]);
        return -ENOSYS;
    }
}

void x_syscall(JitCtx& j) {
    u32 a[6];
    for (unsigned i = 0; i < 6; ++i) a[i] = j.arg(i + 1);
    i32 r = do_syscall(j, j.arg(0), a);
    if (r < 0 && r > -4096) {
        j.set_errno(-r);
        j.set_ret(static_cast<u32>(-1));
    } else {
        j.set_ret(static_cast<u32>(r));
    }
}

}  // namespace

void linux_syscall(JitCtx& j) {
    auto& r = j.r();
    u32 a[6] = {r[0], r[1], r[2], r[3], r[4], r[5]};
    r[0] = static_cast<u32>(do_syscall(j, r[7], a));
}

void register_posix() {
    REG(open, x_open);
    REG(close, x_close);
    REG(read, x_read);
    REG(write, x_write);
    REG(lseek, x_lseek);
    REG(writev, x_writev);
    REG(pipe, x_pipe);
    REG(access, x_access);
    REG(chmod, x_chmod);
    REG(fchmod, x_fchmod);
    REG(fchown, x_fchown);
    REG(mkdir, x_mkdir);
    REG(rmdir, x_rmdir);
    REG(unlink, x_unlink);
    REG(remove, x_remove);
    REG(rename, x_rename);
    REG(umask, x_umask);
    REG(getcwd, x_getcwd);
    REG(ftruncate, x_ftruncate);
    REG(fsync, x_fsync);
    REG(mkstemp, x_mkstemp);
    REG(utimes, x_utimes);
    REG(stat, x_stat);
    REG(lstat, x_lstat);
    REG(fstat, x_fstat);
    REG(fcntl, x_fcntl);
    REG(ioctl, x_ioctl);
    REG(opendir, x_opendir);
    REG(readdir, x_readdir);
    REG(closedir, x_closedir);
    REG(rewinddir, x_rewinddir);
    REG(getpid, x_getpid);
    REG(gettid, x_gettid);
    REG(getuid, x_getuid);
    REG(geteuid, x_geteuid);
    REG(getgid, x_getgid);
    REG(getegid, x_getegid);
    REG(getpwuid, x_getpwuid);
    REG(system, x_system);
    REG(sysconf, x_sysconf);
    REG(sigaction, x_sigaction);
    REG(bsd_signal, x_bsd_signal);
    REG(signal, x_bsd_signal);
    REG(raise, x_raise);
    REG(kill, x_kill);
    REG(sigaltstack, x_sigaltstack);
    REG(setitimer, x_setitimer);
    REG(alarm, x_alarm);
    REG(mmap, x_mmap);
    REG(munmap, x_munmap);
    REG(mprotect, x_mprotect);
    REG(mremap, x_mremap);
    REG_RAW(syscall, x_syscall);
    cpu::set_syscall_handler(&linux_syscall);
}

}  // namespace zb::hle
