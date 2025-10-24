#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <chrono>

constexpr int PORT = 9090;
constexpr int MAX_EVENTS = 1024;
constexpr int BUFFER_SIZE = 4096;
constexpr int THREAD_POOL_SIZE = 4;

// ==================== 线程池 =========================
class ThreadPool {
public:
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        cond.wait(lock, [this]{ return !tasks.empty(); });
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            tasks.push(std::move(task));
        }
        cond.notify_one();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable cond;
};

// ==================== 辅助函数 ======================
int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ==================== 客户端处理 =====================
void handleClient(int epoll_fd, int fd) {
    char buffer[BUFFER_SIZE];
    while (true) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            ssize_t total = 0;
            while (total < n) {
                ssize_t written = write(fd, buffer + total, n - total);
                if (written <= 0) break;
                total += written;
            }
        } else if (n == 0) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            break;
        }
    }
}

// ==================== epoll 事件循环 ==================
void epollLoop(int epoll_fd, int listen_fd, ThreadPool &pool) {
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            // 如果是监听 socket，处理 accept
            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break; // 没有更多连接
                        else {
                            perror("accept");
                            break;
                        }
                    }

                    setNonBlocking(client_fd);

                    struct epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLET;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else {
                // 普通客户端 fd，由线程池处理
                pool.enqueue([epoll_fd, fd] { handleClient(epoll_fd, fd); });
            }
        }
    }
}

// ==================== 主函数 ==========================
int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket failed"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed"); return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen failed"); return 1;
    }

    setNonBlocking(listen_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    ThreadPool pool(THREAD_POOL_SIZE);

    std::thread(epollLoop, epoll_fd, listen_fd, std::ref(pool)).detach();

    std::cout << "High-performance server listening on port " << PORT << std::endl;

    // 主线程休眠即可，所有 accept + client 交给 epollLoop
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));

    return 0;
}
