#include "Discovery.h"

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include "Net.h"

namespace {

// Broadcast geral + o broadcast de cada placa de rede (o geral às vezes só sai pela rota padrão).
std::vector<ULONG> BroadcastAddresses() {
    std::vector<ULONG> out{INADDR_BROADCAST};
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, list, &size) != NO_ERROR)
            return out;
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            if (sin->sin_family != AF_INET || u->OnLinkPrefixLength == 0 || u->OnLinkPrefixLength >= 31) continue;
            ULONG ip = ntohl(sin->sin_addr.s_addr);
            ULONG mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
            out.push_back(htonl(ip | ~mask));
        }
    }
    return out;
}

}  // namespace

void Discovery::Start(Changed onChange) {
    m_onChange = std::move(onChange);
    m_stop = false;
    m_thread = std::thread([this] { Run(); });
}

void Discovery::Stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

std::vector<PhoneInfo> Discovery::Phones() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<PhoneInfo> v;
    for (auto& [id, p] : m_phones) v.push_back(p);
    return v;
}

void Discovery::Run() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    DWORD timeout = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    // Escuta na porta do protocolo para também ouvir os anúncios que o celular faz sozinho.
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(proto::UDP_PORT);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) {
        local.sin_port = 0;
        bind(s, (sockaddr*)&local, sizeof(local));
    }

    const char request[] = "GDCN?1";
    ULONGLONG lastSend = 0;
    while (!m_stop) {
        ULONGLONG now = GetTickCount64();
        if (now - lastSend >= 1500) {
            lastSend = now;
            for (ULONG addr : BroadcastAddresses()) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(proto::UDP_PORT);
                to.sin_addr.s_addr = addr;
                sendto(s, request, sizeof(request) - 1, 0, (sockaddr*)&to, sizeof(to));
            }
            // Esquece quem parou de responder.
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_lock);
                for (auto it = m_phones.begin(); it != m_phones.end();) {
                    if (now - it->second.lastSeen > 7000) {
                        it = m_phones.erase(it);
                        changed = true;
                    } else {
                        ++it;
                    }
                }
            }
            if (changed && m_onChange) m_onChange();
        }

        char buf[512];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        buf[n] = 0;
        // GDCN!1|<id>|<porta>|<nome>
        std::string text(buf, n);
        if (text.rfind("GDCN!1|", 0) != 0) continue;
        size_t a = text.find('|', 7), b = a == std::string::npos ? a : text.find('|', a + 1);
        if (b == std::string::npos) continue;
        PhoneInfo p;
        p.id = text.substr(7, a - 7);
        p.port = (uint16_t)atoi(text.substr(a + 1, b - a - 1).c_str());
        p.name = text.substr(b + 1);
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        p.ip = ip;
        p.lastSeen = GetTickCount64();
        if (p.id.size() != 32 || p.port == 0) continue;

        bool changed;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            auto it = m_phones.find(p.id);
            changed = it == m_phones.end() || it->second.ip != p.ip || it->second.name != p.name;
            m_phones[p.id] = p;
        }
        if (changed && m_onChange) m_onChange();
    }
    closesocket(s);
}
