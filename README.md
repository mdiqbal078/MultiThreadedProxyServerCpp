# Multi-Threaded Proxy Server with LRU Cache (Modern C++20)

A high-performance, multi-threaded HTTP proxy server implemented in modern C++20. This project acts as an intermediary for HTTP requests, forwarding them to remote servers and caching the responses in memory to accelerate subsequent requests and reduce bandwidth usage.

This project was completely refactored from a legacy C implementation into a robust, memory-safe, and highly efficient C++ application.

## 🚀 Key Features

*   **Multi-Threading (C++20):** Utilizes native `std::thread` to handle multiple client connections concurrently without blocking.
*   **Concurrency Control:** Employs C++20's `std::counting_semaphore` to strictly limit the maximum number of active client connections (default: 400), preventing resource exhaustion.
*   **O(1) LRU Cache:** Implements a highly efficient Least Recently Used (LRU) cache using a combination of `std::list` and `std::unordered_map`. This allows for instant $O(1)$ lookups and cache updates, completely bypassing the $O(N)$ bottleneck of traditional linked-list approaches.
*   **Canonical URL Caching:** Parses incoming HTTP requests and builds canonical URLs (stripping browser-specific headers like User-Agent) to ensure high cache hit rates across different browsers.
*   **Memory Safety (RAII & STL):** 
    *   Zero manual memory management (no `malloc` or `free`).
    *   Uses `std::string`, `std::vector`, and `std::string_view` for fast, zero-copy buffer parsing.
    *   Implements a custom RAII `Socket` wrapper class to ensure file descriptors are always safely closed, even in the event of exceptions or early returns, eliminating resource leaks.
*   **Thread-Safe Networking:** Replaced deprecated, thread-unsafe POSIX functions (like `gethostbyname`) with modern, thread-safe alternatives (`getaddrinfo`) supporting both IPv4 and IPv6.

## 🛠️ Architecture

1.  **Main Thread:** Listens for incoming TCP connections on a specified port.
2.  **Worker Threads:** Upon accepting a connection, a new detached `std::thread` is spawned.
3.  **Request Handling:**
    *   The worker reads the raw bytes and parses the HTTP GET request using `proxy_parse`.
    *   It checks the `LRUCache` (protected by a `std::mutex`).
    *   **Cache Hit:** If the requested URL is cached, the proxy instantly streams the data back to the client from memory.
    *   **Cache Miss:** If not cached, the proxy connects to the remote upstream server, forwards the request, streams the incoming payload back to the client, and simultaneously saves it to the LRU cache.

## 📦 Prerequisites

*   A C++ compiler with **C++20 support** (e.g., GCC 11+, Clang 13+).
*   `make` utility.
*   Linux or macOS environment (uses POSIX sockets).

## 🚀 Building and Running

1.  **Clone the repository:**
    ```bash
    git clone <your-repository-url>
    cd MultiThreadedProxyServerClient
    ```

2.  **Compile the project:**
    ```bash
    make
    ```

3.  **Run the proxy server:**
    ```bash
    # Run on default port 8080 or specify your own
    ./proxy 8080
    ```

## 🧪 Testing the Proxy

Configure your web browser or system network settings to use `127.0.0.1` and port `8080` (or your chosen port) as an HTTP proxy. 

Alternatively, test using `curl`:
```bash
curl -x http://127.0.0.1:8080 http://www.example.com
```

You will see console output indicating whether a request resulted in a "Cache MISS" (fetching from the internet) or a "Cache HIT" (serving instantly from memory).

## 🧹 Cleanup
To remove compiled object files and the executable:
```bash
make clean
```
