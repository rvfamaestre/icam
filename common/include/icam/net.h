#pragma once

// Thin RAII wrapper over a blocking Winsock TCP socket with explicit timeouts.

#include <cstddef>
#include <cstdint>
#include <string>

namespace icam::net {

bool Startup();  // WSAStartup once per process; safe to call repeatedly

class Socket {
public:
    Socket() = default;
    explicit Socket(uintptr_t s) : s_(s) {}
    ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : s_(o.s_) { o.s_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            Close();
            s_ = o.s_;
            o.s_ = kInvalid;
        }
        return *this;
    }

    // Connects to IPv4 host:port, waiting at most timeout_ms.
    bool Connect(const std::string& host, uint16_t port, int timeout_ms);
    // Sends everything or fails (timeout_ms per blocking wait).
    bool SendAll(const void* p, size_t n, int timeout_ms = 5000);
    // Reads up to n bytes. Returns >0 bytes read, 0 on timeout, -1 on error/EOF.
    int RecvSome(void* p, size_t n, int timeout_ms);
    // Reads exactly n bytes or fails.
    bool RecvAll(void* p, size_t n, int timeout_ms);
    void SetNoDelay();
    void SetBuffers(int rcv, int snd);
    void Close();
    // Unblocks a thread waiting in RecvSome from another thread.
    void Shutdown();
    bool Valid() const { return s_ != kInvalid; }
    uintptr_t Handle() const { return s_; }

    // Listening helpers (fakephone, tests).
    bool Listen(const std::string& host, uint16_t port);
    Socket Accept(int timeout_ms);
    uint16_t LocalPort() const;

    static constexpr uintptr_t kInvalid = ~uintptr_t(0);
    int last_error() const { return last_error_; }

private:
    uintptr_t s_ = kInvalid;
    int last_error_ = 0;
};

}  // namespace icam::net
