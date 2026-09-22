/*
 * ╔══════════════════════════════════════════════════════╗
 * ║  OBJECTIVE 1 — Echo Server                          ║
 * ║  Client sends a line → server sends it back         ║
 * ╚══════════════════════════════════════════════════════╝
 *
 *  BUILD:  g++ -std=c++17 -pthread -o echo 01_echo_server.cpp
 *  RUN:    ./echo
 *  TEST:   telnet 127.0.0.1 8080    ← type anything, see it echoed back
 *
 *  New concepts here:
 *    socket() bind() listen() accept() recv() send() — the full server loop
 *    One thread per client via std::thread
 */

#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int PORT = 8080;

// ── Utility: trim \r\n from telnet input ──────────────────────────────────────
std::string trim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

// ── This function runs in its own thread for EACH connected client ─────────────
void handle_client(int client_fd) {
    send(client_fd, "Echo server ready. Type something!\r\n", 36, 0);

    char buf[4096];
    while (true) {
        memset(buf, 0, sizeof(buf));
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);

        if (n <= 0) break;   // 0 = client disconnected, <0 = error → stop

        std::string msg = trim(std::string(buf));
        if (msg.empty()) continue;

        std::cout << "Received: " << msg << "\n";

        std::string response = "Echo: " + msg + "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
    }

    close(client_fd);
    std::cout << "Client disconnected (fd=" << client_fd << ")\n";
}

int main() {
    // 1. Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // 2. Allow immediate reuse of port after server restart
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. Bind to 0.0.0.0:8080
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));

    // 4. Start listening  (queue up to 10 pending connections)
    listen(server_fd, 10);
    std::cout << "Echo server on port " << PORT << ". Connect: telnet 127.0.0.1 " << PORT << "\n";

    // 5. Accept loop — each iteration handles ONE new client
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // Spawn a thread; pass client_fd by value so each thread owns its copy
        std::thread(handle_client, client_fd).detach();
        // detach() = don't wait for it; main thread loops back to accept()
    }

    close(server_fd);
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: What does SO_REUSEADDR do?
 * A: After a server closes, the OS keeps the port in TIME_WAIT for ~60s.
 *    Without this option, bind() fails with "Address already in use" during
 *    that window. SO_REUSEADDR lets you rebind immediately. Always set it.
 *
 * Q: Why does accept() return a NEW file descriptor?
 * A: server_fd is the "listening" socket — it never goes away. Each call to
 *    accept() creates a brand-new socket for THAT specific client. This lets
 *    you handle thousands of clients simultaneously while the server_fd keeps
 *    listening.
 *
 * Q: Why detach() instead of join()?
 * A: join() blocks the calling thread until the target thread finishes.
 *    If we join() in the accept loop, we'd handle clients one at a time.
 *    detach() lets each client thread run freely in parallel.
 *
 * Q: What if recv() only returns part of the message?
 * A: TCP is a STREAM protocol — it has no concept of message boundaries.
 *    A single send("hello world") might arrive as recv("hello") + recv(" world").
 *    For line-based protocols, read char-by-char until '\n'. For binary, prefix
 *    every message with a 4-byte length header.
 */
