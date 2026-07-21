// nyan Real VDD — IddCx indirect display driver for Spatial Wall.
//
// Portions derived from the Microsoft Windows-driver-samples IndirectDisplay
// sample (MIT License, Copyright (c) Microsoft Corporation).
//
// Architecture summary:
//  - One root-enumerated adapter (created via SwDeviceCreate by the client),
//    zero monitors at start.
//  - Monitors are plugged/unplugged at runtime through IOCTLs on the device
//    interface (see include/nyanvdd_protocol.h). Each monitor carries a
//    client cookie in its EDID serial for unambiguous correlation.
//  - No mandatory keepalive: monitors persist until unplugged. An optional
//    watchdog (opt-in, >= 10 s) unplugs everything if the controlling client
//    stops responding AND asked for that behavior.
//  - Built against IddCx 1.10 headers with IDDCX_MINIMUM_VERSION_REQUIRED=5:
//    one binary runs on Windows 10 2004+ and lights up 1.8/1.9/1.10 features
//    (precise present regions, realtime GPU priority, HDR plumbing) by
//    checking the OS IddCx version at runtime.

#include "Driver.h"

#include <cstdarg>
#include <cstdio>

using namespace std;
using namespace nyan::vdd;
using namespace Microsoft::WRL;

#pragma region Logging

void NyanVddLog(const wchar_t* Format, ...)
{
    wchar_t Buf[512] = L"[nyanvdd] ";
    const size_t Prefix = wcslen(Buf);
    va_list Args;
    va_start(Args, Format);
    _vsnwprintf_s(Buf + Prefix, ARRAYSIZE(Buf) - Prefix, _TRUNCATE, Format, Args);
    va_end(Args);
    wcscat_s(Buf, L"\n");
    OutputDebugStringW(Buf);
}

#pragma endregion

#pragma region ModeTable

// OS IddCx version thresholds (IddCxGetVersion values).
#define NYANVDD_OS_1_8  0x1800u
#define NYANVDD_OS_1_9  0x1900u
#define NYANVDD_OS_1_10 0x1A00u

namespace
{
    struct ModeEntry { UINT32 W, H, Hz; };

    // Static mode table. Geared to XR glasses walls: 1080p-4K at 60/72/90/120.
    // The per-monitor preferred mode from PLUG is prepended (deduplicated).
    const ModeEntry kModeTable[] = {
        { 1920, 1080,  60 }, { 1920, 1080,  72 }, { 1920, 1080,  90 }, { 1920, 1080, 120 },
        { 2560, 1440,  60 }, { 2560, 1440,  90 }, { 2560, 1440, 120 },
        { 3840, 2160,  60 }, { 3840, 2160,  90 }, { 3840, 2160, 120 },
        { 1280,  720,  60 },
    };
    constexpr UINT32 kMaxModes = ARRAYSIZE(kModeTable) + 1;

    // Returns the mode list for a monitor: preferred first (index 0), then
    // the static table minus a duplicate of the preferred mode.
    UINT32 BuildModeList(const NYANVDD_PLUG_IN* Preferred, ModeEntry Out[kMaxModes])
    {
        UINT32 Count = 0;
        if (Preferred)
        {
            Out[Count++] = { Preferred->Width, Preferred->Height, Preferred->RefreshHz };
        }
        for (const auto& M : kModeTable)
        {
            if (Preferred && M.W == Preferred->Width && M.H == Preferred->Height && M.Hz == Preferred->RefreshHz)
            {
                continue;
            }
            Out[Count++] = M;
        }
        return Count;
    }

    void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
    {
        Mode.totalSize.cx = Mode.activeSize.cx = Width;
        Mode.totalSize.cy = Mode.activeSize.cy = Height;

        Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
        Mode.AdditionalSignalInfo.videoStandard = 255;

        Mode.vSyncFreq.Numerator = VSync;
        Mode.vSyncFreq.Denominator = 1;
        Mode.hSyncFreq.Numerator = VSync * Height;
        Mode.hSyncFreq.Denominator = 1;

        Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        Mode.pixelRate = ((UINT64)VSync) * ((UINT64)Width) * ((UINT64)Height);
    }

