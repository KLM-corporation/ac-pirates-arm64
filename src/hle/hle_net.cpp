// Sockets and name resolution. sockaddr layouts are identical on both ABIs;
// structures that embed pointers, longs or timevals are converted.
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "../mem.h"
#include "hle.h"

namespace zb::hle {
namespace {

template <typename T>
T check(JitCtx& j, T r) {
    if (r < 0) sync_errno(j);
    return r;
}

bool is_timeval_opt(i32 level, i32 name) { return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO); }

i32 n_socket(JitCtx& j, i32 d, i32 t, i32 p) { return check(j, socket(d, t, p)); }
i32 n_connect(JitCtx& j, i32 fd, gaddr sa, u32 len) { return check(j, connect(fd, hp<sockaddr>(sa), len)); }
i32 n_bind(JitCtx& j, i32 fd, gaddr sa, u32 len) { return check(j, bind(fd, hp<sockaddr>(sa), len)); }
i32 n_listen(JitCtx& j, i32 fd, i32 backlog) { return check(j, listen(fd, backlog)); }
i32 n_accept(JitCtx& j, i32 fd, gaddr sa, gp<u32> len) {
    return check(j, accept(fd, sa ? hp<sockaddr>(sa) : nullptr, len.a ? len.get() : nullptr));
}
gssize n_send(JitCtx& j, i32 fd, gaddr buf, gsize n, i32 flags) {
    return check(j, static_cast<gssize>(send(fd, hp(buf), n, flags)));
}
gssize n_recv(JitCtx& j, i32 fd, gaddr buf, gsize n, i32 flags) {
    return check(j, static_cast<gssize>(recv(fd, hp(buf), n, flags)));
}
gssize n_sendto(JitCtx& j, i32 fd, gaddr buf, gsize n, i32 flags, gaddr sa, u32 len) {
    return check(j, static_cast<gssize>(sendto(fd, hp(buf), n, flags, sa ? hp<sockaddr>(sa) : nullptr, len)));
}
gssize n_recvfrom(JitCtx& j, i32 fd, gaddr buf, gsize n, i32 flags, gaddr sa, gp<u32> len) {
    return check(j, static_cast<gssize>(
                        recvfrom(fd, hp(buf), n, flags, sa ? hp<sockaddr>(sa) : nullptr, len.a ? len.get() : nullptr)));
}
i32 n_shutdown(JitCtx& j, i32 fd, i32 how) { return check(j, shutdown(fd, how)); }

i32 n_getsockopt(JitCtx& j, i32 fd, i32 level, i32 name, gaddr val, gp<u32> len) {
    if (is_timeval_opt(level, name)) {
        timeval tv{};
        socklen_t l = sizeof(tv);
        int r = getsockopt(fd, level, name, &tv, &l);
        if (r) return check(j, r);
        wr<i32>(val, static_cast<i32>(tv.tv_sec));
        wr<i32>(val + 4, static_cast<i32>(tv.tv_usec));
        *len.get() = 8;
        return 0;
    }
    return check(j, getsockopt(fd, level, name, hp(val), len.get()));
}

i32 n_setsockopt(JitCtx& j, i32 fd, i32 level, i32 name, gaddr val, u32 len) {
    if (is_timeval_opt(level, name) && len >= 8) {
        timeval tv{rd<i32>(val), rd<i32>(val + 4)};
        return check(j, setsockopt(fd, level, name, &tv, sizeof(tv)));
    }
    return check(j, setsockopt(fd, level, name, hp(val), len));
}

i32 n_getpeername(JitCtx& j, i32 fd, gaddr sa, gp<u32> len) {
    return check(j, getpeername(fd, hp<sockaddr>(sa), len.get()));
}
i32 n_getsockname(JitCtx& j, i32 fd, gaddr sa, gp<u32> len) {
    return check(j, getsockname(fd, hp<sockaddr>(sa), len.get()));
}

i32 n_select(JitCtx& j, i32 nfds, gaddr r, gaddr w, gaddr e, gp<GTimeval> tv) {
    timeval h{}, *ph = nullptr;
    if (tv.a) {
        h = {tv->tv_sec, tv->tv_usec};
        ph = &h;
    }
    int res = select(nfds, hp<fd_set>(r), hp<fd_set>(w), hp<fd_set>(e), ph);
    if (tv.a) *tv.get() = GTimeval{static_cast<i32>(h.tv_sec), static_cast<i32>(h.tv_usec)};
    return check(j, res);
}

i32 n_poll(JitCtx& j, gaddr fds, u32 n, i32 timeout) { return check(j, poll(hp<pollfd>(fds), n, timeout)); }

// Guest struct addrinfo (32 bytes) followed by its sockaddr and canonical name.
i32 n_getaddrinfo(gcstr node, gcstr service, gaddr hints_g, gp<u32> res) {
    addrinfo hints{}, *ph = nullptr;
    if (hints_g) {
        hints.ai_flags = rd<i32>(hints_g);
        hints.ai_family = rd<i32>(hints_g + 4);
        hints.ai_socktype = rd<i32>(hints_g + 8);
        hints.ai_protocol = rd<i32>(hints_g + 12);
        ph = &hints;
    }
    addrinfo* out = nullptr;
    int r = getaddrinfo(node.a ? node.get() : nullptr, service.a ? service.get() : nullptr, ph, &out);
    ZLOGD("getaddrinfo(%s, %s) -> %d", node.a ? node.get() : "-", service.a ? service.get() : "-", r);
    if (r) return r;
    gaddr head = 0, prev = 0;
    for (addrinfo* a = out; a; a = a->ai_next) {
        size_t cn = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
        gaddr g = mem::gcalloc(1, 32 + a->ai_addrlen + static_cast<u32>(cn));
        wr<i32>(g, a->ai_flags);
        wr<i32>(g + 4, a->ai_family);
        wr<i32>(g + 8, a->ai_socktype);
        wr<i32>(g + 12, a->ai_protocol);
        wr<u32>(g + 16, a->ai_addrlen);
        if (a->ai_addr) {
            memcpy(hp(g + 32), a->ai_addr, a->ai_addrlen);
            wr<u32>(g + 24, g + 32);
        }
        if (cn) {
            memcpy(hp(g + 32 + a->ai_addrlen), a->ai_canonname, cn);
            wr<u32>(g + 20, g + 32 + a->ai_addrlen);
        }
        if (prev) wr<u32>(prev + 28, g);
        else head = g;
        prev = g;
    }
    freeaddrinfo(out);
    *res.get() = head;
    return 0;
}

void n_freeaddrinfo(gaddr g) {
    while (g) {
        gaddr next = rd<u32>(g + 28);
        mem::gfree(g);
        g = next;
    }
}

i32 n_getnameinfo(gaddr sa, u32 salen, gstr host, u32 hostlen, gstr serv, u32 servlen, i32 flags) {
    return getnameinfo(hp<sockaddr>(sa), salen, host.a ? host.get() : nullptr, hostlen, serv.a ? serv.get() : nullptr,
                       servlen, flags);
}

u32 n_inet_addr(gcstr cp) { return inet_addr(cp); }
gaddr n_inet_ntoa(JitCtx& j, u32 in) {
    in_addr a{in};
    gaddr buf = tls_buffer(j, tls::kNtoa);
    snprintf(hp<char>(buf), 32, "%s", inet_ntoa(a));
    return buf;
}
i32 n_inet_pton(i32 af, gcstr src, gaddr dst) { return inet_pton(af, src, hp(dst)); }
gaddr n_if_indextoname(u32 idx, gstr buf) { return if_indextoname(idx, buf) ? buf.a : 0; }
i32 n_gethostname(JitCtx& j, gstr buf, gsize len) { return check(j, gethostname(buf, len)); }

}  // namespace

void register_net() {
    REG(socket, n_socket);
    REG(connect, n_connect);
    REG(bind, n_bind);
    REG(listen, n_listen);
    REG(accept, n_accept);
    REG(send, n_send);
    REG(recv, n_recv);
    REG(sendto, n_sendto);
    REG(recvfrom, n_recvfrom);
    REG(shutdown, n_shutdown);
    REG(getsockopt, n_getsockopt);
    REG(setsockopt, n_setsockopt);
    REG(getpeername, n_getpeername);
    REG(getsockname, n_getsockname);
    REG(select, n_select);
    REG(poll, n_poll);
    REG(getaddrinfo, n_getaddrinfo);
    REG(freeaddrinfo, n_freeaddrinfo);
    REG(getnameinfo, n_getnameinfo);
    REG(inet_addr, n_inet_addr);
    REG(inet_ntoa, n_inet_ntoa);
    REG(inet_pton, n_inet_pton);
    REG(if_indextoname, n_if_indextoname);
    REG(gethostname, n_gethostname);
}

}  // namespace zb::hle
