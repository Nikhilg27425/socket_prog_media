/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║  OBJECTIVE 3 — Chat Server with Full Message History    ║
 * ║  New joiner receives ALL previous messages in the group ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 *  BUILD:  g++ -std=c++17 -pthread -o chat_hist 03_chat_with_history.cpp
 *  RUN:    ./chat_hist
 *  TEST:
 *    Terminal 2: telnet 127.0.0.1 8080 → alice → room1 → send 3 messages
 *    Terminal 3: telnet 127.0.0.1 8080 → bob   → room1 → sees alice's history
 *
 *  Only change from Obj 2:
 *    Group now stores vector<ChatMessage> (timestamp + sender + text)
 *    On join: send the stored history FIRST, then enter the message loop
 */

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int PORT = 8080;

// ── A stored message ──────────────────────────────────────────────────────────
struct Msg {
    std::time_t ts;        // Unix timestamp
    std::string sender;
    std::string text;

    // Formatted as "[HH:MM:SS] alice: hello\r\n"
    std::string str() const {
        char tbuf[16];
        std::tm* t = std::localtime(&ts);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", t);
        return std::string("[") + tbuf + "] " + sender + ": " + text + "\r\n";
    }
};

// ── Group state ───────────────────────────────────────────────────────────────
struct Group {
    std::vector<int>  members;   // active client fds
    std::vector<Msg>  history;   // all messages (grows indefinitely here)
};

// ── Shared state ──────────────────────────────────────────────────────────────
std::mutex                   mtx;
std::map<std::string, Group> groups;

// ── Utilities ─────────────────────────────────────────────────────────────────
std::string trim(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? "" : s.substr(0, e + 1);
}

void safe_send(int fd, const std::string& m) { send(fd, m.c_str(), m.size(), 0); }

std::string read_line(int fd) {
    std::string l; char ch;
    while (recv(fd, &ch, 1, 0) > 0) {
        if (ch == '\n') break;
        if (ch != '\r') l += ch;
    }
    return l;
}

// ── Store message AND broadcast ───────────────────────────────────────────────
// Both steps happen logically together:
//   1. Append to history (under lock)
//   2. Collect fds      (under lock)
//   3. Send             (lock released — send can block)
void store_and_broadcast(const std::string& gid, const std::string& sender,
                         const std::string& text, int skip_fd) {
    Msg m { std::time(nullptr), sender, text };
    std::string formatted = m.str();

    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto& g = groups[gid];
        g.history.push_back(m);                   // ← THE NEW LINE vs Obj 2
        for (int fd : g.members)
            if (fd != skip_fd) targets.push_back(fd);
    }

    for (int fd : targets) safe_send(fd, formatted);
}

// ── Replay history to a newly joined client ───────────────────────────────────
void send_history(int client_fd, const std::string& gid) {
    // Copy the history snapshot while holding the lock, then send without lock
    std::vector<Msg> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = groups.find(gid);
        if (it != groups.end()) snapshot = it->second.history;
    }

    if (snapshot.empty()) return;

    safe_send(client_fd, "──── Chat History ────\r\n");
    for (const auto& m : snapshot) safe_send(client_fd, m.str());
    safe_send(client_fd, "─── End of History ───\r\n\r\n");
}

// ── Per-client thread ─────────────────────────────────────────────────────────
void handle_client(int cfd) {
    safe_send(cfd, "Username: ");
    std::string user = trim(read_line(cfd));
    if (user.empty()) { close(cfd); return; }

    safe_send(cfd, "Group ID: ");
    std::string gid = trim(read_line(cfd));
    if (gid.empty()) { close(cfd); return; }

    // Join group
    { std::lock_guard<std::mutex> lock(mtx); groups[gid].members.push_back(cfd); }

    // *** Replay history BEFORE announcing join ***
    send_history(cfd, gid);

    safe_send(cfd, "You joined '" + gid + "'!\r\n");

    // Announce join to others (not stored in history — system message)
    {
        std::vector<int> targets;
        { std::lock_guard<std::mutex> lock(mtx);
          for (int fd : groups[gid].members) if (fd != cfd) targets.push_back(fd); }
        for (int fd : targets) safe_send(fd, "[" + user + " joined]\r\n");
    }

    // Message loop
    char buf[4096];
    while (true) {
        memset(buf, 0, sizeof(buf));
        int n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        std::string msg = trim(std::string(buf));
        if (msg.empty()) continue;
        std::cout << "[" << gid << "] " << user << ": " << msg << "\n";
        store_and_broadcast(gid, user, msg, cfd);
    }

    // Cleanup
    {
        std::vector<int> targets;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto& g = groups[gid];
            g.members.erase(std::remove(g.members.begin(), g.members.end(), cfd), g.members.end());
            for (int fd : g.members) targets.push_back(fd);
            if (g.members.empty() && g.history.empty()) groups.erase(gid);
        }
        for (int fd : targets) safe_send(fd, "[" + user + " left]\r\n");
    }

    close(cfd);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Chat+history server on port " << PORT << "\n";
    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: Why copy history to `snapshot` before sending it?
 * A: Never hold a mutex during I/O. Sending can block if the client's TCP
 *    receive buffer is full (slow client). While blocked, every other thread
 *    trying to add a message or join the group would deadlock waiting for mtx.
 *    Pattern: lock → copy → unlock → do slow work.
 *
 * Q: History grows forever. How do you fix it?
 * A: Two options:
 *    1. Cap: keep only last N messages (use a deque, pop_front when size > N).
 *    2. Time window: keep only messages from the last 15 minutes (Bonus question).
 *       On each insert, also purge entries where ts < now - 900.
 *
 * Q: What is the difference between this and Objective 2?
 * A: Literally 2 lines:
 *    - Group struct stores a history vector.
 *    - store_and_broadcast() pushes to history before broadcasting.
 *    - send_history() is called once on join.
 *    That's it. Build incrementally in the interview — don't rewrite from scratch.
 */