    IDDCX_MONITOR_MODE CreateIddCxMonitorMode(const ModeEntry& M, IDDCX_MONITOR_MODE_ORIGIN Origin)
    {
        IDDCX_MONITOR_MODE Mode = {};
        Mode.Size = sizeof(Mode);
        Mode.Origin = Origin;
        FillSignalInfo(Mode.MonitorVideoSignalInfo, M.W, M.H, M.Hz, true);
        return Mode;
    }

    IDDCX_TARGET_MODE CreateIddCxTargetMode(const ModeEntry& M)
    {
        IDDCX_TARGET_MODE Mode = {};
        Mode.Size = sizeof(Mode);
        FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, M.W, M.H, M.Hz, false);
        return Mode;
    }

#if IDDCX_VERSION_MINOR >= 10
    // HDR-capable (*2) variants, used by the OS when it runs IddCx 1.10+.
    // SDR monitors report 8 bpc; monitors plugged with NYANVDD_PLUG_FLAG_HDR10
    // additionally report 10 bpc. Full HDR10 (metadata, FP16 swap-chain
    // handling) is future work — see docs/design.ja.md.
    void SetBitsPerComponent(IDDCX_WIRE_BITS_PER_COMPONENT& B, bool Hdr10)
    {
        B = {};
        B.Rgb = Hdr10 ? (IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10)
                      : IDDCX_BITS_PER_COMPONENT_8;
    }

    IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(const ModeEntry& M, IDDCX_MONITOR_MODE_ORIGIN Origin, bool Hdr10)
    {
        IDDCX_MONITOR_MODE2 Mode = {};
        Mode.Size = sizeof(Mode);
        Mode.Origin = Origin;
        FillSignalInfo(Mode.MonitorVideoSignalInfo, M.W, M.H, M.Hz, true);
        SetBitsPerComponent(Mode.BitsPerComponent, Hdr10);
        return Mode;
    }

    IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(const ModeEntry& M, bool Hdr10)
    {
        IDDCX_TARGET_MODE2 Mode = {};
        Mode.Size = sizeof(Mode);
        FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, M.W, M.H, M.Hz, false);
        SetBitsPerComponent(Mode.BitsPerComponent, Hdr10);
        return Mode;
    }
#endif
}

#pragma endregion

#pragma region WdfScaffolding

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD NyanVddDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY NyanVddDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED NyanVddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES NyanVddAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION NyanVddParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES NyanVddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES NyanVddMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN NyanVddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN NyanVddMonitorUnassignSwapChain;
EVT_IDD_CX_DEVICE_IO_CONTROL NyanVddIoDeviceControl;

#if IDDCX_VERSION_MINOR >= 10
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 NyanVddAdapterCommitModes2;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 NyanVddParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 NyanVddMonitorQueryModes2;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA NyanVddMonitorSetDefaultHdrMetaData;
#endif

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

// This driver manages exactly one adapter device. The swap-chain thread uses
// this to reach the adapter for the realtime-GPU-priority call without
// threading the pointer through every layer.
static IndirectDeviceContext* g_DeviceContext = nullptr;

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config, NyanVddDeviceAdd);

    return WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS NyanVddDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = NyanVddDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    IddConfig.EvtIddCxAdapterInitFinished = NyanVddAdapterInitFinished;
    IddConfig.EvtIddCxParseMonitorDescription = NyanVddParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = NyanVddMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = NyanVddMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = NyanVddAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = NyanVddMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = NyanVddMonitorUnassignSwapChain;

    // IddCx redirects DeviceIoControl to an internal queue; custom IOCTLs must
    // be handled through this callback (a WDF default queue would never see
    // them).
    IddConfig.EvtIddCxDeviceIoControl = NyanVddIoDeviceControl;

