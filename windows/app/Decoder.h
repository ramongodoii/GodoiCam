// Decodificador H.264 do Media Foundation em modo de baixa latência, saída NV12.
// Usa a placa de vídeo (D3D11/DXVA) quando disponível e cai para a CPU se não der.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <functional>
#include <stdint.h>
#include <string>

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
    // Decodificando na placa de vídeo (falso = CPU).
    bool Hardware() const { return m_hardware; }
    // Os próximos Init usam só a CPU (a placa de vídeo falhou ao decodificar).
    void ForceSoftware() { m_noHardware = true; }
    // Nome da placa de vídeo usada (vazio na CPU).
    const std::wstring& AdapterName() const { return m_adapterName; }
    const char* LastError() const { return m_error; }
    // Entrega os quadros prontos para `sink`. Falso indica erro (peça um quadro-chave).
    bool Decode(const uint8_t* data, size_t len, int64_t ptsUs, const Sink& sink);

private:
    bool InitWith(int width, int height, int fps, bool hardware);
    bool CreateDevice();
    bool SetOutputNV12();
    bool Drain(const Sink& sink);
    // Quadro em textura D3D11: copia para uma textura de leitura e entrega. Falso se o buffer não é D3D11.
    bool DeliverTexture(IMFMediaBuffer* buffer, int64_t ptsUs, const Sink& sink);

    // Declarados antes do MFT: são liberados depois dele.
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_manager;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    bool m_deviceTried = false;
    bool m_noHardware = false;
    std::wstring m_adapterName;

    Microsoft::WRL::ComPtr<IMFTransform> m_mft;
    Microsoft::WRL::ComPtr<IMFSample> m_out;
    DWORD m_outSize = 0;
    bool m_providesSamples = false;
    bool m_hardware = false;
    int m_width = 0, m_height = 0;    // tamanho exibido (do CONFIG)
    UINT32 m_frameW = 0, m_frameH = 0;  // tamanho alinhado da saída
    LONG m_stride = 0;
    char m_error[96] = "";
};
