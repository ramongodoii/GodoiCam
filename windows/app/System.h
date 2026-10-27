// Integrações com o sistema: câmera virtual do Windows 11 e adb para o modo USB.
#pragma once

#include <string>

namespace sys {

// A DLL está registrada em HKLM? (precisa de administrador uma vez, via instalar.cmd)
bool VCamDllRegistered();
// Registra a DLL que está ao lado do .exe, pedindo elevação (UAC).
bool RegisterVCamDllElevated(std::wstring& error);
// Cria (ou reabre) a câmera virtual "GodoiCam", persistente entre reinicializações.
bool EnsureVirtualCamera(std::wstring& error);
bool RemoveVirtualCamera(std::wstring& error);

// Localiza o adb.exe (pasta do app, PATH ou Android SDK).
std::wstring FindAdb();
// adb forward tcp:47180 tcp:47180. Devolve falso com a mensagem do adb.
bool AdbForward(std::wstring& error);
// Há um aparelho autorizado no `adb devices`? (rápido; falso se não houver adb)
bool AdbDevicePresent();

// Situação do celular no cabo USB, para orientar o usuário.
enum class UsbPhone {
    None,          // nada no cabo (ou só carregando)
    Ready,         // autorizado no adb
    Unauthorized,  // depuração ligada, falta tocar em "Permitir" no celular
    NoDebugging,   // celular no cabo (transferência de arquivos), mas sem a depuração USB
    NoAdb,         // celular no cabo, mas o adb.exe não foi encontrado no PC
};
// Consulta o adb e, se ele não vê nada, os aparelhos portáteis (MTP) ligados por USB.
// `name` recebe o nome do aparelho no Windows, quando conhecido. Leva alguns ms (roda o adb).
UsbPhone UsbPhoneState(std::wstring& name);

// "Iniciar com o Windows" (HKCU\...\Run), iniciando minimizado na bandeja.
bool StartWithWindows();
void SetStartWithWindows(bool on);

std::wstring ExeDir();
std::string ComputerName();

}  // namespace sys
