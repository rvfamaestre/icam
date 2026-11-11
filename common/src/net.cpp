#include "icam/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>

namespace icam::net {

bool Startup() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    });
    return ok;
}

namespace {

// Waits for readability (read=true) or writability. Returns 1 ready, 0 timeout, -1 error.
int WaitFor(SOCKET s, bool read, int timeout_ms) {
    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = read ? POLLRDNORM : POLLWRNORM;
    int r = WSAPoll(&pfd, 1, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
    // POLLHUP with pending data still allows a read that returns the data, then 0.
    return 1;
}

}  // namespace

bool Socket::Connect(const std::string& host, uint16_t port, int timeout_ms) {
    Startup();
    Close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        closesocket(s);
        return false;
    }
    int r = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return false;
    }
    if (r != 0) {
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, nullptr, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return false;
        }
        int err = 0;
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            closesocket(s);
            return false;
        }
    }
    // Stays non-blocking: every send/recv is bounded by an explicit poll deadline.
    s_ = static_cast<uintptr_t>(s);
    return true;
}

bool Socket::SendAll(const void* p, size_t n, int timeout_ms) {
    if (!Valid()) return false;
    const char* b = static_cast<const char*>(p);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    while (n > 0) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        if (WaitFor(static_cast<SOCKET>(s_), false, static_cast<int>(deadline - now)) != 1) return false;
        int chunk = n > (1 << 20) ? (1 << 20) : static_cast<int>(n);
        int r = send(static_cast<SOCKET>(s_), b, chunk, 0);
        if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (r <= 0) return false;
        b += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

int Socket::RecvSome(void* p, size_t n, int timeout_ms) {
    if (!Valid()) return -1;
    int w = WaitFor(static_cast<SOCKET>(s_), true, timeout_ms);
    if (w <= 0) return w;
    int chunk = n > (1 << 20) ? (1 << 20) : static_cast<int>(n);
    int r = recv(static_cast<SOCKET>(s_), static_cast<char*>(p), chunk, 0);
    if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
    return r > 0 ? r : -1;
}

bool Socket::RecvAll(void* p, size_t n, int timeout_ms) {
    char* b = static_cast<char*>(p);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    while (n > 0) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        int r = RecvSome(b, n, static_cast<int>(deadline - now));
        if (r == 0) continue;  // spurious wake-up; the deadline bounds the loop
        if (r < 0) return false;
        b += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void Socket::SetNoDelay() {
    BOOL on = TRUE;
    setsockopt(static_cast<SOCKET>(s_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void Socket::SetBuffers(int rcv, int snd) {
    if (rcv > 0) setsockopt(static_cast<SOCKET>(s_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    if (snd > 0) setsockopt(static_cast<SOCKET>(s_), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd), sizeof(snd));
}

void Socket::Close() {
    if (Valid()) {
        closesocket(static_cast<SOCKET>(s_));
        s_ = kInvalid;
    }
}

void Socket::Shutdown() {
    if (Valid()) shutdown(static_cast<SOCKET>(s_), SD_BOTH);
}

bool Socket::Listen(const std::string& host, uint16_t port) {
    Startup();
    Close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 4) != 0) {
        last_error_ = WSAGetLastError();
        closesocket(s);
        return false;
    }
    s_ = static_cast<uintptr_t>(s);
    return true;
}

Socket Socket::Accept(int timeout_ms) {
    if (!Valid() || WaitFor(static_cast<SOCKET>(s_), true, timeout_ms) != 1) return Socket();
    SOCKET c = accept(static_cast<SOCKET>(s_), nullptr, nullptr);
    if (c == INVALID_SOCKET) return Socket();
    u_long nb = 1;
    ioctlsocket(c, FIONBIO, &nb);
    return Socket(static_cast<uintptr_t>(c));
}

uint16_t Socket::LocalPort() const {
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(static_cast<SOCKET>(s_), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

}  // namespace icam::net
