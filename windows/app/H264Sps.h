// Lê, do SPS de um fluxo H.264 (Annex B), como as cores foram codificadas (VUI):
// matriz YUV, faixa (limitada 16–235 ou cheia 0–255), primárias e curva de transferência.
// Serve para conferir se o celular manda o que o PC supõe (BT.709, faixa limitada).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct H264Color {
    bool signaled = false;   // o SPS traz video_signal_type
    bool fullRange = false;  // video_full_range_flag
    int primaries = 2;       // 2 = não especificado (H.273)
    int transfer = 2;
    int matrix = 2;
};

namespace h264sps {

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& data) : m_data(data) {}
    bool ok() const { return m_ok; }
    uint32_t Bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if (m_pos >= m_data.size() * 8) {
                m_ok = false;
                return 0;
            }
            v = (v << 1) | ((m_data[m_pos / 8] >> (7 - m_pos % 8)) & 1);
            m_pos++;
        }
        return v;
    }
    uint32_t Ue() {
        int zeros = 0;
        while (m_ok && Bits(1) == 0)
            if (++zeros > 31) m_ok = false;
        return m_ok ? (1u << zeros) - 1 + Bits(zeros) : 0;
    }
    int32_t Se() {
        uint32_t k = Ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }

private:
    const std::vector<uint8_t>& m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

inline void SkipScalingList(BitReader& b, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0) next = (last + b.Se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

// RBSP do SPS (sem o byte de cabeçalho da NAL e sem os bytes de prevenção 0x03).
inline bool ParseSpsRbsp(const std::vector<uint8_t>& rbsp, H264Color& out) {
    BitReader b(rbsp);
    int profile = (int)b.Bits(8);
    b.Bits(16);  // restrições + nível
    b.Ue();      // seq_parameter_set_id
    static const int kHigh[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    bool high = false;
    for (int p : kHigh) high |= p == profile;
    if (high) {
        uint32_t chroma = b.Ue();
        if (chroma == 3) b.Bits(1);
        b.Ue();
        b.Ue();
        b.Bits(1);
        if (b.Bits(1))
            for (int i = 0; i < (chroma == 3 ? 12 : 8); i++)
                if (b.Bits(1)) SkipScalingList(b, i < 6 ? 16 : 64);
    }
    b.Ue();  // log2_max_frame_num_minus4
    uint32_t pocType = b.Ue();
    if (pocType == 0) {
        b.Ue();
    } else if (pocType == 1) {
        b.Bits(1);
        b.Se();
        b.Se();
        uint32_t n = b.Ue();
        for (uint32_t i = 0; i < n && b.ok(); i++) b.Se();
    }
    b.Ue();     // max_num_ref_frames
    b.Bits(1);  // gaps_in_frame_num_allowed
    b.Ue();     // largura em macroblocos
    b.Ue();     // altura
    if (!b.Bits(1)) b.Bits(1);  // frame_mbs_only / mb_adaptive_frame_field
    b.Bits(1);                  // direct_8x8_inference
    if (b.Bits(1))
        for (int i = 0; i < 4; i++) b.Ue();  // recorte
    out = H264Color{};
    if (!b.ok()) return false;
    if (!b.Bits(1)) return b.ok();  // sem VUI
    if (b.Bits(1) && b.Bits(8) == 255) b.Bits(32);  // proporção do pixel
    if (b.Bits(1)) b.Bits(1);                        // overscan
    if (b.Bits(1)) {
        b.Bits(3);  // video_format
        out.signaled = true;
        out.fullRange = b.Bits(1) != 0;
        if (b.Bits(1)) {
            out.primaries = (int)b.Bits(8);
            out.transfer = (int)b.Bits(8);
            out.matrix = (int)b.Bits(8);
        }
    }
    return b.ok();
}

}  // namespace h264sps

// Procura o SPS (NAL tipo 7) no trecho Annex B. Falso se não houver SPS legível.
inline bool ReadH264Color(const uint8_t* data, size_t len, H264Color& out) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
        size_t start = i + 3;
        if ((data[start] & 0x1F) != 7) continue;
        std::vector<uint8_t> rbsp;
        int zeros = 0;
        for (size_t j = start + 1; j < len; j++) {
            if (zeros >= 2 && data[j] == 1) break;  // próximo código de início
            if (zeros >= 2 && data[j] == 3) {
                zeros = 0;
                continue;
            }
            zeros = data[j] == 0 ? zeros + 1 : 0;
            rbsp.push_back(data[j]);
        }
        while (!rbsp.empty() && rbsp.back() == 0) rbsp.pop_back();
        return h264sps::ParseSpsRbsp(rbsp, out);
    }
    return false;
}

// "BT.709, faixa limitada" etc., para o log e o relatório.
inline std::string DescribeH264Color(const H264Color& c) {
    if (!c.signaled) return "não informada no vídeo (o PC supõe BT.709, faixa limitada)";
    auto matrix = [](int m) -> std::string {
        switch (m) {
            case 1: return "BT.709";
            case 5:
            case 6: return "BT.601";
            case 9: return "BT.2020";
            case 2: return "matriz não especificada";
            default: return "matriz " + std::to_string(m);
        }
    };
    std::string s = matrix(c.matrix) + (c.fullRange ? ", faixa cheia (0-255)" : ", faixa limitada (16-235)");
    if (c.primaries != 2 || c.transfer != 2)
        s += ", primárias " + std::to_string(c.primaries) + ", transferência " + std::to_string(c.transfer);
    return s;
}

// O PC (prévia e câmera virtual) interpreta tudo como BT.709 com faixa limitada.
inline bool H264ColorMatchesPc(const H264Color& c) {
    return !c.signaled || (!c.fullRange && (c.matrix == 1 || c.matrix == 2));
}
