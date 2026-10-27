#include "PairStore.h"

#include <shlobj.h>
#include <wincrypt.h>

#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "crypt32.lib")

std::wstring PairStore::Path() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    dir += L"\\GodoiCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\pairs.dat";
}

// Formato (antes do DPAPI), uma entrada por linha:
//   pc <idhex> | auto <0|1> | last <phoneId> | phone <id> <chave> <nome...>
void PairStore::Load() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::ifstream f(Path(), std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string text;
    if (!raw.empty()) {
        DATA_BLOB in{(DWORD)raw.size(), (BYTE*)raw.data()}, out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
            text.assign((char*)out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "pc") {
            std::string id;
            ls >> id;
            m_pcId = crypto::FromHex(id);
        } else if (kind == "auto") {
            int v = 1;
            ls >> v;
            m_auto = v != 0;
        } else if (kind == "last") {
            ls >> m_last;
        } else if (kind == "phone") {
            std::string id, key, name;
            ls >> id >> key;
            std::getline(ls, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1);
            m_phones[id] = {crypto::FromHex(key), name};
        }
    }
    if (m_pcId.size() != 16) {
        m_pcId = crypto::Random(16);
        Save();
    }
}

void PairStore::Save() {
    std::ostringstream s;
    s << "pc " << crypto::Hex(m_pcId) << "\n";
    s << "auto " << (m_auto ? 1 : 0) << "\n";
    if (!m_last.empty()) s << "last " << m_last << "\n";
    for (auto& [id, p] : m_phones) s << "phone " << id << " " << crypto::Hex(p.key) << " " << p.name << "\n";
    std::string text = s.str();
    DATA_BLOB in{(DWORD)text.size(), (BYTE*)text.data()}, out{};
    if (!CryptProtectData(&in, L"GodoiCam", nullptr, nullptr, nullptr, 0, &out)) return;
    std::ofstream f(Path(), std::ios::binary | std::ios::trunc);
    f.write((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
}

Bytes PairStore::PcId() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_pcId;
}

bool PairStore::Key(const std::string& phoneId, Bytes& key) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_phones.find(phoneId);
    if (it == m_phones.end()) return false;
    key = it->second.key;
    return true;
}

void PairStore::Put(const std::string& phoneId, const Bytes& key, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_lock);
    std::string clean = name;
    for (char& c : clean)
        if (c == '\n' || c == '\r') c = ' ';
    m_phones[phoneId] = {key, clean};
    Save();
}

void PairStore::Remove(const std::string& phoneId) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_phones.erase(phoneId)) Save();
}

void PairStore::Clear() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_phones.clear();
    m_last.clear();
    Save();
}

std::map<std::string, PairStore::Phone> PairStore::All() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_phones;
}

std::string PairStore::LastPhone() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_last;
}

void PairStore::SetLastPhone(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_last == id) return;
    m_last = id;
    Save();
}

bool PairStore::AutoConnect() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_auto;
}

void PairStore::SetAutoConnect(bool on) {
    std::lock_guard<std::mutex> lock(m_lock);
    m_auto = on;
    Save();
}
