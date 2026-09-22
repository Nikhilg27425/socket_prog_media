/*
 * ╔══════════════════════════════════════════════════════╗
 * ║  OBJECTIVE 2 — Multi-Group Chat Server              ║
 * ║  Clients join a group by ID, chat only within group ║
 * ╚══════════════════════════════════════════════════════╝
 *
 *  BUILD:  g++ -std=c++17 -pthread -o chat 02_multi_group_chat.cpp
 *  RUN:    ./chat
 *  TEST:
 *    Terminal 2: telnet 127.0.0.1 8080  → username: alice  → group: room1
 *    Terminal 3: telnet 127.0.0.1 8080  → username: bob    → group: room1
 *    Terminal 4: telnet 127.0.0.1 8080  → username: carol  → group: room2
 *    alice and bob see each other's messages; carol is isolated.
 *
 *  New concepts vs Obj 1:
 *    Shared state: map<group_id → list of client fds>
 *    std::mutex to protect it
 *    Broadcasting: sending one message to many fds
 */

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>    // std::remove
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int PORT = 8080;

// ── Shared state ──────────────────────────────────────────────────────────────
// Key insight: multiple threads access `groups` concurrently.
// ANY read or write to groups MUST hold groups_mutex, or you get data races.
std::mutex                                  groups_mutex;
std::map<std::string, std::vector<int>>     groups;   // group_id → [client_fd, ...]

// ── Utilities ─────────────────────────────────────────────────────────────────
std::string trim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

void safe_send(int fd, const std::string& msg) {
    send(fd, msg.c_str(), msg.size(), 0);
}

// Read one line char-by-char — stops at \n or disconnect
std::string read_line(int fd) {
    std::string line;
    char ch;
    while (recv(fd, &ch, 1, 0) > 0) {
        if (ch == '\n') break;
        if (ch != '\r') line += ch;
    }
    return line;
}

// ── Broadcast to everyone in a group except the sender ────────────────────────
void broadcast(const std::string& group_id, const std::string& msg, int skip_fd) {
    // Step 1: collect targets while holding the lock (fast — just copies ints)
    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(groups_mutex);
        auto it = groups.find(group_id);
        if (it == groups.end()) return;
        for (int fd : it->second)
            if (fd != skip_fd) targets.push_back(fd);
    }
    // Step 2: send WITHOUT holding the lock
    // (send() can block if client's buffer is full — don't starve other threads)
    for (int fd : targets)
        safe_send(fd, msg);
}

// ── Per-client thread ─────────────────────────────────────────────────────────
void handle_client(int client_fd) {
    // Ask for username
    safe_send(client_fd, "Username: ");
    std::string username = trim(read_line(client_fd));
    if (username.empty()) { close(client_fd); return; }

    // Ask for group
    safe_send(client_fd, "Group ID: ");
    std::string group_id = trim(read_line(client_fd));
    if (group_id.empty()) { close(client_fd); return; }

    // Add this client to the group
    {
        std::lock_guard<std::mutex> lock(groups_mutex);
        groups[group_id].push_back(client_fd);
        // If group_id didn't exist, map creates an empty vector automatically
    }

    std::cout << username << " joined group '" << group_id << "'\n";
    safe_send(client_fd, "Joined '" + group_id + "'. Chat away!\r\n");
    broadcast(group_id, "[" + username + " joined]\r\n", client_fd);

    // Message loop
    char buf[4096];
    while (true) {
        memset(buf, 0, sizeof(buf));
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        std::string msg = trim(std::string(buf));
        if (msg.empty()) continue;

        std::cout << "[" << group_id << "] " << username << ": " << msg << "\n";
        broadcast(group_id, "[" + username + "]: " + msg + "\r\n", client_fd);
    }

    // Cleanup
    broadcast(group_id, "[" + username + " left]\r\n", client_fd);
    {
        std::lock_guard<std::mutex> lock(groups_mutex);
        auto& members = groups[group_id];
        members.erase(std::remove(members.begin(), members.end(), client_fd), members.end());
        if (members.empty()) groups.erase(group_id);
    }
    close(client_fd);
    std::cout << username << " disconnected\n";
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Group chat server on port " << PORT << "\n";

    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: Why do you copy the fds into `targets` before sending?
 * A: Holding a mutex while calling send() is a bad idea. send() can block if
 *    the receiving client's buffer is full. While blocked, NO other thread can
 *    acquire groups_mutex — the whole server stalls. Pattern: lock→copy→unlock→use.
 *
 * Q: Why store fds instead of objects in the map?
 * A: fds are just ints — trivially copyable, no memory management. The fd is
 *    all we need to call send()/close(). Username and group are local variables
 *    in handle_client, which is fine since that thread owns them.
 *
 * Q: What happens if you send to a disconnected fd?
 * A: send() returns -1 with errno=EPIPE, or the process gets SIGPIPE which
 *    kills it by default. In production, call signal(SIGPIPE, SIG_IGN) at
 *    startup and check send() return values.
 *
 * Q: Can two clients join the same group at the exact same millisecond?
 * A: Yes — lock_guard handles it. The second thread blocks at the lock until
 *    the first finishes pushing to the vector, then proceeds safely.
 *
 * Q: How would you let a client switch groups?
 * A: Detect command "/switch <new_group>", call leave current group cleanup,
 *    update group_id, then join the new group. Same code paths, just reused.
 */
