// nyan Real VDD — IddCx indirect display driver for Spatial Wall.
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
        // Builds a 128-byte EDID base block: vendor "NYN", product 0x3D0F,
        // serial = Cookie, product name "nyan Wall", serial string
        // "NW-XXXXXXXX", preferred DTD from the plug parameters (falls back to
        // 1080p60 when the mode is not DTD-encodable, i.e. pixel clock over
        // 655.35 MHz).
        void BuildEdid(UINT32 Cookie, UINT32 Width, UINT32 Height, UINT32 RefreshHz, BYTE Out[128]);

        // Reads the serial number (= cookie) back out of an EDID base block.
        // Returns 0 if the blob is not one of ours.
        UINT32 CookieFromEdid(const void* Data, UINT32 Size);

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
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);
            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            HANDLE m_hThread = nullptr;
            HANDLE m_hTerminateEvent = nullptr;
        };

        struct MonitorSlot
        {
            bool Used = false;          // slot reserved (plug in progress or live)
            bool Arrived = false;       // IddCxMonitorArrival succeeded
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
            void FillStatus(_Out_ NYANVDD_STATUS_OUT* Out);
            NTSTATUS SetWatchdog(UINT32 TimeoutMs);
            void PetWatchdog();
            void NoteRealtimeGpuPriority(bool Applied);

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

            std::mutex m_Lock;
            MonitorSlot m_Slots[NYANVDD_MAX_MONITORS];

            // Watchdog (disarmed unless a client arms it).
            HANDLE m_WatchdogThread = nullptr;
            HANDLE m_WatchdogWake = nullptr;   // auto-reset: re-evaluate now
            HANDLE m_WatchdogStop = nullptr;   // manual-reset: thread exit
            volatile UINT32 m_WatchdogTimeoutMs = 0;
            volatile ULONGLONG m_WatchdogDeadline = 0;
        };

        /// Per-monitor state attached to the IDDCX_MONITOR object.
        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor);
            ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

        private:
            IDDCX_MONITOR m_Monitor;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        };
    }
}
