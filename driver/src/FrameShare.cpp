// SPDX-License-Identifier: MIT
// Copyright (c) 2026 nyan Real
#include "FrameShare.h"
#include <sddl.h>
#include <cstdio>
#include <new>

namespace nyan::vdd {
using Microsoft::WRL::ComPtr;

FrameChannel::~FrameChannel()
{
    if (Meta) UnmapViewOfFile(Meta);
    if (Mapping) CloseHandle(Mapping);
    if (Security) LocalFree(Security);
}

HRESULT FrameChannel::Create(std::shared_ptr<FrameChannel>& Out) try
{
    auto Channel = std::make_shared<FrameChannel>();
    GUID Id{};
    HRESULT Hr = CoCreateGuid(&Id);
    if (FAILED(Hr)) return Hr;
    WCHAR Guid[40]{};
    if (!StringFromGUID2(Id, Guid, ARRAYSIZE(Guid))) return E_FAIL;
    swprintf_s(Channel->Name, L"Global\\NyanVddCapture-%s", Guid);
    // ponytail: diagnostic transport is admin-only, including the GPU handles.
    // Add interactive-session/secure-desktop authorization before app adoption.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)", SDDL_REVISION_1,
            &Channel->Security, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    SECURITY_ATTRIBUTES Security{sizeof(Security), Channel->Security, FALSE};
    WCHAR Name[NYANVDD_CAPTURE_NAME_CHARS]{};
    swprintf_s(Name, L"%s-meta", Channel->Name);
    Channel->Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &Security,
        PAGE_READWRITE, 0, sizeof(NYANVDD_CAPTURE_METADATA), Name);
    if (!Channel->Mapping) return HRESULT_FROM_WIN32(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    Channel->Meta = static_cast<NYANVDD_CAPTURE_METADATA*>(MapViewOfFile(
        Channel->Mapping, FILE_MAP_WRITE, 0, 0, sizeof(NYANVDD_CAPTURE_METADATA)));
    if (!Channel->Meta) return HRESULT_FROM_WIN32(GetLastError());
    *Channel->Meta = {};
    Channel->Meta->ProtocolVersion = NYANVDD_PROTOCOL_VERSION;
    Channel->Meta->DriverProcessId = GetCurrentProcessId();
    Out = std::move(Channel);
    return S_OK;
}
catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

void FrameChannel::Stop()
{
    Stopped = true;
    if (Meta) {
        InterlockedCompareExchange(&Meta->State, NYANVDD_CAPTURE_STOPPED, NYANVDD_CAPTURE_WAITING);
        InterlockedCompareExchange(&Meta->State, NYANVDD_CAPTURE_STOPPED, NYANVDD_CAPTURE_READY);
    }
}

void FrameChannel::Fail(HRESULT Error)
{
    Stopped = true;
    Meta->Error = Error;
    InterlockedExchange(&Meta->State, NYANVDD_CAPTURE_FAILED);
}

FramePublisher::~FramePublisher()
{
    if (Channel) Channel->Stop();
    for (HANDLE Handle : Handles) if (Handle) CloseHandle(Handle);
}

HRESULT FramePublisher::Init(const std::shared_ptr<FrameChannel>& NewChannel,
                            ID3D11Device* Device, const D3D11_TEXTURE2D_DESC& Source, LUID Adapter)
{
    Channel = NewChannel;
    if (Source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || Source.ArraySize != 1 ||
        Source.MipLevels != 1 || Source.SampleDesc.Count != 1 ||
        Source.Width == 0 || Source.Height == 0 ||
        Source.Width > NYANVDD_MAX_DIMENSION || Source.Height > NYANVDD_MAX_DIMENSION)
        return E_INVALIDARG;
    Desc = Source;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Desc.CPUAccessFlags = 0;
    Desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    SECURITY_ATTRIBUTES Security{sizeof(Security), Channel->Security, FALSE};
    for (UINT i = 0; i < NYANVDD_CAPTURE_SLOTS; ++i) {
        HRESULT Hr = Device->CreateTexture2D(&Desc, nullptr, &Textures[i]);
        if (FAILED(Hr)) return Hr;
        Hr = Textures[i].As(&Mutexes[i]);
        if (FAILED(Hr)) return Hr;
        ComPtr<IDXGIResource1> Resource;
        Hr = Textures[i].As(&Resource);
        if (FAILED(Hr)) return Hr;
        WCHAR Name[NYANVDD_CAPTURE_NAME_CHARS]{};
        swprintf_s(Name, L"%s-%u", Channel->Name, i);
        Hr = Resource->CreateSharedHandle(&Security,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, Name, &Handles[i]);
        if (FAILED(Hr)) return Hr;
    }
    auto& Meta = *Channel->Meta;
    Meta.Width = Source.Width; Meta.Height = Source.Height;
    Meta.AdapterLuid = Adapter;
    Meta.SourceBindFlags = Source.BindFlags; Meta.SourceMiscFlags = Source.MiscFlags;
    InterlockedCompareExchange(&Meta.State, NYANVDD_CAPTURE_READY, NYANVDD_CAPTURE_WAITING);
    return S_OK;
}

HRESULT FramePublisher::Publish(ID3D11DeviceContext* Context, ID3D11Texture2D* Source, LONG64 PresentQpc)
{
    if (!Channel->Active()) return S_FALSE;
    auto& Meta = *Channel->Meta;
    InterlockedIncrement64(&Meta.FramesSeen);
    D3D11_TEXTURE2D_DESC Actual{}; Source->GetDesc(&Actual);
    if (Actual.Width != Desc.Width || Actual.Height != Desc.Height || Actual.Format != Desc.Format)
        return DXGI_ERROR_ACCESS_LOST;
    for (UINT Attempt = 0; Attempt < NYANVDD_CAPTURE_SLOTS; ++Attempt) {
        const UINT i = (Next + Attempt) % NYANVDD_CAPTURE_SLOTS;
        HRESULT Hr = Mutexes[i]->AcquireSync(0, 0);
        if (Hr == WAIT_TIMEOUT) continue;
        if (Hr != S_OK) return FAILED(Hr) ? Hr : E_FAIL;
        LARGE_INTEGER Begin{}, End{}; QueryPerformanceCounter(&Begin);
        Context->CopyResource(Textures[i].Get(), Source);
        Context->Flush();
        QueryPerformanceCounter(&End);
        Meta.Slots[i].PresentQpc = PresentQpc;
        Meta.Slots[i].PublishQpc = End.QuadPart;
        InterlockedExchange64(&Meta.Slots[i].Sequence, ++Sequence);
        Hr = Mutexes[i]->ReleaseSync(1);
        if (FAILED(Hr)) return Hr;
        InterlockedIncrement64(&Meta.FramesPublished);
        InterlockedAdd64(&Meta.SubmitQpcTicks, End.QuadPart - Begin.QuadPart);
        Next = (i + 1) % NYANVDD_CAPTURE_SLOTS;
        return S_OK;
    }
    InterlockedIncrement64(&Meta.FramesSkipped);
    return S_FALSE;
}
}