#if IDDCX_VERSION_MINOR >= 10
    // HDR-capable (*2) paths. On OS versions below IddCx 1.10 these fields
    // are ignored and the *1 callbacks above are used.
    IddConfig.EvtIddCxAdapterCommitModes2 = NyanVddAdapterCommitModes2;
    IddConfig.EvtIddCxParseMonitorDescription2 = NyanVddParseMonitorDescription2;
    IddConfig.EvtIddCxMonitorQueryTargetModes2 = NyanVddMonitorQueryModes2;
    IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = NyanVddMonitorSetDefaultHdrMetaData;
#endif

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Expose the control device interface for clients (Spatial Wall, ctl).
    static const GUID InterfaceGuid = NYANVDD_INTERFACE_GUID_INIT;
    Status = WdfDeviceCreateDeviceInterface(Device, &InterfaceGuid, nullptr);
    if (!NT_SUCCESS(Status))
    {
        NYVDD_LOG(L"WdfDeviceCreateDeviceInterface failed: 0x%08X", Status);
        return Status;
    }

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS NyanVddDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma endregion

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

HRESULT Direct3DDevice::Init()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_hThread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
}

SwapChainProcessor::~SwapChainProcessor()
{
    SetEvent(m_hTerminateEvent);

    if (m_hThread)
    {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
    }
    CloseHandle(m_hTerminateEvent);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // MMCSS keeps this thread scheduled sensibly under load.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Deleting the swap-chain object kicks the system into providing a new
    // one if the monitor is still active.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

#if IDDCX_VERSION_MINOR >= 9
    // IddCx 1.9+ (Win11 22H2): raise this device's GPU work to realtime
    // priority so frame processing is not queued behind application work.
    // This is a latency-sensitive XR path; best effort on older OS/WDDM.
    if (g_DeviceContext && g_DeviceContext->OsVersion() >= NYANVDD_OS_1_9)
    {
        IDARG_IN_SETREALTIMEGPUPRIORITY Priority = {};
        Priority.pDevice = DxgiDevice.Get();
        HRESULT PriorityHr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &Priority);
        g_DeviceContext->NoteRealtimeGpuPriority(SUCCEEDED(PriorityHr));
        if (FAILED(PriorityHr))
        {
            NYVDD_LOG(L"IddCxSetRealtimeGPUPriority failed: 0x%08X", PriorityHr);
        }
    }
#endif

    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        if (hr == E_PENDING)
        {
            HANDLE WaitHandles[] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }
            else
            {
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // The desktop image lives in DWM; the app captures it through
            // Windows.Graphics.Capture. Nothing to transport here — release
            // the surface immediately and tell the OS we are done.
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);
            AcquiredBuffer.Reset();

            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
        }
        else
        {
            // Swap-chain likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST).
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext* IndirectDeviceContext::Get(WDFDEVICE Device)
{
    return WdfObjectGet_IndirectDeviceContextWrapper(Device)->pContext;
}

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice)
{
    g_DeviceContext = this;
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    g_DeviceContext = nullptr;

    if (m_WatchdogThread)
    {
        SetEvent(m_WatchdogStop);
        WaitForSingleObject(m_WatchdogThread, INFINITE);
        CloseHandle(m_WatchdogThread);
        CloseHandle(m_WatchdogWake);
        CloseHandle(m_WatchdogStop);
    }
}

