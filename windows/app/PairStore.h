// Identidade do PC e chaves dos celulares pareados, protegidas com DPAPI do usuário.
#pragma once

#include <map>
#include <mutex>
#include <string>

#include "Crypto.h"

class PairStore {
public:
    struct Phone {
        Bytes key;
        std::string name;
    };

    void Load();
    Bytes PcId();
    bool Key(const std::string& phoneId, Bytes& key);
    void Put(const std::string& phoneId, const Bytes& key, const std::string& name);
    void Remove(const std::string& phoneId);
    void Clear();
    std::map<std::string, Phone> All();

    std::string LastPhone();
    void SetLastPhone(const std::string& id);
    bool AutoConnect();
    void SetAutoConnect(bool on);

private:
    void Save();
    std::wstring Path();
    std::mutex m_lock;
    Bytes m_pcId;
    std::map<std::string, Phone> m_phones;
    std::string m_last;
    bool m_auto = true;
};
