/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  Media.net Machine Coding — TCP Key-Value Store Server      ║
 * ║  Supports:  SET key value                                   ║
 * ║             GET key                                         ║
 * ║             DEL key                                         ║
 * ║  Invalid commands return an error message.                  ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 *  BUILD:  g++ -std=c++17 -pthread -o kv_server 05_kv_server.cpp
 *  RUN:    ./kv_server
 *  TEST (in another terminal):
 *    nc 127.0.0.1 9090
 *    > SET user:32 alvida
 *    OK
 *    > GET user:32
 *    alvida
 *    > DEL user:32
 *    OK
 *    > GET user:32
 *    ERROR: key not found
 *    > BLAH foo
 *    ERROR: unknown command
 *
 *  This is essentially a baby Redis.
 *  New concept: parsing text commands, thread-safe map as the store.
 */

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <sstream>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const int PORT = 9090;

// ── The key-value store ───────────────────────────────────────────────────────
// std::map<string, string> is our in-memory database.
// All access must hold kv_mutex.
std::mutex               kv_mutex;
std::map<std::string, std::string> kv_store;

// ── Utilities ─────────────────────────────────────────────────────────────────
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Split a string by whitespace into tokens
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);
    return tokens;
}

// Read exactly one line from fd (\n terminated)
std::string read_line(int fd) {
    std::string line;
    char ch;
    while (recv(fd, &ch, 1, 0) > 0) {
        if (ch == '\n') break;
        if (ch != '\r') line += ch;
    }
    return line;
}

void reply(int fd, const std::string& msg) {
    std::string r = msg + "\r\n";
    send(fd, r.c_str(), r.size(), 0);
}

// ── Command processor ─────────────────────────────────────────────────────────
// Returns the response string for a given command line.
// All KV operations happen here, under kv_mutex.
std::string process(const std::string& line) {
    std::vector<std::string> tokens = split(line);
    if (tokens.empty()) return "";

    std::string cmd = tokens[0];
    // Convert to uppercase for case-insensitive matching
    for (char& c : cmd) c = toupper(c);

    if (cmd == "SET") {
        // SET key value  (value can be a single token)
        if (tokens.size() < 3) return "ERROR: SET requires key and value";
        std::string key = tokens[1];
        std::string val = tokens[2];
        {
            std::lock_guard<std::mutex> lock(kv_mutex);
            kv_store[key] = val;
        }
        return "OK";

    } else if (cmd == "GET") {
        if (tokens.size() < 2) return "ERROR: GET requires a key";
        std::string key = tokens[1];
        {
            std::lock_guard<std::mutex> lock(kv_mutex);
            auto it = kv_store.find(key);
            if (it == kv_store.end()) return "ERROR: key not found";
            return it->second;
        }

    } else if (cmd == "DEL") {
        if (tokens.size() < 2) return "ERROR: DEL requires a key";
        std::string key = tokens[1];
        {
            std::lock_guard<std::mutex> lock(kv_mutex);
            if (kv_store.erase(key) == 0) return "ERROR: key not found";
            return "OK";
        }

    } else {
        return "ERROR: unknown command '" + tokens[0] + "'. Use SET/GET/DEL";
    }
}

// ── Per-client thread ─────────────────────────────────────────────────────────
void handle_client(int cfd) {
    reply(cfd, "KV Server ready. Commands: SET <key> <val> | GET <key> | DEL <key>");

    while (true) {
        std::string line = read_line(cfd);
        if (line.empty()) break;    // disconnect

        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        std::cout << "CMD: " << trimmed << "\n";

        std::string response = process(trimmed);
        if (!response.empty()) reply(cfd, response);
    }

    close(cfd);
    std::cout << "Client disconnected\n";
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

    std::cout << "KV Server on port " << PORT << "\n";
    std::cout << "Test: nc 127.0.0.1 " << PORT << "\n";

    while (true) {
        int cfd = accept(server_fd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}

/*
 * ── INTERVIEW Q&A ─────────────────────────────────────────────────────────────
 *
 * Q: Why std::map and not std::unordered_map?
 * A: unordered_map gives O(1) average for get/set/del vs O(log n) for map.
 *    For a KV store, unordered_map is the right choice. I used map here for
 *    simplicity; swap it in one line for the interview.
 *
 * Q: Is this thread-safe? What's the granularity of your lock?
 * A: Yes. I use a single mutex (coarse-grained lock) around all KV operations.
 *    This means SET and GET can't run concurrently, even if they touch different
 *    keys. A finer approach: std::shared_mutex (reader-writer lock) — multiple
 *    GET calls can proceed in parallel, only SET/DEL need exclusive access.
 *    In C++17: shared_lock for GET, unique_lock for SET/DEL.
 *
 * Q: What if value contains spaces? e.g. SET key hello world
 * A: Current code only takes tokens[2]. To support multi-word values, replace
 *    the split approach with: take everything after "SET key " as the value.
 *    Fix: std::string val = line.substr(line.find(tokens[1]) + tokens[1].size() + 1)
 *
 * Q: How would you make this persistent?
 * A: Same as the bonus chat: append each SET/DEL to a log file. On startup,
 *    replay the log. This is exactly how Redis AOF (Append-Only File) works.
 *
 * Q: How would you add expiry (TTL)?
 * A: Store map<string, {value, expiry_time}>. On GET, check if expiry_time <
 *    now — if so, delete and return "not found". A background thread can
 *    periodically scan and purge expired keys (lazy vs eager expiry).
 */