void IndirectDeviceContext::InitAdapter()
{
    // Runtime IddCx version — the key to running one binary from Windows 10
    // 2004 (IddCx 1.5) up while using newer DDIs where present.
    IDARG_OUT_GETVERSION Version = {};
    if (NT_SUCCESS(IddCxGetVersion(&Version)))
    {
        m_OsVersion = (UINT32)Version.IddCxVersion;
    }
    NYVDD_LOG(L"OS IddCx version: 0x%04X", m_OsVersion);

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    AdapterCaps.MaxMonitorsSupported = NYANVDD_MAX_MONITORS;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"nyan Real Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"nyan Real";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Spatial Wall VDD";

    IDDCX_ENDPOINT_VERSION Version2 = {};
    Version2.Size = sizeof(Version2);
    Version2.MajorVer = NYANVDD_DRIVER_VERSION_MAJOR;
    Version2.MinorVer = NYANVDD_DRIVER_VERSION_MINOR;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version2;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version2;

#if IDDCX_VERSION_MINOR >= 8
    if (m_OsVersion >= NYANVDD_OS_1_8)
    {
        // Ask DWM for precise dirty regions: the wall capture pipeline feeds
        // on accurate damage, and imprecise regions inflate capture rate.
        AdapterCaps.Flags |= IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS;
        m_CapFlags |= NYANVDD_CAP_PRECISE_DIRTY;
    }
#endif
#if IDDCX_VERSION_MINOR >= 10
    if (m_OsVersion >= NYANVDD_OS_1_10)
    {
        // FP16 swap-chain surfaces are fine here (frames are discarded), and
        // declaring it is what makes the OS light up the HDR path at all.
        AdapterCaps.Flags |= IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
        m_CapFlags |= NYANVDD_CAP_HDR10_READY;
    }
#endif

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        m_Adapter = AdapterInitOut.AdapterObject;

        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
    else
    {
        NYVDD_LOG(L"IddCxAdapterInitAsync failed: 0x%08X", Status);
    }
}

void IndirectDeviceContext::OnAdapterInitFinished(NTSTATUS Status)
{
    lock_guard<mutex> Guard(m_Lock);
    m_AdapterReady = NT_SUCCESS(Status);
    NYVDD_LOG(L"Adapter init finished: 0x%08X (ready=%d)", Status, m_AdapterReady ? 1 : 0);
}

