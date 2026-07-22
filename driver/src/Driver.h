// nyan Real Virtual Display Driver — an IddCx indirect display driver,
// built for nyan Real (Spatial Wall).
//
// Portions derived from the Microsoft Windows-driver-samples IndirectDisplay
// sample (MIT License, Copyright (c) Microsoft Corporation).

#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <mutex>

#include <winioctl.h>
#include "../../include/nyanvdd_protocol.h"
#include "Edid.h"

#define NYANVDD_DRIVER_VERSION_MAJOR 0
#define NYANVDD_DRIVER_VERSION_MINOR 1

// Lightweight debug logging (view with DebugView / WinDbg). No WPP: keeps the
// public source self-contained and greppable.
void NyanVddLog(_In_z_ _Printf_format_string_ const wchar_t* Format, ...);
#define NYVDD_LOG(...) NyanVddLog(__VA_ARGS__)

namespace nyan
{
    namespace vdd
    {
        /// Manages the creation and lifetime of a Direct3D render device.
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// Consumes buffers from an indirect display swap-chain on its own
        /// thread. Frames are acquired and immediately released: this driver's
        /// monitors exist to be composed by DWM and captured by the app
        /// (Windows.Graphics.Capture), not to transport pixels themselves.
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::unique_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

            SwapChainProcessor(const SwapChainProcessor&) = delete;
            SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

            // Starts the processing thread after construction so allocation
            // and Win32 handle failures can be reported without leaving an
            // assigned swap-chain unowned.
            HRESULT Start();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);
            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::unique_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            HANDLE m_hThread = nullptr;
            HANDLE m_hTerminateEvent = nullptr;
            bool m_RtPriorityHeld = false;
        };

        struct MonitorSlot
        {
            bool Used = false;          // slot reserved (plug in progress or live)
            bool Arrived = false;       // IddCxMonitorArrival succeeded
            bool Active = false;        // the OS committed a display path for it
            NYANVDD_PLUG_IN Params = {};
            IDDCX_MONITOR Monitor = nullptr;
            BYTE Edid[128] = {};
        };

        /// Per-device state: the IddCx adapter, the monitor slot table and the
        /// optional watchdog. All public entry points are thread-safe.
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            ~IndirectDeviceContext();

            void InitAdapter();
            void OnAdapterInitFinished(NTSTATUS Status);

            // Control plane (called from EvtIddCxDeviceIoControl).
            NTSTATUS Plug(const NYANVDD_PLUG_IN& In, _Out_ UINT32* ConnectorIndexOut);
            NTSTATUS Unplug(UINT32 Cookie); // 0 = all
            void List(_Out_ NYANVDD_LIST_OUT* Out);

            // Records which monitors the OS is actually driving, from the
            // committed display paths. Monitors absent from the list are
            // marked inactive, which tells a client the OS has not attached
            // the monitor to a desktop (as opposed to having attached it to a
            // desktop the client's session cannot see — see the note on
            // NYANVDD_MONITOR_FLAG_ACTIVE).
            void SetActiveMonitors(const IDDCX_MONITOR* Monitors, UINT32 Count);
            void FillStatus(_Out_ NYANVDD_STATUS_OUT* Out);
            NTSTATUS SetWatchdog(UINT32 TimeoutMs);
            void PetWatchdog();

            // NYANVDD_CAP_RT_GPU_PRIORITY reflects swap-chains that currently
            // hold realtime priority, so it has to be reference counted: it is
            // acquired per swap-chain and must drop when the last one goes away
            // (otherwise status keeps advertising it with no monitors plugged).
            void AddRealtimeGpuPriorityRef();
            void ReleaseRealtimeGpuPriorityRef();

            // Mode enumeration support (called from the DDI callbacks).
            // Copies the slot for the given EDID into *SlotOut; returns false
            // if the EDID does not match a live slot.
            bool CopySlotByEdid(const void* Data, UINT32 Size, _Out_ MonitorSlot* SlotOut);

            UINT32 OsVersion() const { return m_OsVersion; }
            bool Hdr10Ready() const { return (m_CapFlags & NYANVDD_CAP_HDR10_READY) != 0; }

            static IndirectDeviceContext* Get(WDFDEVICE Device);

        private:
            NTSTATUS CreateAndArriveMonitor(UINT ConnectorIndex);
            void WatchdogLoop();
            static DWORD CALLBACK WatchdogThread(LPVOID Argument);

            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter = nullptr;
            bool m_AdapterReady = false;
            UINT32 m_OsVersion = 0;   // IDDCX_VERSION from IddCxGetVersion, 0 if unavailable
            UINT32 m_CapFlags = 0;    // NYANVDD_CAP_*
            UINT32 m_RtPriorityRefs = 0;
            UINT32 m_AdapterState = NYANVDD_ADAPTER_STATE_STARTING;
            bool m_AdapterInitStarted = false;

            std::mutex m_Lock;
            MonitorSlot m_Slots[NYANVDD_MAX_MONITORS];

            // Watchdog (disarmed unless a client arms it).
            HANDLE m_WatchdogThread = nullptr;
            HANDLE m_WatchdogWake = nullptr;   // auto-reset: re-evaluate now
            HANDLE m_WatchdogStop = nullptr;   // manual-reset: thread exit
            UINT32 m_WatchdogTimeoutMs = 0;
            ULONGLONG m_WatchdogDeadline = 0;
        };

        /// Per-monitor state attached to the IDDCX_MONITOR object.
        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext() = default;
            ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

        private:
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        };
    }
}
