#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr int PORT = 9090;
constexpr const char* SERVER_IP = "127.0.0.1";
constexpr int CLIENT_COUNT = 10;
constexpr int REQUESTS_PER_CLIENT = 10;

std::mutex cout_mutex;

void clientTask(int client_id) {
    for (int i = 0; i < REQUESTS_PER_CLIENT; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { perror("socket"); continue; }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect"); close(sock); continue;
        }

        std::string msg = "Client " + std::to_string(client_id) + " request " + std::to_string(i);
        send(sock, msg.c_str(), msg.size(), 0);

        char buffer[1024] = {0};
        ssize_t n = read(sock, buffer, sizeof(buffer)-1);

        if (n > 0) {
            buffer[n] = '\0';
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[Client " << client_id << "] Received: " << buffer << std::endl;
        }

        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    std::vector<std::thread> clients;

    for (int i = 0; i < CLIENT_COUNT; ++i) {
        clients.emplace_back(clientTask, i);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto &t : clients) t.join();

    return 0;
}
