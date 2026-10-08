// SPDX-License-Identifier: MIT
// Copyright (c) 2026 nyan Real
// Same consumer and stimulus for WGC vs the experimental VDD shared GPU ring.
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <cfgmgr32.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include "../../driver/src/FrameShare.h"

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
using namespace nyan::vdd;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WD3D = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
void Check(HRESULT Hr) { winrt::check_hresult(Hr); }

struct SharedReader {
    HANDLE Device = INVALID_HANDLE_VALUE;
    HANDLE Mapping = nullptr;
    const NYANVDD_CAPTURE_METADATA* Meta = nullptr;
    WCHAR Name[NYANVDD_CAPTURE_NAME_CHARS]{};
    ComPtr<ID3D11Texture2D> Textures[NYANVDD_CAPTURE_SLOTS];
    ComPtr<ID3D11ShaderResourceView> Views[NYANVDD_CAPTURE_SLOTS];
    ComPtr<IDXGIKeyedMutex> Mutexes[NYANVDD_CAPTURE_SLOTS];
    int Held = -1;
    LONG64 Sequence = 0;

    ~SharedReader() {
        if (Held >= 0) Mutexes[Held]->ReleaseSync(0);
        if (Device != INVALID_HANDLE_VALUE) CloseHandle(Device);
        if (Meta && Mapping) UnmapViewOfFile(Meta);
        if (Mapping) CloseHandle(Mapping);
    }
    void Connect(UINT32 Cookie) {
        GUID Guid = NYANVDD_INTERFACE_GUID_INIT;
        ULONG Count = 0;
        if (CM_Get_Device_Interface_List_SizeW(&Count, &Guid, nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || Count < 2)
            Check(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        std::vector<wchar_t> Paths(Count);
        if (CM_Get_Device_Interface_ListW(&Guid, nullptr, Paths.data(), Count,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) Check(E_FAIL);
        Device = CreateFileW(Paths.data(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (Device == INVALID_HANDLE_VALUE) Check(HRESULT_FROM_WIN32(GetLastError()));
        NYANVDD_STATUS_OUT Status{}; DWORD Bytes = 0;
        if (!DeviceIoControl(Device, IOCTL_NYANVDD_GET_STATUS, nullptr, 0,
                &Status, sizeof(Status), &Bytes, nullptr)) Check(HRESULT_FROM_WIN32(GetLastError()));
        if (Bytes != sizeof(Status) || Status.ProtocolVersion != 4 ||
            !(Status.CapFlags & NYANVDD_CAP_SHARED_CAPTURE)) Check(E_NOINTERFACE);
        NYANVDD_CAPTURE_IN In{Cookie}; NYANVDD_CAPTURE_OUT Out{};
        if (!DeviceIoControl(Device, IOCTL_NYANVDD_OPEN_CAPTURE, &In, sizeof(In),
                &Out, sizeof(Out), &Bytes, nullptr)) Check(HRESULT_FROM_WIN32(GetLastError()));
        if (Bytes != sizeof(Out) || Out.Name[NYANVDD_CAPTURE_NAME_CHARS-1]) Check(E_INVALIDARG);
        wcscpy_s(Name, Out.Name);
        WCHAR MapName[NYANVDD_CAPTURE_NAME_CHARS]{}; swprintf_s(MapName, L"%s-meta", Name);
        Mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, MapName);
        if (!Mapping) Check(HRESULT_FROM_WIN32(GetLastError()));
        Meta = static_cast<const NYANVDD_CAPTURE_METADATA*>(MapViewOfFile(
            Mapping, FILE_MAP_READ, 0, 0, sizeof(NYANVDD_CAPTURE_METADATA)));
        if (!Meta) Check(HRESULT_FROM_WIN32(GetLastError()));
        if (Meta->ProtocolVersion != 4) Check(E_NOINTERFACE);
    }
    bool Import(ID3D11Device* Gpu) {
        const LONG State = Meta->State; MemoryBarrier();
        if (State == NYANVDD_CAPTURE_WAITING) return false;
        if (State != NYANVDD_CAPTURE_READY) Check(FAILED(Meta->Error) ? Meta->Error : DXGI_ERROR_ACCESS_LOST);
        if (Textures[0]) return true;
        ComPtr<ID3D11Device1> Device1; Check(Gpu->QueryInterface(IID_PPV_ARGS(&Device1)));
        ComPtr<IDXGIDevice> Dxgi; Check(Gpu->QueryInterface(IID_PPV_ARGS(&Dxgi)));
        ComPtr<IDXGIAdapter> Adapter; Check(Dxgi->GetAdapter(&Adapter));
        DXGI_ADAPTER_DESC AdapterDesc{}; Check(Adapter->GetDesc(&AdapterDesc));
        if (memcmp(&AdapterDesc.AdapterLuid, &Meta->AdapterLuid, sizeof(LUID))) Check(DXGI_ERROR_UNSUPPORTED);
        for (UINT i = 0; i < NYANVDD_CAPTURE_SLOTS; ++i) {
            WCHAR TextureName[NYANVDD_CAPTURE_NAME_CHARS]{}; swprintf_s(TextureName, L"%s-%u", Name, i);
            Check(Device1->OpenSharedResourceByName(TextureName,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&Textures[i])));
            D3D11_TEXTURE2D_DESC Desc{}; Textures[i]->GetDesc(&Desc);
            if (Desc.Width != Meta->Width || Desc.Height != Meta->Height || Desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
                Check(E_INVALIDARG);
            Check(Textures[i].As(&Mutexes[i]));
            Check(Gpu->CreateShaderResourceView(Textures[i].Get(), nullptr, &Views[i]));
        }
        return true;
    }
    bool Poll() {
        bool Changed = false;
        for (UINT i = 0; i < NYANVDD_CAPTURE_SLOTS; ++i) {
            if (int(i) == Held) continue;
            const HRESULT Hr = Mutexes[i]->AcquireSync(1, 0);
            if (Hr == WAIT_TIMEOUT) continue;
            if (Hr != S_OK) Check(FAILED(Hr) ? Hr : E_FAIL);
            const LONG64 Next = Meta->Slots[i].Sequence;
            if (Next > Sequence) {
                if (Held >= 0) Check(Mutexes[Held]->ReleaseSync(0));
                Held = int(i); Sequence = Next; Changed = true;
            } else Check(Mutexes[i]->ReleaseSync(0));
        }
        return Changed;
    }
};

ComPtr<ID3D11Texture2D> Staging(ID3D11Device* Device, UINT Width, UINT Height) {
    D3D11_TEXTURE2D_DESC Desc{};
    Desc.Width = Width; Desc.Height = Height;
    Desc.MipLevels = Desc.ArraySize = Desc.SampleDesc.Count = 1;
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.Usage = D3D11_USAGE_STAGING; Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> Texture; Check(Device->CreateTexture2D(&Desc, nullptr, &Texture));
    return Texture;
}

double CpuMs(HANDLE Process) {
    FILETIME Created{}, Exit{}, Kernel{}, User{};
    if (!GetProcessTimes(Process, &Created, &Exit, &Kernel, &User)) return -1;
    const auto Value = [](FILETIME T) { return (ULONGLONG(T.dwHighDateTime)<<32) | T.dwLowDateTime; };
    return double(Value(Kernel) + Value(User))/10000.0;
}

HANDLE OpenDwm() {
    DWORD Session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &Session);
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE Result = nullptr;
    PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
    for (BOOL More = Process32FirstW(Snapshot, &Entry); More; More = Process32NextW(Snapshot, &Entry)) {
        DWORD Candidate = MAXDWORD;
        if (_wcsicmp(Entry.szExeFile, L"dwm.exe") == 0 &&
            ProcessIdToSessionId(Entry.th32ProcessID, &Candidate) && Candidate == Session) {
            Result = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
            break;
        }
    }
    CloseHandle(Snapshot); return Result;
}

void PrintTimes(const char* Name, std::vector<double> Values) {
    if (Values.empty()) { printf("%s: n=0\n", Name); return; }
    std::sort(Values.begin(), Values.end());
    printf("%s: n=%zu median=%.3f p95=%.3f max=%.3f\n", Name, Values.size(),
        Values[Values.size()/2], Values[Values.size()*95/100], Values.back());
}
}

int CaptureShareSelfTest() try {
    ComPtr<ID3D11Device> Producer, Consumer;
    ComPtr<ID3D11DeviceContext> Write, Read;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &Producer, nullptr, &Write));
    ComPtr<IDXGIDevice> Dxgi; Check(Producer.As(&Dxgi));
    ComPtr<IDXGIAdapter> Adapter; Check(Dxgi->GetAdapter(&Adapter));
    DXGI_ADAPTER_DESC AdapterDesc{}; Check(Adapter->GetDesc(&AdapterDesc));
    Check(D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &Consumer, nullptr, &Read));
    // Anonymous metadata and Local names exercise the exact GPU publisher
    // without changing the installed driver or requiring administrative rights.
    auto Channel = std::make_shared<FrameChannel>();
    GUID Id{}; Check(CoCreateGuid(&Id)); WCHAR Guid[40]{}; StringFromGUID2(Id, Guid, 40);
    swprintf_s(Channel->Name, L"Local\\NyanVddCapture-%s", Guid);
    Channel->Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(NYANVDD_CAPTURE_METADATA), nullptr);
    if (!Channel->Mapping) Check(HRESULT_FROM_WIN32(GetLastError()));
    Channel->Meta = static_cast<NYANVDD_CAPTURE_METADATA*>(MapViewOfFile(
        Channel->Mapping, FILE_MAP_WRITE, 0, 0, sizeof(NYANVDD_CAPTURE_METADATA)));
    if (!Channel->Meta) Check(HRESULT_FROM_WIN32(GetLastError()));
    D3D11_TEXTURE2D_DESC Desc{};
    Desc.Width = 32; Desc.Height = 24; Desc.MipLevels = Desc.ArraySize = Desc.SampleDesc.Count = 1;
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D11Texture2D> Source; Check(Producer->CreateTexture2D(&Desc, nullptr, &Source));
    FramePublisher Publisher; Check(Publisher.Init(Channel, Producer.Get(), Desc, AdapterDesc.AdapterLuid));
    SharedReader Reader; Reader.Meta = Channel->Meta; wcscpy_s(Reader.Name, Channel->Name);
    if (!Reader.Import(Consumer.Get())) return 1;
    auto Stage = Staging(Consumer.Get(), 32, 24);
    auto Pixels = [&] {
        Read->CopyResource(Stage.Get(), Reader.Textures[Reader.Held].Get());
        D3D11_MAPPED_SUBRESOURCE Map{}; Check(Read->Map(Stage.Get(), 0, D3D11_MAP_READ, 0, &Map));
        std::vector<UINT32> Result(32*24);
        for (UINT y = 0; y < 24; ++y) memcpy(Result.data()+y*32,
            static_cast<const BYTE*>(Map.pData)+y*Map.RowPitch, 32*4);
        Read->Unmap(Stage.Get(), 0); return Result;
    };
    auto Publish = [&](UINT Frame) {
        std::vector<UINT32> Pixels(32*24);
        for (UINT i = 0; i < Pixels.size(); ++i) Pixels[i] = 0xff000000 | Frame*10000+i;
        Write->UpdateSubresource(Source.Get(), 0, nullptr, Pixels.data(), 32*4, 0);
        return Publisher.Publish(Write.Get(), Source.Get(), Frame);
    };
    Check(Publish(1)); Check(Publish(2)); Check(Publish(3));
    if (Publish(4) != S_FALSE) return 1; // full ring must not wait or overwrite
    for (int Retry = 0; !Reader.Poll() && Retry < 1000; ++Retry) Sleep(1);
    if (Reader.Held < 0) return 1;
    const auto Held = Pixels();
    int Skipped = 0;
    for (UINT Frame = 5; Frame < 17; ++Frame) {
        const HRESULT Hr = Publish(Frame);
        if (Hr == S_FALSE) ++Skipped; else Check(Hr);
        if (Pixels() != Held) return 1;
    }
    if (!Skipped) return 1;
    for (int Frame = 17; Frame < 47; ++Frame) {
        Reader.Poll();
        bool Published = false;
        for (int Retry = 0; Retry < 1000 && !Published; ++Retry) {
            const HRESULT Hr = Publish(Frame); Check(Hr); Published = Hr == S_OK;
            if (!Published) { Reader.Poll(); Sleep(1); }
        }
        if (!Published) return 1;
        for (int Retry = 0; Retry < 1000; ++Retry) {
            Reader.Poll();
            const auto PixelsRead = Pixels();
            if (PixelsRead[0] == (0xff000000u | UINT(Frame)*10000)) {
                for (UINT i = 0; i < PixelsRead.size(); ++i)
                    if (PixelsRead[i] != (0xff000000u | UINT(Frame)*10000+i)) return 1;
                break;
            }
            if (Retry == 999) return 1;
            Sleep(1);
        }
    }
    Channel->Stop();
    if (Publish(47) != S_FALSE) return 1;
    printf("PASS GPU sharing: 2 D3D devices, retained pixels immutable, full-ring skip, 30 exact updates, stop\n");
    return 0;
} catch (const winrt::hresult_error& Error) {
    printf("FAIL GPU sharing: 0x%08X\n", unsigned(Error.code())); return 1;
}

