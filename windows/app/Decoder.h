// Decodificador H.264 do Media Foundation em modo de baixa latência, saída NV12.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <functional>
#include <stdint.h>

struct DecodedFrame {
    const uint8_t* y;
    int yPitch;
    const uint8_t* uv;
    int uvPitch;
    int width;
    int height;
    int64_t ptsUs;
};

class H264Decoder {
public:
    using Sink = std::function<void(const DecodedFrame&)>;

    bool Init(int width, int height, int fps);
    void Close();
    bool Ready() const { return m_mft != nullptr; }
    const char* LastError() const { return m_error; }
    // Entrega os quadros prontos para `sink`. Falso indica erro (peça um quadro-chave).
    bool Decode(const uint8_t* data, size_t len, int64_t ptsUs, const Sink& sink);

private:
    bool SetOutputNV12();
    bool Drain(const Sink& sink);

    Microsoft::WRL::ComPtr<IMFTransform> m_mft;
    Microsoft::WRL::ComPtr<IMFSample> m_out;
    DWORD m_outSize = 0;
    bool m_providesSamples = false;
    int m_width = 0, m_height = 0;    // tamanho exibido (do CONFIG)
    UINT32 m_frameW = 0, m_frameH = 0;  // tamanho alinhado da saída
    LONG m_stride = 0;
    char m_error[96] = "";
};
