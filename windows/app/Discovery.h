// Procura celulares com o GodoiCan aberto na rede local (broadcast UDP).
#pragma once

#include <winsock2.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PhoneInfo {
    std::string id;  // hex
    std::string name;
    std::string ip;
    uint16_t port = 0;
    ULONGLONG lastSeen = 0;
};

class Discovery {
public:
    using Changed = std::function<void()>;
    void Start(Changed onChange);
    void Stop();
    std::vector<PhoneInfo> Phones();

private:
    void Run();
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_lock;
    std::map<std::string, PhoneInfo> m_phones;
    Changed m_onChange;
};
