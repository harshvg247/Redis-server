Here is a complete, professional README.md file tailored specifically to the project you've built.

-----

# `cepoll-redis`

[](https://en.wikipedia.org/wiki/C_\(programming_language\))
[](https://www.linux.org/)
[](https://opensource.org/licenses/MIT)

A high-performance, event-driven Redis clone written in C. This server uses `epoll` for non-blocking I/O to manage thousands of concurrent connections on a single thread.

It implements the RESP protocol and supports strings, lists, sorted sets, and both passive and active key expiry.

## ✨ Features

  * **High-Performance I/O:** Built with a Linux `epoll` event loop for non-blocking, single-threaded concurrency.
  * **RESP Protocol:** Implements the Redis Serialization Protocol (RESP) for client communication.
  * **String Type:** Full support for `SET`, `GET`, `PING`, and `ECHO`.
  * **List Type:** Supports `RPUSH` and `LRANGE` for list operations.
  * **Sorted Set Type:** Supports `ZADD` and `ZRANGE` (rank queries) using a custom **rank-augmented AVL tree** (`zset.h`) for O(log N) operations.
  * **Key Expiry (TTL):**
      * **`SET ... PX`:** Full support for millisecond-precision expiry.
      * **Passive Eviction:** Expired keys are deleted on access (`GET`, `LRANGE`, etc.).
      * **Active Eviction:** A high-performance **min-heap** (`minheap.h`) actively purges expired keys in the background with O(1) lookup time for the next key to expire.

-----

## 🏛️ Architecture

This server operates on a single-threaded, event-driven model, just like Redis.

  * **Event Loop (`main.c`):** The core server uses `epoll_wait()` to efficiently manage all client connections. A 100ms timeout is used to ensure the active eviction loop runs periodically, even on an idle server.
  * **Parser (`parser.h`):** A lightweight, header-only parser for the RESP protocol. It handles parsing RESP arrays and bulk strings from the client.
  * **Handlers (`handler.h`):** All command logic (`handle_set`, `handle_get`, `handle_zadd`, etc.) and data structures are modularized in this header.
  * **Data Store:**
      * **Main Keyspace:** A `uthash` (hash table) maps string keys to a generic `db_entry` struct.
      * **Data Types:** The `db_entry` struct uses a `void*` and a `val_type` enum to store different data types (strings, lists, ZSETs).
      * **Sorted Sets:** Implemented using a custom, self-contained, rank-augmented AVL tree (`zset.h`) that provides O(log N) `ZADD` and `ZRANGE` (by rank) operations.
  * **Expiry System:** A dual-system is used for high performance:
    1.  **Passive Eviction:** `handle_get` (and others) will delete a key if it's found to be expired upon access.
    2.  **Active Eviction:** A global `minheap.h` stores `expiry_entry_t` structs. The main loop peeks at the heap (O(1)) and evicts expired keys. A "stale pointer check" correctly handles keys that were overwritten or passively-deleted, preventing use-after-free bugs.

-----

## 🚀 Getting Started

### Prerequisites

  * A C compiler (like `gcc` or `clang`)
  * A Linux environment (required for `epoll`)
  * `uthash.h` (must be in your include path or project directory)

### Compilation

This project uses header-only modules, so you only need to compile `main.c`. (You will also need your `utils.c` and `time_utils.c` if they are not header-only).

```bash
# Assuming utils.h, time_utils.h, parser.h, handler.h,
# minheap.h, and zset.h are all in the same directory.
gcc main.c -o cepoll-redis -O2
```

### Running

To start the server on the default port 6379:

```bash
./cepoll-redis
```

-----

## ⌨️ Usage (with `netcat`)

You can test the server using `netcat` (`nc`).

### PING/SET/GET

```bash
# Set a key
(printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n') | nc localhost 6379
# Expected: +OK

# Get a key
(printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n') | nc localhost 6379
# Expected: $3\r\nbar\r\n
```

### Lists (RPUSH/LRANGE)

```bash
# Push items to a list
(printf '*4\r\n$5\r\nRPUSH\r\n$6\r\nmylist\r\n$1\r\na\r\n$1\r\nb\r\n') | nc localhost 6379
# Expected: :2

# Get a range from the list
(printf '*4\r\n$6\r\nLRANGE\r\n$6\r\nmylist\r\n$1\r\n0\r\n$2\r\n-1\r\n') | nc localhost 6379
# Expected: *2\r\n$1\r\na\r\n$1\r\nb\r\n
```

### Sorted Sets (ZADD/ZRANGE)

```bash
# Add members to a leaderboard
(printf '*8\r\n$4\r\nZADD\r\n$5\r\nboard\r\n$3\r\n100\r\n$5\r\nalice\r\n$3\r\n200\r\n$3\r\nbob\r\n$2\r\n50\r\n$3\r\ndan\r\n') | nc localhost 6379
# Expected: :3

# Get the top 2 players (ranks 0 and 1, sorted by score)
(printf '*4\r\n$6\r\nZRANGE\r\n$5\r\nboard\r\n$1\r\n0\r\n$1\r\n1\r\n') | nc localhost 6379
# Expected: *2\r\n$3\r\ndan\r\n$5\r\nalice\r\n
```

### Active Expiry

```bash
# Set a key to expire in 500ms
(printf '*5\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$5\r\nhello\r\n$2\r\nPX\r\n$3\r\n500\r\n') | nc localhost 6379
# Expected: +OK

# Wait 1 second.
# In your server log, you should see: "Active evict: mykey"
sleep 1

# Try to get the key (it should be gone)
(printf '*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n') | nc localhost 6379
# Expected: $-1\r\n (nil)
```
