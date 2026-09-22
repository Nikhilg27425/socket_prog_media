/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  FOLLOW-UP 1 — Proxy Server                                 ║
 * ║  Client ←→ Proxy ←→ KV Server                              ║
 * ║  Proxy forwards requests to the KV server and relays back.  ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 *  WHY A PROXY?  In production:
 *   - Adds a layer for caching, auth, rate limiting, logging
 *   - Hides the backend server's real IP/port from clients
 *   - Can be upgraded to a load balancer (see next file)
 *
 *  BUILD:  g++ -std=c++17 -pthread -o proxy 06_kv_proxy.cpp
 *
 *  RUN (in order):
 *    Terminal 1: ./kv_server          ← backend on port 9090
 *    Terminal 2: ./proxy              ← proxy on port 9091
 *    Terminal 3: nc 127.0.0.1 9091   ← client talks to proxy
 *    Type: SET foo bar   → proxy forwards to kv_server → relays OK back
 *
 *  Core idea:
 *    For each client connection to the proxy:
 *      1. Open a NEW connection to the backend server
 *      2. Spawn two threads: client→backend relay, backend→client relay
 *      3. When either side closes, close both connections
 */

#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int  PROXY_PORT    = 9091;
static const char BACKEND_IP[]  = "127.0.0.1";
static const int  BACKEND_PORT  = 9090;
static const int  BUF_SIZE      = 4096;

// ── Connect to the backend server ─────────────────────────────────────────────
// Returns the connected fd, or -1 on failure.
int connect_to_backend() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(BACKEND_PORT);
    inet_pton(AF_INET, BACKEND_IP, &addr.sin_addr);  // inet_pton: string→binary IP

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── One-direction relay ────────────────────────────────────────────────────────
// Reads from `src` and writes to `dst` in a loop until src closes.
// When this function returns, we signal shutdown by closing both fds.
void relay(int src, int dst) {
    char buf[BUF_SIZE];
    while (true) {
        int n = recv(src, buf, sizeof(buf), 0);
        if (n <= 0) break;           // src closed or errored
        int sent = 0;
        while (sent < n) {           // ensure all bytes are forwarded
            int k = send(dst, buf + sent, n - sent, 0);
            if (k <= 0) goto done;
            sent += k;
        }
    }
done:
    // Closing src and dst unblocks the other relay thread's recv()
    shutdown(src, SHUT_RDWR);
    shutdown(dst, SHUT_RDWR);
}

// ── Per-client handler ────────────────────────────────────────────────────────
void handle_client(int client_fd) {
    // Open a fresh connection to the backend for this client
    int backend_fd = connect_to_backend();
    if (backend_fd < 0) {
        std::string err = "ERROR: backend unavailable\r\n";
        send(client_fd, err.c_str(), err.size(), 0);
        close(client_fd);
        return;
    }

    std::cout << "Proxying: client_fd=" << client_fd
              << " ↔ backend_fd=" << backend_fd << "\n";

    // Two relay threads: one per direction
    // Thread A: client → backend
    // Thread B: backend → client
    std::thread a(relay, client_fd, backend_fd);
    std::thread b(relay, backend_fd, client_fd);

    // Wait for both to finish (one side closing triggers the other to close)
    a.join();
    b.join();

    close(client_fd);
    close(backend_fd);
    std::cout << "Proxy session closed\n";
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PROXY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Proxy on port " << PROXY_PORT
              << " → backend " << BACKEND_IP << ":" << BACKEND_PORT << "\n";

    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: Why two threads per proxied connection?
 * A: Data flows in both directions simultaneously. If you used one thread to
 *    read from client and forward, you'd miss data the backend is sending at
 *    the same time. Two threads, one per direction, is the simplest correct
 *    approach.
 *
 * Q: How does one side closing cause both relay threads to stop?
 * A: relay() calls shutdown(SHUT_RDWR) on both fds when its recv() returns 0.
 *    This causes the OTHER thread's recv() or send() to also fail/return 0,
 *    breaking its loop. Both threads then exit, join() returns, and we clean up.
 *
 * Q: What would you add to make this a caching proxy?
 * A: For GET commands: check a local cache (map<key,value>) first. Cache hit →
 *    return immediately without forwarding. Cache miss → forward, get result,
 *    store in cache, return to client. Invalidate on SET/DEL.
 *
 * Q: What is the difference between a proxy and a reverse proxy?
 * A: Forward proxy: sits in front of clients (e.g., company proxy that filters
 *    internet access). Reverse proxy: sits in front of servers (e.g., Nginx
 *    forwarding requests to app servers). This is a reverse proxy.
 *
 * Q: How would you add authentication to this proxy?
 * A: Read the first line from the client — treat it as a token. Validate it
 *    before opening the backend connection. If invalid, send error and close.
 */