NTSTATUS IndirectDeviceContext::CreateAndArriveMonitor(UINT ConnectorIndex)
{
    BYTE Edid[128];
    {
        lock_guard<mutex> Guard(m_Lock);
        memcpy(Edid, m_Slots[ConnectorIndex].Edid, sizeof(Edid));
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = ConnectorIndex;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = sizeof(Edid);
    MonitorInfo.MonitorDescription.pData = Edid;

    // Stable container id per cookie: Windows keys remembered display
    // settings off monitor identity, so a re-plug of the same cookie is the
    // same monitor as far as the OS is concerned.
    UINT32 Cookie = CookieFromEdid(Edid, sizeof(Edid));
    GUID ContainerId = { 0x408B3FE4, 0x8AC2, 0x4E97, { 0x83, 0xD8, 0xBE, 0x29, 0x00, 0x00, 0x00, 0x00 } };
    memcpy(&ContainerId.Data4[4], &Cookie, sizeof(Cookie));
    MonitorInfo.MonitorContainerId = ContainerId;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        NYVDD_LOG(L"IddCxMonitorCreate failed: 0x%08X", Status);
        return Status;
    }

    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
    pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);

    IDARG_OUT_MONITORARRIVAL ArrivalOut;
    Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    if (!NT_SUCCESS(Status))
    {
        NYVDD_LOG(L"IddCxMonitorArrival failed: 0x%08X", Status);
        WdfObjectDelete(MonitorCreateOut.MonitorObject);
        return Status;
    }

    bool Stale = false;
    {
        lock_guard<mutex> Guard(m_Lock);
        MonitorSlot& Slot = m_Slots[ConnectorIndex];
        if (Slot.Used && Slot.Params.Cookie == Cookie && !Slot.Arrived)
        {
            Slot.Monitor = MonitorCreateOut.MonitorObject;
            Slot.Arrived = true;
        }
        else
        {
            // The slot was unplugged (and possibly reused) while the arrival
            // was in flight — undo the arrival we just made.
            Stale = true;
        }
    }
    if (Stale)
    {
        NYVDD_LOG(L"Plug of cookie 0x%08X raced an unplug — rolling back", Cookie);
        IddCxMonitorDeparture(MonitorCreateOut.MonitorObject);
        return STATUS_CANCELLED;
    }
    return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::Plug(const NYANVDD_PLUG_IN& In, UINT32* ConnectorIndexOut)
{
    if (In.Cookie == 0 ||
        In.Width < 640 || In.Width > 7680 ||
        In.Height < 480 || In.Height > 4320 ||
        In.RefreshHz < 24 || In.RefreshHz > 240)
    {
        return STATUS_INVALID_PARAMETER;
    }

    UINT Index = MAXUINT;
    {
        lock_guard<mutex> Guard(m_Lock);
        if (!m_AdapterReady)
        {
            return STATUS_DEVICE_NOT_READY;
        }
        for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
        {
            if (m_Slots[i].Used && m_Slots[i].Params.Cookie == In.Cookie)
            {
                return STATUS_OBJECT_NAME_COLLISION;
            }
        }
        for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
        {
            if (!m_Slots[i].Used)
            {
                Index = i;
                break;
            }
        }
        if (Index == MAXUINT)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        MonitorSlot& Slot = m_Slots[Index];
        Slot = {};
        Slot.Used = true;
        Slot.Params = In;
        if (!(m_CapFlags & NYANVDD_CAP_HDR10_READY))
        {
            Slot.Params.Flags &= ~NYANVDD_PLUG_FLAG_HDR10;
        }
        BuildEdid(In.Cookie, In.Width, In.Height, In.RefreshHz, Slot.Edid);
    }

    NTSTATUS Status = CreateAndArriveMonitor(Index);
    if (!NT_SUCCESS(Status))
    {
        lock_guard<mutex> Guard(m_Lock);
        // Only roll back our own reservation — the slot may already have
        // been cleared (raced unplug) and even reused by another plug.
        if (m_Slots[Index].Used && m_Slots[Index].Params.Cookie == In.Cookie && !m_Slots[Index].Arrived)
        {
            m_Slots[Index] = {};
        }
        return Status;
    }

    NYVDD_LOG(L"Plugged cookie 0x%08X on connector %u (%ux%u@%u, flags 0x%X)",
              In.Cookie, Index, In.Width, In.Height, In.RefreshHz, In.Flags);
    *ConnectorIndexOut = Index;
    return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::Unplug(UINT32 Cookie)
{
    IDDCX_MONITOR Targets[NYANVDD_MAX_MONITORS];
    UINT32 TargetCount = 0;
    bool Found = false;
    {
        lock_guard<mutex> Guard(m_Lock);
        for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
        {
            MonitorSlot& Slot = m_Slots[i];
            if (Slot.Used && (Cookie == 0 || Slot.Params.Cookie == Cookie))
            {
                Found = true;
                if (Slot.Arrived && Slot.Monitor)
                {
                    Targets[TargetCount++] = Slot.Monitor;
                }
                Slot = {};
            }
        }
    }

    if (!Found && Cookie != 0)
    {
        return STATUS_NOT_FOUND;
    }

    for (UINT32 i = 0; i < TargetCount; ++i)
    {
        NTSTATUS Status = IddCxMonitorDeparture(Targets[i]);
        if (!NT_SUCCESS(Status))
        {
            NYVDD_LOG(L"IddCxMonitorDeparture failed: 0x%08X", Status);
        }
    }

    NYVDD_LOG(L"Unplugged cookie 0x%08X (%u monitor(s) departed)", Cookie, TargetCount);
    return STATUS_SUCCESS;
}

void IndirectDeviceContext::List(NYANVDD_LIST_OUT* Out)
{
    memset(Out, 0, sizeof(*Out));
    lock_guard<mutex> Guard(m_Lock);
    for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
    {
        const MonitorSlot& Slot = m_Slots[i];
        if (Slot.Used && Slot.Arrived)
        {
            NYANVDD_MONITOR_INFO& Info = Out->Monitors[Out->Count++];
            Info.Cookie = Slot.Params.Cookie;
            Info.ConnectorIndex = i;
            Info.Width = Slot.Params.Width;
            Info.Height = Slot.Params.Height;
            Info.RefreshHz = Slot.Params.RefreshHz;
            Info.Flags = Slot.Params.Flags;
        }
    }
}

void IndirectDeviceContext::FillStatus(NYANVDD_STATUS_OUT* Out)
{
    memset(Out, 0, sizeof(*Out));
    Out->ProtocolVersion = NYANVDD_PROTOCOL_VERSION;
    Out->DriverVersion = (NYANVDD_DRIVER_VERSION_MAJOR << 16) | NYANVDD_DRIVER_VERSION_MINOR;
    Out->MaxMonitors = NYANVDD_MAX_MONITORS;

    lock_guard<mutex> Guard(m_Lock);
    Out->IddCxOsVersion = m_OsVersion;
    Out->CapFlags = m_CapFlags;
    Out->WatchdogTimeoutMs = m_WatchdogTimeoutMs;
    for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
    {
        if (m_Slots[i].Used && m_Slots[i].Arrived)
        {
            Out->MonitorCount++;
        }
    }
}

bool IndirectDeviceContext::CopySlotByEdid(const void* Data, UINT32 Size, MonitorSlot* SlotOut)
{
    UINT32 Cookie = CookieFromEdid(Data, Size);
    if (Cookie == 0)
    {
        return false;
    }

    lock_guard<mutex> Guard(m_Lock);
    for (UINT i = 0; i < NYANVDD_MAX_MONITORS; ++i)
    {
        if (m_Slots[i].Used && m_Slots[i].Params.Cookie == Cookie)
        {
            *SlotOut = m_Slots[i];
            return true;
        }
    }
    return false;
}

NTSTATUS IndirectDeviceContext::SetWatchdog(UINT32 TimeoutMs)
{
    lock_guard<mutex> Guard(m_Lock);

    if (TimeoutMs != 0 && TimeoutMs < 10000)
    {
        TimeoutMs = 10000; // structural floor: transient stalls must not fire it
    }

    if (TimeoutMs != 0 && !m_WatchdogThread)
    {
        m_WatchdogWake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_WatchdogStop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        m_WatchdogThread = CreateThread(nullptr, 0, WatchdogThread, this, 0, nullptr);
        if (!m_WatchdogThread)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    m_WatchdogTimeoutMs = TimeoutMs;
    m_WatchdogDeadline = GetTickCount64() + TimeoutMs;
    if (m_WatchdogWake)
    {
        SetEvent(m_WatchdogWake);
    }
    NYVDD_LOG(L"Watchdog %s (timeout %u ms)", TimeoutMs ? L"armed" : L"disarmed", TimeoutMs);
    return STATUS_SUCCESS;
}

void IndirectDeviceContext::NoteRealtimeGpuPriority(bool Applied)
{
    lock_guard<mutex> Guard(m_Lock);
    if (Applied)
    {
        m_CapFlags |= NYANVDD_CAP_RT_GPU_PRIORITY;
    }
    else
    {
        m_CapFlags &= ~NYANVDD_CAP_RT_GPU_PRIORITY;
    }
}

void IndirectDeviceContext::PetWatchdog()
{
    if (m_WatchdogTimeoutMs != 0)
    {
        m_WatchdogDeadline = GetTickCount64() + m_WatchdogTimeoutMs;
    }
}

DWORD CALLBACK IndirectDeviceContext::WatchdogThread(LPVOID Argument)
{
    reinterpret_cast<IndirectDeviceContext*>(Argument)->WatchdogLoop();
    return 0;
}

void IndirectDeviceContext::WatchdogLoop()
{
    HANDLE WaitHandles[] = { m_WatchdogStop, m_WatchdogWake };
    for (;;)
    {
        DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 1000);
        if (WaitResult == WAIT_OBJECT_0)
        {
            return; // device teardown
        }

        if (m_WatchdogTimeoutMs != 0 && GetTickCount64() > m_WatchdogDeadline)
        {
            NYVDD_LOG(L"Watchdog fired — unplugging all monitors");
            m_WatchdogTimeoutMs = 0;
            Unplug(0);
        }
    }
}

#pragma endregion

#pragma region IndirectMonitorContext

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
    m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // Delete the swap-chain so the OS generates a new one and retries.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DdiCallbacks

_Use_decl_annotations_
NTSTATUS NyanVddAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    pDeviceContextWrapper->pContext->OnAdapterInitFinished(pInArgs->AdapterInitStatus);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    // Nothing to reconfigure: swap-chains are managed by IddCx and this
    // device has no transport of its own.
    return STATUS_SUCCESS;
}

