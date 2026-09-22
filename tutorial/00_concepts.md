# Socket Programming — 1-Day Crash Course

You have one day. Read this doc in 20 minutes, then go straight to the code.

---

## The Only Mental Model You Need

```
YOUR CODE
   │
   │  send(fd, "hello", 5)   ← writing to a socket is like writing to a file
   ▼
[ Socket File Descriptor ]   ← just an int (3, 4, 5...)
   │
   ▼
[ OS TCP/IP Stack ]           ← OS handles reliability, ordering, retransmit
   │
   ▼
[ Network → Other Machine ]
```

A **socket** is a file descriptor. You write bytes into it, bytes come out the other end. That's it.

---

## The 7 Syscalls You Will Use (memorize these)

```cpp
// 1. Create the socket
int fd = socket(AF_INET, SOCK_STREAM, 0);
//              ^IPv4    ^TCP          ^auto

// 2. Let port be reused immediately after server restart
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

// 3. Bind to a port
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family      = AF_INET;
addr.sin_port        = htons(8080);   // htons = host→network byte order
addr.sin_addr.s_addr = INADDR_ANY;    // accept on all interfaces
bind(fd, (struct sockaddr*)&addr, sizeof(addr));

// 4. Start accepting connections (server only)
listen(fd, 10);   // 10 = max queued connections

// 5. Accept one connection → returns a NEW fd for that client
int client_fd = accept(fd, nullptr, nullptr);

// 6. Send / Receive bytes
send(client_fd, "hello\r\n", 7, 0);
char buf[1024];
int n = recv(client_fd, buf, sizeof(buf)-1, 0);
// n == 0  → client disconnected
// n == -1 → error

// 7. Close
close(client_fd);
```

---

## Server Lifecycle (copy-paste this skeleton every time)

```
socket() → setsockopt() → bind() → listen() → loop { accept() → thread }
```

```cpp
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
listen(server_fd, 10);

while (true) {
    int client_fd = accept(server_fd, nullptr, nullptr);
    std::thread(handle_client, client_fd).detach();
}
```

---

## Threads + Mutex (the only concurrency you need)

```cpp
std::mutex mtx;
std::map<std::string, std::vector<int>> groups;  // shared state

// To access shared state safely:
{
    std::lock_guard<std::mutex> lock(mtx);  // locks here
    groups["room1"].push_back(client_fd);
}   // unlocks automatically when this block exits

// Spawn a thread per client:
std::thread(handle_client, client_fd).detach();
// detach() = thread runs independently, main loop keeps accepting
```

---

## Test Without Writing a Client

```bash
telnet 127.0.0.1 8080    # simulates a client, sends lines on Enter
nc 127.0.0.1 8080        # netcat, same thing but more flexible
```

---

## Compile Command (same for every file)

```bash
g++ -std=c++17 -pthread -o server filename.cpp && ./server
```

---

## The 3 Bugs That Will Kill You in an Interview

| Bug | Symptom | Fix |
|-----|---------|-----|
| Forgot `SO_REUSEADDR` | "Address already in use" on restart | Add `setsockopt` before `bind` |
| Not checking `recv` return value | Infinite loop / crash on disconnect | `if (n <= 0) break;` |
| Shared state without mutex | Random crashes with multiple clients | `std::lock_guard<std::mutex>` |

---

## Study Schedule for Today

| Time | Task |
|------|------|
| 20 min | Read this file |
| 30 min | Read + compile + test `01_echo_server.cpp` |
| 45 min | Read + compile + test `02_multi_group_chat.cpp` |
| 45 min | Read + compile + test `03_chat_with_history.cpp` |
| 30 min | Read `04_bonus_persistent_chat.cpp` (understand the pattern) |
| 45 min | Read + compile + test `05_kv_server.cpp` |
| 30 min | Read `06_kv_proxy.cpp` + `07_kv_load_balancer.cpp` |
| 45 min | Re-code `01` and `05` from scratch WITHOUT looking |
| Rest   | Re-read the "INTERVIEW Q&A" sections at bottom of each file |
