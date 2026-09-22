/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  BONUS — Persistent Chat + 15-Minute Rolling Window            ║
 * ║  1. Messages saved to disk → server restart restores state      ║
 * ║  2. New joiners only see messages from the last 15 minutes      ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 *  BUILD:  g++ -std=c++17 -pthread -o chat_bonus 04_bonus_persistent_chat.cpp
 *  RUN:    ./chat_bonus
 *  TEST:
 *    Send messages, Ctrl+C to kill server, restart — history is restored.
 *    Join after 15 min gap → old messages are not shown.
 *
 *  Two new ideas vs Obj 3:
 *    A) Persist: every message is appended to a file (group_<id>.log)
 *    B) 15-min window: filter history on read, purge on insert
 */

#include <iostream>
#include <fstream>
#include <sstream>
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

static const int PORT          = 8080;
static const int WINDOW_SECS   = 15 * 60;   // 15 minutes

// ── Message ───────────────────────────────────────────────────────────────────
struct Msg {
    std::time_t ts;
    std::string sender;
    std::string text;

    std::string str() const {
        char tbuf[16];
        std::tm* t = std::localtime(&ts);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", t);
        return std::string("[") + tbuf + "] " + sender + ": " + text + "\r\n";
    }

    // File format: "<unix_ts> <sender> <text>" (one line per message)
    std::string serialize() const {
        return std::to_string(ts) + " " + sender + " " + text + "\n";
    }

    static Msg deserialize(const std::string& line) {
        std::istringstream ss(line);
        Msg m;
        ss >> m.ts >> m.sender;
        std::getline(ss, m.text);
        if (!m.text.empty() && m.text[0] == ' ') m.text = m.text.substr(1);
        return m;
    }
};

// ── Group ─────────────────────────────────────────────────────────────────────
struct Group {
    std::vector<int>  members;
    std::vector<Msg>  history;   // in-memory; loaded from disk on first use
    bool              loaded = false;
};

// ── Shared state ──────────────────────────────────────────────────────────────
std::mutex                   mtx;
std::map<std::string, Group> groups;

// ── Disk helpers ──────────────────────────────────────────────────────────────
std::string log_path(const std::string& gid) { return "group_" + gid + ".log"; }

// Append one message to the group's log file (called under lock is fine — fast)
void persist(const std::string& gid, const Msg& m) {
    std::ofstream f(log_path(gid), std::ios::app);
    if (f) f << m.serialize();
}

// Load history from disk, keeping only messages within the last WINDOW_SECS
void load_from_disk(Group& g, const std::string& gid) {
    if (g.loaded) return;
    g.loaded = true;

    std::ifstream f(log_path(gid));
    if (!f) return;  // no file yet → fresh start

    std::time_t cutoff = std::time(nullptr) - WINDOW_SECS;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        Msg m = Msg::deserialize(line);
        if (m.ts >= cutoff) g.history.push_back(m);
    }
    std::cout << "[disk] Loaded " << g.history.size()
              << " recent messages for group '" << gid << "'\n";
}

// ── Purge expired messages from in-memory history ─────────────────────────────
// Called on every insert — keeps the vector from growing forever.
void purge_old(std::vector<Msg>& history) {
    std::time_t cutoff = std::time(nullptr) - WINDOW_SECS;
    history.erase(
        std::remove_if(history.begin(), history.end(),
                       [&](const Msg& m){ return m.ts < cutoff; }),
        history.end()
    );
}

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

// ── Store + persist + broadcast ───────────────────────────────────────────────
void store_and_broadcast(const std::string& gid, const std::string& sender,
                         const std::string& text, int skip_fd) {
    Msg m { std::time(nullptr), sender, text };
    std::string formatted = m.str();

    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto& g = groups[gid];
        purge_old(g.history);         // keep only last 15 min
        g.history.push_back(m);
        persist(gid, m);              // write to disk
        for (int fd : g.members)
            if (fd != skip_fd) targets.push_back(fd);
    }
    for (int fd : targets) safe_send(fd, formatted);
}

// ── Send recent history to new joiner ─────────────────────────────────────────
void send_history(int cfd, const std::string& gid) {
    std::vector<Msg> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto& g = groups[gid];
        load_from_disk(g, gid);  // lazy-load on first join
        std::time_t cutoff = std::time(nullptr) - WINDOW_SECS;
        for (const auto& m : g.history)
            if (m.ts >= cutoff) snapshot.push_back(m);
    }

    if (snapshot.empty()) return;
    safe_send(cfd, "──── Last 15 min ────\r\n");
    for (const auto& m : snapshot) safe_send(cfd, m.str());
    safe_send(cfd, "──── End ────\r\n\r\n");
}

// ── Per-client thread ─────────────────────────────────────────────────────────
void handle_client(int cfd) {
    safe_send(cfd, "Username: ");
    std::string user = trim(read_line(cfd));
    if (user.empty()) { close(cfd); return; }

    safe_send(cfd, "Group ID: ");
    std::string gid = trim(read_line(cfd));
    if (gid.empty()) { close(cfd); return; }

    { std::lock_guard<std::mutex> lock(mtx); groups[gid].members.push_back(cfd); }

    send_history(cfd, gid);
    safe_send(cfd, "You joined '" + gid + "'!\r\n");

    {
        std::vector<int> targets;
        { std::lock_guard<std::mutex> lock(mtx);
          for (int fd : groups[gid].members) if (fd != cfd) targets.push_back(fd); }
        for (int fd : targets) safe_send(fd, "[" + user + " joined]\r\n");
    }

    char buf[4096];
    while (true) {
        memset(buf, 0, sizeof(buf));
        int n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        std::string msg = trim(std::string(buf));
        if (msg.empty()) continue;
        store_and_broadcast(gid, user, msg, cfd);
    }

    {
        std::vector<int> targets;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto& g = groups[gid];
            g.members.erase(std::remove(g.members.begin(), g.members.end(), cfd), g.members.end());
            for (int fd : g.members) targets.push_back(fd);
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

    std::cout << "Persistent chat server on port " << PORT << "\n";
    std::cout << "Messages saved to group_<id>.log files\n";

    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: Why append to a log file instead of a database?
 * A: For an interview, log files are simpler and fast enough. In production,
 *    you'd use a DB like PostgreSQL (for querying) or Redis (for speed) or
 *    Cassandra (for time-series chat data at scale). The interviewer will ask
 *    this — have an answer ready.
 *
 * Q: Why lazy-load history (load_from_disk called on first join)?
 * A: If you load all groups at startup, you waste memory for groups that might
 *    never be accessed in this session. Lazy-loading is a standard pattern.
 *
 * Q: The purge_old() function runs on every message. Is that slow?
 * A: O(n) where n = messages in the last 15 minutes. In a busy group that
 *    could be thousands. A better approach: use a deque and only pop_front
 *    when the oldest element is expired, making it O(1) per insert.
 *
 * Q: How would you scale this to multiple server instances?
 * A: The in-memory state becomes a bottleneck. Replace it with:
 *    - Redis Pub/Sub for real-time broadcast across server instances
 *    - A shared DB for history (PostgreSQL, Cassandra)
 *    - A message queue (Kafka) for durability and replay
 *    That's the production architecture for apps like Slack.
 */