// Shared implementation for ParseMonitorDescription: resolves the slot from
// the EDID cookie and reports its mode list (preferred mode first). Falls
// back to the static table for EDIDs we cannot resolve (e.g. a query racing
// an unplug).
static NTSTATUS ParseDescriptionCommon(
    const IDDCX_MONITOR_DESCRIPTION& Description,
    UINT32 InputCount,
    _Out_ UINT32* OutputCount,
    _Out_ UINT32* PreferredIdx,
    ModeEntry Modes[kMaxModes],
    _Out_ bool* Hdr10)
{
    if (Description.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID ||
        Description.DataSize != 128)
    {
        return STATUS_INVALID_PARAMETER;
    }

    MonitorSlot Slot;
    const bool Resolved = g_DeviceContext &&
        g_DeviceContext->CopySlotByEdid(Description.pData, Description.DataSize, &Slot);

    const UINT32 Count = BuildModeList(Resolved ? &Slot.Params : nullptr, Modes);
    *OutputCount = Count;
    *PreferredIdx = 0;
    *Hdr10 = Resolved && (Slot.Params.Flags & NYANVDD_PLUG_FLAG_HDR10) != 0;

    if (InputCount < Count)
    {
        return (InputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    ModeEntry Modes[kMaxModes];
    bool Hdr10 = false;
    NTSTATUS Status = ParseDescriptionCommon(pInArgs->MonitorDescription,
                                             pInArgs->MonitorModeBufferInputCount,
                                             &pOutArgs->MonitorModeBufferOutputCount,
                                             &pOutArgs->PreferredMonitorModeIdx,
                                             Modes, &Hdr10);
    if (Status != STATUS_SUCCESS || pInArgs->MonitorModeBufferInputCount == 0 ||
        pInArgs->pMonitorModes == nullptr)
    {
        return Status;
    }

    for (UINT32 i = 0; i < pOutArgs->MonitorModeBufferOutputCount; ++i)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(Modes[i],
            i == 0 ? IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR : IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // All nyanvdd monitors carry an EDID, so this path is not expected; keep
    // it functional with the static table for robustness.
    pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(kModeTable);
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < ARRAYSIZE(kModeTable))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (UINT32 i = 0; i < ARRAYSIZE(kModeTable); ++i)
    {
        pInArgs->pDefaultMonitorModes[i] = CreateIddCxMonitorMode(kModeTable[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Target modes describe the device's processing capability; report the
    // full static table (the OS intersects them with the monitor modes).
    pOutArgs->TargetModeBufferOutputCount = ARRAYSIZE(kModeTable);
    if (pInArgs->TargetModeBufferInputCount >= ARRAYSIZE(kModeTable))
    {
        for (UINT32 i = 0; i < ARRAYSIZE(kModeTable); ++i)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode(kModeTable[i]);
        }
    }
    return STATUS_SUCCESS;
}

#if IDDCX_VERSION_MINOR >= 10

_Use_decl_annotations_
NTSTATUS NyanVddAdapterCommitModes2(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES2* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddParseMonitorDescription2(const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION2* pOutArgs)
{
    ModeEntry Modes[kMaxModes];
    bool Hdr10 = false;
    NTSTATUS Status = ParseDescriptionCommon(pInArgs->MonitorDescription,
                                             pInArgs->MonitorModeBufferInputCount,
                                             &pOutArgs->MonitorModeBufferOutputCount,
                                             &pOutArgs->PreferredMonitorModeIdx,
                                             Modes, &Hdr10);
    if (Status != STATUS_SUCCESS || pInArgs->MonitorModeBufferInputCount == 0 ||
        pInArgs->pMonitorModes == nullptr)
    {
        return Status;
    }

    for (UINT32 i = 0; i < pOutArgs->MonitorModeBufferOutputCount; ++i)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode2(Modes[i],
            i == 0 ? IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR : IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
            Hdr10);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddMonitorSetDefaultHdrMetaData(IDDCX_MONITOR MonitorObject, const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    // Frames are not transported by this driver, so the default HDR metadata
    // has nothing to be applied to — accept and ignore.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES2* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    pOutArgs->TargetModeBufferOutputCount = ARRAYSIZE(kModeTable);
    if (pInArgs->TargetModeBufferInputCount >= ARRAYSIZE(kModeTable))
    {
        for (UINT32 i = 0; i < ARRAYSIZE(kModeTable); ++i)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode2(kModeTable[i], true);
        }
    }
    return STATUS_SUCCESS;
}

#endif // IDDCX_VERSION_MINOR >= 10

_Use_decl_annotations_
NTSTATUS NyanVddMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NyanVddMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion

#pragma region ControlPlane

_Use_decl_annotations_
void NyanVddIoDeviceControl(WDFDEVICE Device, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    IndirectDeviceContext* Context = IndirectDeviceContext::Get(Device);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;

    switch (IoControlCode)
    {
    case IOCTL_NYANVDD_GET_STATUS:
    {
        NYANVDD_STATUS_OUT* Out = nullptr;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID*)&Out, nullptr);
        if (NT_SUCCESS(Status))
        {
            Context->FillStatus(Out);
            Information = sizeof(*Out);
        }
        break;
    }
    case IOCTL_NYANVDD_PLUG:
    {
        NYANVDD_PLUG_IN* In = nullptr;
        NYANVDD_PLUG_OUT* Out = nullptr;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID*)&In, nullptr);
        if (NT_SUCCESS(Status))
        {
            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID*)&Out, nullptr);
        }
        if (NT_SUCCESS(Status))
        {
            UINT32 Connector = 0;
            Status = Context->Plug(*In, &Connector);
            if (NT_SUCCESS(Status))
            {
                Out->ConnectorIndex = Connector;
                Information = sizeof(*Out);
            }
        }
        break;
    }
    case IOCTL_NYANVDD_UNPLUG:
    {
        NYANVDD_UNPLUG_IN* In = nullptr;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID*)&In, nullptr);
        if (NT_SUCCESS(Status))
        {
            Status = Context->Unplug(In->Cookie);
        }
        break;
    }
    case IOCTL_NYANVDD_LIST:
    {
        NYANVDD_LIST_OUT* Out = nullptr;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID*)&Out, nullptr);
        if (NT_SUCCESS(Status))
        {
            Context->List(Out);
            Information = sizeof(*Out);
        }
        break;
    }
    case IOCTL_NYANVDD_SET_WATCHDOG:
    {
        NYANVDD_WATCHDOG_IN* In = nullptr;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID*)&In, nullptr);
        if (NT_SUCCESS(Status))
        {
            Status = Context->SetWatchdog(In->TimeoutMs);
        }
        break;
    }
    default:
        break;
    }

    if (NT_SUCCESS(Status))
    {
        // Liveness signal: every successful control call refreshes the
        // (optional) watchdog.
        Context->PetWatchdog();
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

#pragma endregion
