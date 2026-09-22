# Media.net SRE — Socket Programming in C++
## One-Day Prep Guide

---

## Files in This Tutorial

| File | What it covers | Time to study |
|------|---------------|---------------|
| `00_concepts.md` | Core theory + syscall reference | 20 min |
| `01_echo_server.cpp` | socket/bind/listen/accept/recv/send | 30 min |
| `02_multi_group_chat.cpp` | Shared state, mutex, broadcast | 45 min |
| `03_chat_with_history.cpp` | Message history, replay on join | 45 min |
| `04_bonus_persistent_chat.cpp` | File persistence, 15-min rolling window | 30 min |
| `05_kv_server.cpp` | KV store (SET/GET/DEL), command parsing | 45 min |
| `06_kv_proxy.cpp` | Proxy, bidirectional relay, two-thread pattern | 30 min |
| `07_kv_load_balancer.cpp` | Round-robin LB, atomic counter, failover | 30 min |

**Total: ~4.5 hours reading + running. Spend remaining time coding from scratch.**

---

## Build All at Once

```bash
cd /path/to/tutorial

g++ -std=c++17 -pthread -o echo        01_echo_server.cpp
g++ -std=c++17 -pthread -o chat        02_multi_group_chat.cpp
g++ -std=c++17 -pthread -o chat_hist   03_chat_with_history.cpp
g++ -std=c++17 -pthread -o chat_bonus  04_bonus_persistent_chat.cpp
g++ -std=c++17 -pthread -o kv_server   05_kv_server.cpp
g++ -std=c++17 -pthread -o proxy       06_kv_proxy.cpp
g++ -std=c++17 -pthread -o lb          07_kv_load_balancer.cpp
```

---

## How to Test Each Program

### Obj 1 — Echo Server
```bash
./echo
# In another terminal:
telnet 127.0.0.1 8080
# Type anything → see "Echo: <message>"
```

### Obj 2 — Group Chat
```bash
./chat
# Terminal 2: telnet 127.0.0.1 8080  → alice → room1
# Terminal 3: telnet 127.0.0.1 8080  → bob   → room1
# Terminal 4: telnet 127.0.0.1 8080  → carol → room2
# alice + bob see each other; carol sees nothing from them
```

### Obj 3 — Chat with History
```bash
./chat_hist
# Terminal 2: connect as alice → room1 → send 3 messages
# Terminal 3: connect as bob  → room1 → sees alice's messages on join
```

### Bonus — Persistent Chat
```bash
./chat_bonus
# Send messages, Ctrl+C to stop, restart → history is restored
# Check: ls group_*.log   (log files created per group)
```

### KV Server
```bash
./kv_server
# Terminal 2:
nc 127.0.0.1 9090
SET user:32 alvida
GET user:32          # → alvida
DEL user:32
GET user:32          # → ERROR: key not found
```

### Proxy
```bash
./kv_server          # backend on 9090
./proxy              # proxy on 9091
nc 127.0.0.1 9091   # client talks to proxy
SET foo bar          # forwarded transparently to kv_server
```

### Load Balancer
```bash
# Start 3 KV servers — edit PORT in 05_kv_server.cpp and recompile 3 times
# OR use netcat as dummy backends:
nc -l 9090 &
nc -l 9091 &
nc -l 9092 &
./lb
nc 127.0.0.1 8888    # connection routed to one of the backends
```

---

## The Skeleton You Can Write From Memory in an Interview

```cpp
#include <iostream>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

void handle_client(int cfd) {
    char buf[4096];
    while (true) {
        int n = recv(cfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        // --- YOUR LOGIC HERE ---
        send(cfd, buf, n, 0);  // echo example
    }
    close(cfd);
}

int main() {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(8080); addr.sin_addr.s_addr = INADDR_ANY;
    bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sfd, 10);

    while (true) {
        int cfd = accept(sfd, nullptr, nullptr);
        if (cfd >= 0) std::thread(handle_client, cfd).detach();
    }
}
```

**This is your starting point for EVERY problem. Write it first, then extend.**

---

## Progression Map: How Each File Extends the Previous

```
01_echo_server
    │  add: shared groups map + mutex + broadcast
    ▼
02_multi_group_chat
    │  add: Msg struct with timestamp + store_and_broadcast + send_history
    ▼
03_chat_with_history
    │  add: persist to file + purge_old (15-min window)
    ▼
04_bonus_persistent_chat


05_kv_server  (independent — new problem, same skeleton)
    │  add: connect_to_backend + bidirectional relay threads
    ▼
06_kv_proxy
    │  replace single backend with vector<Backend> + round-robin counter
    ▼
07_kv_load_balancer
```

In the interview, **say this out loud**. Interviewers love seeing you think
incrementally rather than jump to the final solution.

---

## Key Things to Say in the Interview (Quick Reference)

**On SO_REUSEADDR:**
> "Without it, restarting the server within 60 seconds gives 'Address already
> in use' because the OS holds the port in TIME_WAIT. Always set it."

**On threading (detach vs join):**
> "detach() lets the client thread run freely. join() would block the accept
> loop, making the server single-threaded. In production I'd use a thread pool."

**On mutex + lock_guard:**
> "lock_guard is RAII — it locks on construction and unlocks when it goes out
> of scope, even if an exception is thrown. I never unlock manually."

**On holding locks during I/O:**
> "I copy the fd list under the lock, then release the lock before calling send().
> Holding a mutex during send() can block if the client's buffer is full,
> starving every other thread."

**On scaling:**
> "Thread-per-client doesn't scale past ~10k connections. For scale: epoll with
> non-blocking sockets (Linux), or use a framework like Boost.Asio. For chat at
> Slack-scale: Redis Pub/Sub for broadcast, Cassandra for history."

**On the KV store:**
> "This is a baby Redis. Real Redis uses a single-threaded event loop (no
> mutexes needed), handles 100k+ ops/sec, and supports persistence via RDB
> snapshots and AOF (append-only file)."

**On the load balancer:**
> "Round-robin works when all backends are identical. For a KV store, you also
> need consistent hashing so the same key always routes to the same server,
> or shared storage so every backend sees every key."

---

## What to Practice in the Last Hour

1. Close this file and retype `01_echo_server.cpp` from scratch in 10 minutes.
2. Extend it to `02_multi_group_chat.cpp` in 15 minutes.
3. Extend it to `03_chat_with_history.cpp` in 15 minutes.
4. Close everything and retype `05_kv_server.cpp` from scratch in 15 minutes.

If you can do all four, you will ace the round.
