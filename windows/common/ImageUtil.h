// Conversões e redimensionamento de imagem em CPU (NV12 e BGRA), usados pela câmera
// virtual e pela prévia do app.
#pragma once

#include <stdint.h>
#include <vector>

namespace img {

inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Tabela de amostragem bilinear para um eixo: índice e peso (0..256) por pixel de destino.
struct Axis {
    std::vector<int> index;
    std::vector<int> weight;
    void Build(int src, int dst) {
        index.resize(dst);
        weight.resize(dst);
        for (int i = 0; i < dst; i++) {
            // Centro do pixel de destino mapeado no espaço de origem, em ponto fixo 8 bits.
            int pos = (int)(((int64_t)(2 * i + 1) * src * 128) / dst) - 128;
            if (pos < 0) pos = 0;
            int idx = pos >> 8;
            if (idx >= src - 1) {
                idx = src - 1 > 0 ? src - 2 : 0;
                pos = (src - 1) << 8;
            }
            index[i] = idx;
            weight[i] = src > 1 ? pos - (idx << 8) : 0;
        }
    }
};

// Plano de 1 byte por amostra (Y), bilinear.
inline void ScalePlane(const uint8_t* src, int sPitch, int sw, int sh, uint8_t* dst, int dPitch, int dw, int dh) {
    if (sw == dw && sh == dh) {
        for (int y = 0; y < dh; y++) memcpy(dst + (size_t)y * dPitch, src + (size_t)y * sPitch, dw);
        return;
    }
    Axis ax, ay;
    ax.Build(sw, dw);
    ay.Build(sh, dh);
    for (int y = 0; y < dh; y++) {
        const uint8_t* r0 = src + (size_t)ay.index[y] * sPitch;
        const uint8_t* r1 = sh > 1 ? r0 + sPitch : r0;
        int wy = ay.weight[y];
        uint8_t* out = dst + (size_t)y * dPitch;
        for (int x = 0; x < dw; x++) {
            int i = ax.index[x];
            int wx = ax.weight[x];
            int i1 = sw > 1 ? i + 1 : i;
            int top = r0[i] * (256 - wx) + r0[i1] * wx;
            int bot = r1[i] * (256 - wx) + r1[i1] * wx;
            out[x] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
        }
    }
}

// Plano UV intercalado (2 bytes por amostra), bilinear.
inline void ScaleUV(const uint8_t* src, int sPitch, int sw, int sh, uint8_t* dst, int dPitch, int dw, int dh) {
    if (sw == dw && sh == dh) {
        for (int y = 0; y < dh; y++) memcpy(dst + (size_t)y * dPitch, src + (size_t)y * sPitch, (size_t)dw * 2);
        return;
    }
    Axis ax, ay;
    ax.Build(sw, dw);
    ay.Build(sh, dh);
    for (int y = 0; y < dh; y++) {
        const uint8_t* r0 = src + (size_t)ay.index[y] * sPitch;
        const uint8_t* r1 = sh > 1 ? r0 + sPitch : r0;
        int wy = ay.weight[y];
        uint8_t* out = dst + (size_t)y * dPitch;
        for (int x = 0; x < dw; x++) {
            int i = ax.index[x] * 2;
            int wx = ax.weight[x];
            int i1 = sw > 1 ? i + 2 : i;
            for (int c = 0; c < 2; c++) {
                int top = r0[i + c] * (256 - wx) + r0[i1 + c] * wx;
                int bot = r1[i + c] * (256 - wx) + r1[i1 + c] * wx;
                out[x * 2 + c] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
}

// NV12 → NV12 em outro tamanho. As alturas e larguras devem ser pares.
inline void ScaleNV12(const uint8_t* sy, int syPitch, const uint8_t* suv, int suvPitch, int sw, int sh,
                      uint8_t* dy, int dyPitch, uint8_t* duv, int duvPitch, int dw, int dh) {
    ScalePlane(sy, syPitch, sw, sh, dy, dyPitch, dw, dh);
    ScaleUV(suv, suvPitch, sw / 2, sh / 2, duv, duvPitch, dw / 2, dh / 2);
}

// NV12 (BT.709, faixa limitada) → BGRA no mesmo tamanho.
inline void NV12ToBGRA(const uint8_t* sy, int syPitch, const uint8_t* suv, int suvPitch, int w, int h,
                       uint8_t* dst, int dPitch) {
    for (int y = 0; y < h; y++) {
        const uint8_t* ry = sy + (size_t)y * syPitch;
        const uint8_t* ruv = suv + (size_t)(y / 2) * suvPitch;
        uint8_t* out = dst + (size_t)y * dPitch;
        for (int x = 0; x < w; x++) {
            int c = (ry[x] - 16) * 298;
            int u = ruv[(x & ~1)] - 128;
            int v = ruv[(x & ~1) + 1] - 128;
            out[x * 4 + 0] = Clamp8((c + 541 * u + 128) >> 8);
            out[x * 4 + 1] = Clamp8((c - 55 * u - 136 * v + 128) >> 8);
            out[x * 4 + 2] = Clamp8((c + 459 * v + 128) >> 8);
            out[x * 4 + 3] = 255;
        }
    }
}

// NV12 → BGRA reduzido por amostragem do vizinho mais próximo (prévia).
inline void NV12ToBGRANearest(const uint8_t* sy, int syPitch, const uint8_t* suv, int suvPitch, int sw, int sh,
                              uint8_t* dst, int dPitch, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int srcY = (int)((int64_t)y * sh / dh);
        const uint8_t* ry = sy + (size_t)srcY * syPitch;
        const uint8_t* ruv = suv + (size_t)(srcY / 2) * suvPitch;
        uint8_t* out = dst + (size_t)y * dPitch;
        for (int x = 0; x < dw; x++) {
            int srcX = (int)((int64_t)x * sw / dw);
            int c = (ry[srcX] - 16) * 298;
            int u = ruv[(srcX & ~1)] - 128;
            int v = ruv[(srcX & ~1) + 1] - 128;
            out[x * 4 + 0] = Clamp8((c + 541 * u + 128) >> 8);
            out[x * 4 + 1] = Clamp8((c - 55 * u - 136 * v + 128) >> 8);
            out[x * 4 + 2] = Clamp8((c + 459 * v + 128) >> 8);
            out[x * 4 + 3] = 255;
        }
    }
}

// BGRA → NV12 (BT.709, faixa limitada), mesmo tamanho. Usado só para a tela de espera.
inline void BGRAToNV12(const uint8_t* src, int sPitch, int w, int h, uint8_t* dy, int dyPitch, uint8_t* duv,
                       int duvPitch) {
    for (int y = 0; y < h; y++) {
        const uint8_t* row = src + (size_t)y * sPitch;
        uint8_t* out = dy + (size_t)y * dyPitch;
        for (int x = 0; x < w; x++) {
            int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            out[x] = Clamp8(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < h / 2; y++) {
        const uint8_t* r0 = src + (size_t)(y * 2) * sPitch;
        const uint8_t* r1 = r0 + sPitch;
        uint8_t* out = duv + (size_t)y * duvPitch;
        for (int x = 0; x < w / 2; x++) {
            int b = 0, g = 0, r = 0;
            for (int k = 0; k < 2; k++) {
                b += r0[(x * 2 + k) * 4] + r1[(x * 2 + k) * 4];
                g += r0[(x * 2 + k) * 4 + 1] + r1[(x * 2 + k) * 4 + 1];
                r += r0[(x * 2 + k) * 4 + 2] + r1[(x * 2 + k) * 4 + 2];
            }
            b /= 4;
            g /= 4;
            r /= 4;
            out[x * 2] = Clamp8(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            out[x * 2 + 1] = Clamp8(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}

}  // namespace img