int RunCaptureBench(HMONITOR Monitor, const RECT& Rect, UINT32 Cookie, bool Shared,
                    int Seconds, int Size, int Hz, DWORD DriverPid) try {
    struct TimerResolution {
        bool Active = timeBeginPeriod(1) == TIMERR_NOERROR;
        ~TimerResolution() { if (Active) timeEndPeriod(1); }
    } Timer;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    ComPtr<ID3D11Device> Device; ComPtr<ID3D11DeviceContext> Context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &Context));
    SharedReader Reader;
    WGC::Direct3D11CaptureFramePool Pool{nullptr}; WGC::GraphicsCaptureSession Session{nullptr};
    WGC::Direct3D11CaptureFrame Held{nullptr};
    if (Shared) Reader.Connect(Cookie);
    else {
        ComPtr<IDXGIDevice> Dxgi; Check(Device.As(&Dxgi));
        winrt::com_ptr<IInspectable> Wrapper;
        Check(CreateDirect3D11DeviceFromDXGIDevice(Dxgi.Get(), Wrapper.put()));
        WGC::GraphicsCaptureItem Item{nullptr};
        auto Interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        Check(Interop->CreateForMonitor(Monitor, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(Item)));
        Pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(Wrapper.as<WD3D::IDirect3DDevice>(),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, Item.Size());
        Session = Pool.CreateCaptureSession(Item);
        Session.IsBorderRequired(false); Session.IsCursorCaptureEnabled(false);
        Session.DirtyRegionMode(WGC::GraphicsCaptureDirtyRegionMode::ReportOnly);
        Session.StartCapture();
    }
    const int Width = int(Rect.right-Rect.left), Height = int(Rect.bottom-Rect.top);
    const bool Full = Size >= Width;
    const int X = Full ? 0 : 40, Y = Full ? 0 : 40;
    struct Window { HWND Value; ~Window() { if (Value) DestroyWindow(Value); } } Window{
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"STATIC", L"VDD capture benchmark",
            WS_POPUP | WS_VISIBLE, Rect.left+X, Rect.top+Y, Full ? Width : std::max(16, Size), Full ? Height : std::max(16, Size),
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    if (!Window.Value) Check(HRESULT_FROM_WIN32(GetLastError()));
    auto Stage = Staging(Device.Get(), 1, 1);
    LARGE_INTEGER Frequency{}, Start{}; QueryPerformanceFrequency(&Frequency); QueryPerformanceCounter(&Start);
    std::vector<LONGLONG> Painted(1);
    std::vector<double> PollUs[2], LatencyMs;
    ComPtr<ID3D11ShaderResourceView> View;
    HANDLE Driver = nullptr;
    struct ProcessHandle { HANDLE& Value; ~ProcessHandle() { if (Value) CloseHandle(Value); } } DriverGuard{Driver};
    HANDLE Dwm = OpenDwm(); ProcessHandle DwmGuard{Dwm};
    LONGLONG NextPaint = Start.QuadPart, MeasurementStart = 0;
    double CpuBegin = 0, DriverCpuBegin = 0, DwmCpuBegin = 0;
    UINT LastSeen = 0; size_t Frames = 0, InvalidPixels = 0;
    while (true) {
        LARGE_INTEGER Now{}; QueryPerformanceCounter(&Now);
        const double Elapsed = double(Now.QuadPart-Start.QuadPart)/Frequency.QuadPart;
        if (Elapsed >= Seconds+1.0) break;
        MSG Message{}; while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&Message);
        if (Now.QuadPart >= NextPaint && (Size > 0 || Painted.size() == 1)) {
            const UINT Id = UINT(Painted.size()); Painted.push_back(Now.QuadPart);
            HDC Dc = GetDC(Window.Value); HBRUSH Brush = CreateSolidBrush(RGB(Id&255, (Id>>8)&255, 0x5a));
            RECT Client{}; GetClientRect(Window.Value, &Client); FillRect(Dc, &Client, Brush);
            DeleteObject(Brush); ReleaseDC(Window.Value, Dc);
            NextPaint = Now.QuadPart + Frequency.QuadPart/std::clamp(Hz, 1, 120);
        }
        if (Shared && !Reader.Import(Device.Get())) { Sleep(1); continue; }
        if (!MeasurementStart && Elapsed >= 1.0) {
            if (Shared) DriverPid = Reader.Meta->DriverProcessId;
            if (DriverPid) Driver = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DriverPid);
            MeasurementStart = Now.QuadPart;
            CpuBegin = CpuMs(GetCurrentProcess()); DriverCpuBegin = Driver ? CpuMs(Driver) : 0;
            DwmCpuBegin = Dwm ? CpuMs(Dwm) : 0;
        }
        LARGE_INTEGER Begin{}, End{}; QueryPerformanceCounter(&Begin);
        ComPtr<ID3D11Texture2D> Texture;
        bool New = false;
        if (Shared) {
            New = Reader.Poll();
            if (New) Texture = Reader.Textures[Reader.Held];
        } else if (auto Frame = Pool.TryGetNextFrame()) {
            if (Held) Held.Close(); Held = Frame;
            Check(Frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
                ->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(Texture.GetAddressOf())));
            View.Reset(); Check(Device->CreateShaderResourceView(Texture.Get(), nullptr, &View));
            New = true;
        }
        QueryPerformanceCounter(&End);
        if (MeasurementStart) PollUs[New].push_back(double(End.QuadPart-Begin.QuadPart)*1e6/Frequency.QuadPart);
        if (New) {
            const D3D11_BOX Box{UINT(X+8), UINT(Y+8), 0, UINT(X+9), UINT(Y+9), 1};
            Context->CopySubresourceRegion(Stage.Get(), 0, 0, 0, 0, Texture.Get(), 0, &Box);
            D3D11_MAPPED_SUBRESOURCE Map{}; Check(Context->Map(Stage.Get(), 0, D3D11_MAP_READ, 0, &Map));
            const UINT Pixel = *static_cast<UINT*>(Map.pData); Context->Unmap(Stage.Get(), 0);
            QueryPerformanceCounter(&End);
            const UINT Id = ((Pixel>>16)&255) | (Pixel&0xff00);
            if (MeasurementStart) {
                ++Frames;
                if ((Pixel&255) != 0x5a || Id == 0 || Id >= Painted.size()) ++InvalidPixels;
                else if (Id > LastSeen) LatencyMs.push_back(double(End.QuadPart-Painted[Id])*1000/Frequency.QuadPart);
            }
            LastSeen = Id;
        }
        Sleep(1);
    }
    printf("capture-bench transport=%s size=%dx%d stimulus=%d@%d seconds=%d frames=%zu marker_errors=%zu driver_pid=%lu\n",
        Shared ? "shared" : "wgc", Width, Height, Size, Hz, Seconds, Frames, InvalidPixels, DriverPid);
    printf("CPU ms: consumer=%.3f driver=%.3f dwm=%.3f (-1 if unavailable)\n",
        CpuMs(GetCurrentProcess())-CpuBegin, Driver ? CpuMs(Driver)-DriverCpuBegin : -1.0,
        Dwm ? CpuMs(Dwm)-DwmCpuBegin : -1.0);
    PrintTimes("poll-new-us", PollUs[1]); PrintTimes("poll-idle-us", PollUs[0]);
    PrintTimes("paint-to-readback-ms", LatencyMs);
    if (Shared) printf("producer: seen=%lld published=%lld skipped=%lld submit_cpu_ms=%.3f source_bind=0x%X source_misc=0x%X\n",
        Reader.Meta->FramesSeen, Reader.Meta->FramesPublished, Reader.Meta->FramesSkipped,
        double(Reader.Meta->SubmitQpcTicks)*1000/Frequency.QuadPart,
        Reader.Meta->SourceBindFlags, Reader.Meta->SourceMiscFlags);
    if (Held) Held.Close();
    if (Session) Session.Close();
    if (Pool) Pool.Close();
    return MeasurementStart && !InvalidPixels && (Size == 0 || !LatencyMs.empty()) ? 0 : 1;
} catch (const winrt::hresult_error& Error) {
    printf("FAIL capture-bench: 0x%08X\n", unsigned(Error.code())); return 1;
}
