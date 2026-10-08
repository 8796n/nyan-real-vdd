// SPDX-License-Identifier: MIT
// Copyright (c) 2026 nyan Real
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include "../../include/nyanvdd_protocol.h"

namespace nyan::vdd {

// CPU-only channel state may be closed from WDF callbacks; GPU objects belong
// exclusively to the swap-chain worker, whose teardown never blocks callbacks.
struct FrameChannel {
    ~FrameChannel();
    static HRESULT Create(std::shared_ptr<FrameChannel>& Out);
    void Stop();
    void Fail(HRESULT Error);
    bool Active() const { return !Stopped.load(); }

    WCHAR Name[NYANVDD_CAPTURE_NAME_CHARS]{};
    NYANVDD_CAPTURE_METADATA* Meta = nullptr;
    PSECURITY_DESCRIPTOR Security = nullptr;
    HANDLE Mapping = nullptr;
    std::atomic<bool> Stopped{false};
};

struct CaptureBinding {
    // Access only through atomic_load/atomic_store for shared_ptr.
    std::shared_ptr<FrameChannel> Channel;
};

class FramePublisher {
public:
    ~FramePublisher();
    HRESULT Init(const std::shared_ptr<FrameChannel>& Channel,
                 ID3D11Device* Device, const D3D11_TEXTURE2D_DESC& Source, LUID Adapter);
    HRESULT Publish(ID3D11DeviceContext* Context, ID3D11Texture2D* Source, LONG64 PresentQpc);

private:
    std::shared_ptr<FrameChannel> Channel;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, NYANVDD_CAPTURE_SLOTS> Textures;
    std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, NYANVDD_CAPTURE_SLOTS> Mutexes;
    std::array<HANDLE, NYANVDD_CAPTURE_SLOTS> Handles{};
    D3D11_TEXTURE2D_DESC Desc{};
    UINT Next = 0;
    LONG64 Sequence = 0;
};
}
