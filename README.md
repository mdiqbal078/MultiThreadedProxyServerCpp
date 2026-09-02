# Multi-Threaded Proxy Server with LRU Cache (C++20 Multi-Reactor Epoll)

A high-performance, completely non-blocking, multi-threaded HTTP proxy server engineered in modern C++20. This project acts as an intermediary for HTTP/HTTPS requests, forwarding them to remote servers and caching responses in memory to accelerate subsequent requests and reduce bandwidth usage.

This project was recently refactored from a `std::thread`-per-connection model into a highly robust **Multi-Reactor Event-Driven Architecture**, capable of handling thousands of concurrent connections with zero memory leaks and a microsecond-latency caching engine.

## 🚀 Key Architectural Features

*   **Multi-Reactor Architecture (epoll / kqueue):** Abandons the legacy thread-per-connection model. Spawns $N$ dedicated reactor threads (matching your CPU cores). Each thread runs its own non-blocking event loop, and the OS load-balances connections across them using Linux's `SO_REUSEPORT`.
*   **100% Non-Blocking I/O:** Every socket is strictly non-blocking. A custom `ConnectionState` machine and robust `buffer.hpp` handle `EAGAIN` partial writes seamlessly, preventing the proxy from ever locking up when dealing with slow clients or upstream servers.
*   **Thread-Safe $O(1)$ LRU Cache:** Implements a highly efficient Least Recently Used (LRU) cache using a combination of `std::list` and `std::unordered_map`, protected by a `std::mutex`. Achieves **<1ms median latency** for cached resources under extreme load.
*   **Asynchronous DNS Thread Pool:** Offloads blocking `getaddrinfo` syscalls to a dedicated thread pool. When DNS resolves, a pipe notifies the epoll loop, ensuring the reactor thread never freezes.
*   **HTTPS `CONNECT` Tunneling:** Intercepts `CONNECT` requests to function as a blind TCP relay for encrypted HTTPS traffic.
*   **Security & Resilience:** 
    *   **Idle Reaper:** Periodically sweeps and closes dead connections to prevent file descriptor leaks.
    *   **Memory Safety:** Built and verified with AddressSanitizer (`-fsanitize=address`) to guarantee zero leaks in manual state management.
*   **Live JSON Metrics Endpoint:** Exposes a lock-free atomic dashboard on `http://localhost:8081/` tracking Cache Hit Ratio, Eviction Latency, and Active Connections in real-time.

## 🧪 Benchmark Results

Tested with ApacheBench (`ab`) using 500 requests at 50 concurrency:
*   **Median Latency (50% of requests):** 0 ms (Served instantly from RAM).
*   **Cache Hit Ratio:** 90% (under benchmark load).
*   **Throughput Reliability:** Handled full concurrency gracefully without dropped connections.

## 📦 Prerequisites

*   A C++ compiler with **C++20 support** (e.g., GCC 11+, Clang 13+).
*   `make` utility.
*   Linux (uses `epoll`) or macOS (uses `kqueue`).

## 🚀 Building and Running

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/mdiqbal078/MultiThreadedProxyServerCpp.git
    cd MultiThreadedProxyServerCpp
    ```

2.  **Compile the project:**
    (Compiles with `-fsanitize=address` for memory safety guarantees)
    ```bash
    make asan
    ```

3.  **Run the proxy server:**
    ```bash
    # Runs the proxy on port 8080 and the metrics dashboard on port 8081
    ./proxy 8080
    ```

## 📊 Live Metrics Dashboard
While the proxy is running, you can pull live JSON metrics from the server at any time:
```bash
curl -s http://localhost:8081/
```
Example Output:
```json
{
  "total_requests": 15230,
  "active_connections": 1000,
  "cache_hits": 14200,
  "cache_hit_ratio_pct": 93.2,
  "avg_eviction_latency_us": 0.42
}
```

## 🧪 Testing the Proxy

Configure your web browser or system network settings to use `127.0.0.1` and port `8080` (or your chosen port) as an HTTP proxy. 

Alternatively, test using `curl`:
```bash
curl -v -x http://127.0.0.1:8080 http://neverssl.com/
```

## 🧹 Cleanup
To remove compiled object files and the executable:
```bash
make clean
```
