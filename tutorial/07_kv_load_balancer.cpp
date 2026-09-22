/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  FOLLOW-UP 2 — Load Balancer (Round-Robin)                     ║
 * ║  Client → Load Balancer → one of [Server1, Server2, Server3]   ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 *  WHY A LOAD BALANCER?
 *   - No single server is a bottleneck
 *   - If one server goes down, others keep serving
 *   - Scales horizontally by adding more backends
 *
 *  Round-Robin strategy: distribute requests in order (1,2,3,1,2,3...)
 *  Other strategies: least-connections, IP-hash, weighted (discuss in interview)
 *
 *  BUILD:  g++ -std=c++17 -pthread -o lb 07_kv_load_balancer.cpp
 *
 *  RUN (start multiple KV servers on different ports first):
 *    Terminal 1: PORT=9090 ./kv_server    (edit PORT in 05_kv_server.cpp, recompile)
 *    Terminal 2: PORT=9091 ./kv_server
 *    Terminal 3: PORT=9092 ./kv_server
 *    Terminal 4: ./lb
 *    Terminal 5: nc 127.0.0.1 8888   → all commands go through the LB
 *
 *  For quick testing without 3 servers: nc can act as a dummy backend.
 *    Terminal 1: nc -l 9090
 *    Terminal 2: nc -l 9091
 *    Terminal 3: nc -l 9092
 *    Terminal 4: ./lb
 */

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int LB_PORT  = 8888;
static const int BUF_SIZE = 4096;

// ── Backend server descriptor ─────────────────────────────────────────────────
struct Backend {
    std::string host;
    int         port;
};

// ── Backend pool ──────────────────────────────────────────────────────────────
const std::vector<Backend> BACKENDS = {
    {"127.0.0.1", 9090},
    {"127.0.0.1", 9091},
    {"127.0.0.1", 9092},
};

// ── Round-Robin counter ───────────────────────────────────────────────────────
// std::atomic<int> is thread-safe without a mutex — perfect for a counter.
// fetch_add returns the OLD value and increments atomically.
std::atomic<int> rr_counter{0};

Backend pick_backend() {
    int idx = rr_counter.fetch_add(1) % (int)BACKENDS.size();
    return BACKENDS[idx];
}

// ── Connect to a specific backend ─────────────────────────────────────────────
int connect_to(const Backend& b) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(b.port);
    inet_pton(AF_INET, b.host.c_str(), &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── One-direction relay (same as proxy) ───────────────────────────────────────
void relay(int src, int dst) {
    char buf[BUF_SIZE];
    while (true) {
        int n = recv(src, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int k = send(dst, buf + sent, n - sent, 0);
            if (k <= 0) goto done;
            sent += k;
        }
    }
done:
    shutdown(src, SHUT_RDWR);
    shutdown(dst, SHUT_RDWR);
}

// ── Per-client handler ────────────────────────────────────────────────────────
void handle_client(int client_fd) {
    // Try to pick a healthy backend (try each in round-robin order)
    int backend_fd = -1;
    Backend chosen;

    // Try up to BACKENDS.size() times to find an available backend
    for (size_t i = 0; i < BACKENDS.size(); ++i) {
        chosen     = pick_backend();
        backend_fd = connect_to(chosen);
        if (backend_fd >= 0) break;
        std::cout << "Backend " << chosen.host << ":" << chosen.port
                  << " unreachable, trying next...\n";
    }

    if (backend_fd < 0) {
        std::string err = "ERROR: no backend available\r\n";
        send(client_fd, err.c_str(), err.size(), 0);
        close(client_fd);
        return;
    }

    std::cout << "Routing client_fd=" << client_fd
              << " → " << chosen.host << ":" << chosen.port << "\n";

    // Bidirectional relay — identical to the proxy
    std::thread a(relay, client_fd, backend_fd);
    std::thread b(relay, backend_fd, client_fd);
    a.join();
    b.join();

    close(client_fd);
    close(backend_fd);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(LB_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Load Balancer on port " << LB_PORT << "\n";
    std::cout << "Backends: ";
    for (const auto& b : BACKENDS) std::cout << b.host << ":" << b.port << " ";
    std::cout << "\n";

    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: What's the difference between a proxy and a load balancer?
 * A: A proxy forwards to ONE backend. A load balancer distributes across
 *    MULTIPLE backends using a strategy. This LB uses round-robin. The proxy
 *    file (06) is a special case with exactly one backend.
 *
 * Q: What other load balancing strategies exist?
 * A: - Round-Robin: rotate through backends in order (implemented here)
 *    - Least Connections: pick backend with fewest active connections
 *      (add atomic counter per backend, increment on connect, decrement on close)
 *    - IP Hash: hash client IP → always same client goes to same backend
 *      (useful for session stickiness)
 *    - Weighted: backend A gets 70% of requests, B gets 30% (for heterogeneous servers)
 *
 * Q: How do you handle a backend going down?
 * A: Current code tries the next backend if connect() fails. For production:
 *    - Health checks: background thread periodically connects to each backend
 *      and marks it UP/DOWN. Only route to UP backends.
 *    - Circuit breaker: after N consecutive failures, stop routing to that
 *      backend for a cooldown period.
 *
 * Q: This LB is stateless — is that a problem for the KV store?
 * A: Yes! If client sets "foo=bar" on Server1, then a subsequent GET "foo" is
 *    routed to Server2 — Server2 doesn't have it. Solutions:
 *    1. Consistent hashing: hash(key) determines which backend handles it
 *       (same key ALWAYS goes to same server).
 *    2. Shared storage: all backends read/write a common DB.
 *    3. Replication: backends sync state with each other.
 *
 * Q: Why std::atomic for the round-robin counter instead of a mutex?
 * A: Incrementing a simple integer is cheaper with atomic than with a mutex.
 *    std::atomic<int>::fetch_add is a single CPU instruction (LOCK XADD on x86).
 *    A mutex involves OS calls and thread context switches.
 */
