// pthreads and semaphores for the guest.
//
// Bionic LP32 synchronization objects are 4-byte words (mutex, condvar,
// semaphore, once). They are implemented directly on the guest word with host
// futexes, so static initializers keep working:
//   mutex word: [31:16] owner id (recursive/errorcheck only), [15:14] type,
//               [13] shared, [12:2] recursion count, [1:0] state
//               (0 unlocked, 1 locked, 2 locked with waiters).
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <mutex>
#include <unordered_map>

#include "hle.h"

namespace zb::hle {
namespace {

constexpr u32 kTypeMask = 0xC000;
constexpr u32 kShared = 0x2000;
constexpr u32 kState = 3;
constexpr u32 kCountInc = 4;
constexpr u32 kCountMask = 0x1FFC;
constexpr u32 kTypeErrorCheck = 0x8000;
constexpr u32 kGuestDefaultStack = 1024 * 1024;
constexpr size_t kHostThreadStack = 4 * 1024 * 1024;

int futex_wait(gaddr a, u32 val, const timespec* rel) {
    if (!g_debug) return static_cast<int>(syscall(SYS_futex, hp(a), FUTEX_WAIT_PRIVATE, val, rel, nullptr, 0));
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = static_cast<int>(syscall(SYS_futex, hp(a), FUTEX_WAIT_PRIVATE, val, rel, nullptr, 0));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    cpu::note_wait("futex", (t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    return r;
}
void futex_wake(gaddr a, int n) { syscall(SYS_futex, hp(a), FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0); }

u32* word(gaddr a) { return hp<u32>(a); }

// ------------------------------------------------------------ mutex

int mutex_lock(GuestThread* t, gaddr m, bool after_wait) {
    u32* w = word(m);
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);
    if (type == 0) {
        u32 expected = base;
        if (!after_wait &&
            __atomic_compare_exchange_n(w, &expected, base | 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
        while ((__atomic_exchange_n(w, base | 2, __ATOMIC_ACQUIRE) & kState) != 0) futex_wait(m, base | 2, nullptr);
        return 0;
    }
    u32 tid = t->id;
    if ((v & kState) != 0 && (v >> 16) == tid) {
        if (type == kTypeErrorCheck) return EDEADLK;
        if ((v & kCountMask) == kCountMask) return EAGAIN;
        __atomic_fetch_add(w, kCountInc, __ATOMIC_RELAXED);
        return 0;
    }
    u32 expected = base;
    if (__atomic_compare_exchange_n(w, &expected, base | 1 | (tid << 16), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;
    }
    for (;;) {
        u32 cur = __atomic_load_n(w, __ATOMIC_RELAXED);
        if ((cur & kState) == 0) {
            u32 e = cur;
            if (__atomic_compare_exchange_n(w, &e, base | 2 | (tid << 16), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
            continue;
        }
        if ((cur & kState) == 1) {
            u32 e = cur;
            u32 want = (cur & ~kState) | 2;
            if (!__atomic_compare_exchange_n(w, &e, want, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
            cur = want;
        }
        futex_wait(m, cur, nullptr);
    }
}

int mutex_trylock(GuestThread* t, gaddr m) {
    u32* w = word(m);
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);
    if (type != 0 && (v & kState) != 0 && (v >> 16) == t->id) {
        if (type == kTypeErrorCheck) return EBUSY;
        if ((v & kCountMask) == kCountMask) return EAGAIN;
        __atomic_fetch_add(w, kCountInc, __ATOMIC_RELAXED);
        return 0;
    }
    u32 expected = base;
    u32 locked = base | 1 | (type ? (t->id << 16) : 0);
    return __atomic_compare_exchange_n(w, &expected, locked, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}

int mutex_unlock(GuestThread* t, gaddr m) {
    u32* w = word(m);
    u32 v = __atomic_load_n(w, __ATOMIC_RELAXED);
    u32 type = v & kTypeMask;
    u32 base = v & (kTypeMask | kShared);
    if (type != 0) {
        if ((v & kState) == 0 || (v >> 16) != t->id) return EPERM;
        if (v & kCountMask) {
            __atomic_fetch_sub(w, kCountInc, __ATOMIC_RELEASE);
            return 0;
        }
    }
    u32 old = __atomic_exchange_n(w, base, __ATOMIC_RELEASE);
    if ((old & kState) == 2) futex_wake(m, 1);
    return 0;
}

i32 p_mutex_init(gaddr m, gaddr attr) {
    u32 type = attr ? (rd<u32>(attr) & 0xF) : 0;
    if (type > 2) return EINVAL;
    wr<u32>(m, type << 14);
    return 0;
}
i32 p_mutex_destroy(gaddr m) { return (rd<u32>(m) & kState) ? EBUSY : 0; }
i32 p_mutex_lock(JitCtx& j, gaddr m) { return mutex_lock(j.thread, m, false); }
i32 p_mutex_trylock(JitCtx& j, gaddr m) { return mutex_trylock(j.thread, m); }
i32 p_mutex_unlock(JitCtx& j, gaddr m) { return mutex_unlock(j.thread, m); }

i32 p_mutexattr_init(gaddr a) {
    wr<u32>(a, 0);
    return 0;
}
i32 p_mutexattr_destroy(gaddr) { return 0; }
i32 p_mutexattr_settype(gaddr a, i32 type) {
    if (type < 0 || type > 2) return EINVAL;
    wr<u32>(a, (rd<u32>(a) & ~0xFu) | static_cast<u32>(type));
    return 0;
}

// ------------------------------------------------------------ condvar

int cond_wait(GuestThread* t, gaddr c, gaddr m, const timespec* rel) {
    u32 seq = __atomic_load_n(word(c), __ATOMIC_RELAXED);
    mutex_unlock(t, m);
    int r = futex_wait(c, seq, rel);
    int err = (r == -1 && errno == ETIMEDOUT) ? ETIMEDOUT : 0;
    mutex_lock(t, m, true);
    return err;
}

i32 p_cond_init(gaddr c, gaddr) {
    wr<u32>(c, 0);
    return 0;
}
i32 p_cond_destroy(gaddr) { return 0; }
i32 p_cond_wait(JitCtx& j, gaddr c, gaddr m) { return cond_wait(j.thread, c, m, nullptr); }
i32 p_cond_timedwait_relative_np(JitCtx& j, gaddr c, gaddr m, gp<GTimespec> rel) {
    if (!rel.a) return cond_wait(j.thread, c, m, nullptr);
    timespec ts{rel->tv_sec, rel->tv_nsec};
    if (ts.tv_sec < 0 || (ts.tv_sec == 0 && ts.tv_nsec <= 0)) return ETIMEDOUT;
    return cond_wait(j.thread, c, m, &ts);
}
i32 p_cond_timedwait(JitCtx& j, gaddr c, gaddr m, gp<GTimespec> abs) {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    i64 ns = (static_cast<i64>(abs->tv_sec) - now.tv_sec) * 1000000000ll + (abs->tv_nsec - now.tv_nsec);
    if (ns <= 0) return ETIMEDOUT;
    timespec ts{static_cast<time_t>(ns / 1000000000ll), static_cast<long>(ns % 1000000000ll)};
    return cond_wait(j.thread, c, m, &ts);
}
i32 p_cond_signal(gaddr c) {
    __atomic_fetch_add(word(c), 4u, __ATOMIC_RELEASE);
    futex_wake(c, 1);
    return 0;
}
i32 p_cond_broadcast(gaddr c) {
    __atomic_fetch_add(word(c), 4u, __ATOMIC_RELEASE);
    futex_wake(c, INT_MAX);
    return 0;
}

// ------------------------------------------------------------ once / keys

i32 p_once(gaddr o, gaddr fn) {
    u32* w = word(o);
    if (__atomic_load_n(w, __ATOMIC_ACQUIRE) == 2) return 0;
    u32 e = 0;
    if (__atomic_compare_exchange_n(w, &e, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        cpu::call(fn);
        __atomic_store_n(w, 2u, __ATOMIC_RELEASE);
        futex_wake(o, INT_MAX);
        return 0;
    }
    while (__atomic_load_n(w, __ATOMIC_ACQUIRE) != 2) futex_wait(o, 1, nullptr);
    return 0;
}

struct KeyRec {
    bool used = false;
    gaddr dtor = 0;
};
std::mutex g_key_lock;
std::array<KeyRec, 128> g_keys;

i32 p_key_create(gp<u32> key, gaddr dtor) {
    std::lock_guard lk(g_key_lock);
    for (u32 i = 0; i < g_keys.size(); ++i) {
        if (!g_keys[i].used) {
            g_keys[i] = {true, dtor};
            *key.get() = i;
            return 0;
        }
    }
    return EAGAIN;
}
i32 p_key_delete(u32 key) {
    std::lock_guard lk(g_key_lock);
    if (key >= g_keys.size() || !g_keys[key].used) return EINVAL;
    g_keys[key] = {};
    return 0;
}
gaddr p_getspecific(JitCtx& j, u32 key) { return key < 128 ? j.thread->keys[key] : 0; }
i32 p_setspecific(JitCtx& j, u32 key, gaddr v) {
    if (key >= 128) return EINVAL;
    j.thread->keys[key] = v;
    return 0;
}

void run_key_destructors(GuestThread* t) {
    for (int iter = 0; iter < 4; ++iter) {
        bool any = false;
        for (u32 k = 0; k < 128; ++k) {
            gaddr dtor;
            {
                std::lock_guard lk(g_key_lock);
                if (!g_keys[k].used || !g_keys[k].dtor) continue;
                dtor = g_keys[k].dtor;
            }
            gaddr v = t->keys[k];
            if (!v) continue;
            t->keys[k] = 0;
            cpu::call(dtor, cpu::Args().u32v(v));
            any = true;
        }
        if (!any) break;
    }
}

// ------------------------------------------------------------ threads

struct ThreadRec {
    pthread_t host{};
    bool detached = false;
    bool done = false;
    u32 retval = 0;
};
std::mutex g_thread_lock;
std::unordered_map<gaddr, ThreadRec> g_threads;

struct StartInfo {
    GuestThread* t;
    gaddr fn;
    gaddr arg;
};

void* thread_main(void* p) {
    auto* info = static_cast<StartInfo*>(p);
    GuestThread* t = info->t;
    cpu::adopt_thread(t);
    ZLOGD("guest thread %u runs on tid %d (entry %s)", t->id, gettid(), cpu::symbolize(info->fn & ~1u).c_str());
    u64 r = cpu::call(info->fn, cpu::Args().u32v(info->arg));
    u32 retval = t->exiting ? t->exit_value : static_cast<u32>(r);
    t->exiting = false;
    run_key_destructors(t);
    cpu::run_thread_exit_hooks(t);
    gaddr handle = t->tls;
    delete info;
    std::lock_guard lk(g_thread_lock);
    auto it = g_threads.find(handle);
    if (it != g_threads.end()) {
        if (it->second.detached) {
            g_threads.erase(it);
        } else {
            it->second.done = true;
            it->second.retval = retval;
        }
    }
    return nullptr;
}

i32 p_create(gp<u32> out, gaddr attr, gaddr fn, gaddr arg) {
    u32 stack = kGuestDefaultStack;
    bool detached = false;
    if (attr) {
        detached = rd<u32>(attr) & 1;
        u32 s = rd<u32>(attr + 8);
        if (s >= 16384) stack = s;
    }
    GuestThread* t = cpu::new_thread_state(stack);
    auto* info = new StartInfo{t, fn, arg};
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, kHostThreadStack);
    if (detached) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    {
        std::lock_guard lk(g_thread_lock);
        g_threads[t->tls] = ThreadRec{{}, detached, false, 0};
    }
    *out.get() = t->tls;
    pthread_t host;
    int r = pthread_create(&host, &ha, thread_main, info);
    pthread_attr_destroy(&ha);
    if (r) {
        std::lock_guard lk(g_thread_lock);
        g_threads.erase(t->tls);
        delete info;
        return r;
    }
    std::lock_guard lk(g_thread_lock);
    if (auto it = g_threads.find(t->tls); it != g_threads.end()) it->second.host = host;
    ZLOGD("pthread_create -> guest thread %u (stack %u KiB)", t->id, stack / 1024);
    return 0;
}

bool find_host(gaddr handle, pthread_t* out) {
    std::lock_guard lk(g_thread_lock);
    auto it = g_threads.find(handle);
    if (it == g_threads.end()) return false;
    *out = it->second.host;
    return true;
}

i32 p_join(JitCtx& j, gaddr handle, gp<u32> retval) {
    if (handle == j.thread->tls) return EDEADLK;
    pthread_t host;
    if (!find_host(handle, &host)) return ESRCH;
    int r = pthread_join(host, nullptr);
    if (r) return r;
    std::lock_guard lk(g_thread_lock);
    auto it = g_threads.find(handle);
    if (it != g_threads.end()) {
        if (retval.a) *retval.get() = it->second.retval;
        g_threads.erase(it);
    }
    return 0;
}

i32 p_detach(gaddr handle) {
    std::lock_guard lk(g_thread_lock);
    auto it = g_threads.find(handle);
    if (it == g_threads.end()) return ESRCH;
    pthread_detach(it->second.host);
    if (it->second.done) g_threads.erase(it);
    else it->second.detached = true;
    return 0;
}

gaddr p_self(JitCtx& j) { return j.thread->tls; }

void p_exit(JitCtx& j, u32 retval) {
    j.thread->exiting = true;
    j.thread->exit_value = retval;
}

i32 p_kill(JitCtx& j, gaddr handle, i32 sig) {
    pthread_t host;
    bool known = handle == j.thread->tls || find_host(handle, &host);
    if (!known) return ESRCH;
    if (sig != 0) ZLOGW("pthread_kill(sig %d) ignored", sig);
    return 0;
}

i32 p_setname(JitCtx& j, gaddr handle, gcstr name) {
    std::string n(name.get());
    if (n.size() > 15) n.resize(15);
    if (handle == j.thread->tls) return pthread_setname_np(pthread_self(), n.c_str());
    pthread_t host;
    if (!find_host(handle, &host)) return ESRCH;
    return pthread_setname_np(host, n.c_str());
}

i32 p_getschedparam(JitCtx& j, gaddr handle, gp<i32> policy, gp<i32> param) {
    pthread_t host = pthread_self();
    if (handle != j.thread->tls && !find_host(handle, &host)) return ESRCH;
    sched_param sp{};
    int pol = 0;
    int r = pthread_getschedparam(host, &pol, &sp);
    if (!r) {
        if (policy.a) *policy.get() = pol;
        if (param.a) *param.get() = sp.sched_priority;
    }
    return r;
}

i32 p_setschedparam(JitCtx& j, gaddr handle, i32 policy, gp<i32> param) {
    pthread_t host = pthread_self();
    if (handle != j.thread->tls && !find_host(handle, &host)) return ESRCH;
    sched_param sp{};
    sp.sched_priority = param.a ? *param.get() : 0;
    int r = pthread_setschedparam(host, policy, &sp);
    if (r) ZLOGD("pthread_setschedparam(policy %d, prio %d) -> %d", policy, sp.sched_priority, r);
    return 0;  // best effort: priority changes are not essential
}

i32 p_attr_init(gaddr a) {
    wr<u32>(a + 0, 0);
    wr<u32>(a + 4, 0);
    wr<u32>(a + 8, kGuestDefaultStack);
    wr<u32>(a + 12, 4096);
    wr<u32>(a + 16, 0);
    wr<u32>(a + 20, 0);
    return 0;
}
i32 p_attr_destroy(gaddr) { return 0; }
i32 p_attr_setdetachstate(gaddr a, i32 state) {
    u32 f = rd<u32>(a);
    wr<u32>(a, state == 1 ? (f | 1) : (f & ~1u));
    return 0;
}
i32 p_attr_setstacksize(gaddr a, gsize size) {
    if (size < 8192) return EINVAL;
    wr<u32>(a + 8, size);
    return 0;
}

i32 s_get_priority_max(i32 policy) { return sched_get_priority_max(policy); }
i32 s_get_priority_min(i32 policy) { return sched_get_priority_min(policy); }
i32 s_yield() { return sched_yield(); }

// ------------------------------------------------------------ semaphores (count in the word)

i32 sem_init_(gaddr s, i32, u32 value) {
    wr<u32>(s, value);
    return 0;
}
i32 sem_post_(gaddr s) {
    __atomic_fetch_add(word(s), 1u, __ATOMIC_RELEASE);
    futex_wake(s, 1);
    return 0;
}
i32 sem_wait_(gaddr s) {
    u32* w = word(s);
    for (;;) {
        u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(w, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
            continue;
        }
        futex_wait(s, 0, nullptr);
    }
}
i32 sem_trywait_(JitCtx& j, gaddr s) {
    u32* w = word(s);
    u32 v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    while (v > 0) {
        if (__atomic_compare_exchange_n(w, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    }
    j.set_errno(EAGAIN);
    return -1;
}
i32 sem_close_(gaddr) { return 0; }
i32 sem_destroy_(gaddr) { return 0; }

}  // namespace

void register_pthread() {
    REG(pthread_mutex_init, p_mutex_init);
    REG(pthread_mutex_destroy, p_mutex_destroy);
    REG(pthread_mutex_lock, p_mutex_lock);
    REG(pthread_mutex_trylock, p_mutex_trylock);
    REG(pthread_mutex_unlock, p_mutex_unlock);
    REG(pthread_mutexattr_init, p_mutexattr_init);
    REG(pthread_mutexattr_destroy, p_mutexattr_destroy);
    REG(pthread_mutexattr_settype, p_mutexattr_settype);
    REG(pthread_cond_init, p_cond_init);
    REG(pthread_cond_destroy, p_cond_destroy);
    REG(pthread_cond_wait, p_cond_wait);
    REG(pthread_cond_timedwait, p_cond_timedwait);
    REG(pthread_cond_timedwait_relative_np, p_cond_timedwait_relative_np);
    REG(pthread_cond_signal, p_cond_signal);
    REG(pthread_cond_broadcast, p_cond_broadcast);
    REG(pthread_once, p_once);
    REG(pthread_key_create, p_key_create);
    REG(pthread_key_delete, p_key_delete);
    REG(pthread_getspecific, p_getspecific);
    REG(pthread_setspecific, p_setspecific);
    REG(pthread_create, p_create);
    REG(pthread_join, p_join);
    REG(pthread_detach, p_detach);
    REG(pthread_self, p_self);
    REG(pthread_exit, p_exit);
    REG(pthread_kill, p_kill);
    REG(pthread_setname_np, p_setname);
    REG(pthread_getschedparam, p_getschedparam);
    REG(pthread_setschedparam, p_setschedparam);
    REG(pthread_attr_init, p_attr_init);
    REG(pthread_attr_destroy, p_attr_destroy);
    REG(pthread_attr_setdetachstate, p_attr_setdetachstate);
    REG(pthread_attr_setstacksize, p_attr_setstacksize);
    REG(sched_get_priority_max, s_get_priority_max);
    REG(sched_get_priority_min, s_get_priority_min);
    REG(sched_yield, s_yield);
    REG(sem_init, sem_init_);
    REG(sem_post, sem_post_);
    REG(sem_wait, sem_wait_);
    REG(sem_trywait, sem_trywait_);
    REG(sem_close, sem_close_);
    REG(sem_destroy, sem_destroy_);
}

}  // namespace zb::hle
