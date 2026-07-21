// nyanvddctl — control CLI for the nyan Real Virtual Display Driver.
//
// Doubles as the reference client for the protocol in
// include/nyanvdd_protocol.h and as the device-node installer
// (SwDeviceCreate) used by scripts/install.ps1.

#include <windows.h>
#include <winioctl.h>
#include <swdevice.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <cwchar>
#include <random>
#include <string>
#include <vector>

#include "../include/nyanvdd_protocol.h"

#pragma comment(lib, "swdevice.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace
{
    const GUID kInterfaceGuid = NYANVDD_INTERFACE_GUID_INIT;

    void PrintUsage()
    {
        wprintf(
            L"nyanvddctl — nyan Real Virtual Display control tool\n"
            L"\n"
            L"  nyanvddctl install-device        create the persistent VDD device node\n"
            L"  nyanvddctl remove-device         remove the VDD device node\n"
            L"  nyanvddctl status                driver/protocol/OS-IddCx versions and caps\n"
            L"  nyanvddctl list                  list plugged monitors\n"
            L"  nyanvddctl plug [WxH@Hz] [--hdr] [--cookie N]\n"
            L"                                   plug a monitor (default 1920x1080@60)\n"
            L"  nyanvddctl unplug <cookie|all>   unplug one monitor (hex ok: 0x...) or all\n"
            L"  nyanvddctl watchdog <ms|off>     arm/disarm the liveness watchdog (>=10000)\n");
    }

    // ---- device node management (SwDeviceCreate) ----

    struct CreateContext
    {
        HANDLE Event;
        HRESULT Result;
    };

    VOID WINAPI CreationCallback(
        _In_ HSWDEVICE hSwDevice,
        _In_ HRESULT hrCreateResult,
        _In_opt_ PVOID pContext,
        _In_opt_ PCWSTR pszDeviceInstanceId)
    {
        UNREFERENCED_PARAMETER(hSwDevice);
        UNREFERENCED_PARAMETER(pszDeviceInstanceId);
        auto* Context = (CreateContext*)pContext;
        Context->Result = hrCreateResult;
        SetEvent(Context->Event);
    }

    // Creates (or reopens) the SWD instance for the VDD adapter and applies
    // the requested lifetime. Returns 0 on success.
    int SetDeviceLifetime(SW_DEVICE_LIFETIME Lifetime)
    {
        CreateContext Context = { CreateEventW(nullptr, FALSE, FALSE, nullptr), E_FAIL };

        SW_DEVICE_CREATE_INFO CreateInfo = {};
        CreateInfo.cbSize = sizeof(CreateInfo);
        CreateInfo.pszInstanceId = NYANVDD_INSTANCE_ID;
        CreateInfo.pszzHardwareIds = NYANVDD_HARDWARE_ID L"\0";
        CreateInfo.pszzCompatibleIds = NYANVDD_HARDWARE_ID L"\0";
        CreateInfo.pszDeviceDescription = L"nyan Real Virtual Display Adapter";
        CreateInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                     SWDeviceCapabilitiesSilentInstall |
                                     SWDeviceCapabilitiesDriverRequired;

        HSWDEVICE hSwDevice = nullptr;
        HRESULT hr = SwDeviceCreate(L"NyanVDD", L"HTREE\\ROOT\\0", &CreateInfo, 0, nullptr,
                                    CreationCallback, &Context, &hSwDevice);
        if (FAILED(hr))
        {
            fwprintf(stderr, L"SwDeviceCreate failed: 0x%08X\n", hr);
            return 1;
        }

        if (WaitForSingleObject(Context.Event, 10 * 1000) != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"timed out waiting for device creation\n");
            SwDeviceClose(hSwDevice);
            return 1;
        }
        if (FAILED(Context.Result))
        {
            fwprintf(stderr, L"device creation failed: 0x%08X\n", Context.Result);
            SwDeviceClose(hSwDevice);
            return 1;
        }

        hr = SwDeviceSetLifetime(hSwDevice, Lifetime);
        if (FAILED(hr))
        {
            fwprintf(stderr, L"SwDeviceSetLifetime failed: 0x%08X\n", hr);
            SwDeviceClose(hSwDevice);
            return 1;
        }

        // With SWDeviceLifetimeParentPresent the device survives this close
        // (and reboots); with SWDeviceLifetimeHandle the close removes it.
        SwDeviceClose(hSwDevice);
        return 0;
    }

    // ---- control channel ----

    HANDLE OpenDevice()
    {
        ULONG Length = 0;
        CONFIGRET Ret = CM_Get_Device_Interface_List_SizeW(
            &Length, const_cast<GUID*>(&kInterfaceGuid), nullptr,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (Ret != CR_SUCCESS || Length <= 1)
        {
            fwprintf(stderr,
                L"no nyanvdd device found — is the driver installed and the device created?\n"
                L"  (scripts\\install.ps1, or: pnputil /add-driver nyanvdd.inf /install && nyanvddctl install-device)\n");
            return INVALID_HANDLE_VALUE;
        }

        std::vector<wchar_t> List(Length);
        Ret = CM_Get_Device_Interface_ListW(
            const_cast<GUID*>(&kInterfaceGuid), nullptr, List.data(), Length,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (Ret != CR_SUCCESS || List[0] == L'\0')
        {
            fwprintf(stderr, L"CM_Get_Device_Interface_ListW failed: %u\n", Ret);
            return INVALID_HANDLE_VALUE;
        }

        HANDLE Device = CreateFileW(List.data(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (Device == INVALID_HANDLE_VALUE)
        {
            fwprintf(stderr, L"CreateFile(%s) failed: %lu\n", List.data(), GetLastError());
        }
        return Device;
    }

    bool Ioctl(HANDLE Device, DWORD Code, const void* In, DWORD InSize, void* Out, DWORD OutSize)
    {
        DWORD Returned = 0;
        if (!DeviceIoControl(Device, Code, const_cast<void*>(In), InSize, Out, OutSize, &Returned, nullptr))
        {
            fwprintf(stderr, L"DeviceIoControl failed: %lu\n", GetLastError());
            return false;
        }
        return true;
    }

    // ---- commands ----

    int CmdStatus(HANDLE Device)
    {
        NYANVDD_STATUS_OUT Status = {};
        if (!Ioctl(Device, IOCTL_NYANVDD_GET_STATUS, nullptr, 0, &Status, sizeof(Status)))
        {
            return 1;
        }
        wprintf(L"protocol   : v%u\n", Status.ProtocolVersion);
        wprintf(L"driver     : %u.%u\n", Status.DriverVersion >> 16, Status.DriverVersion & 0xFFFF);
        wprintf(L"os iddcx   : 0x%04X\n", Status.IddCxOsVersion);
        wprintf(L"monitors   : %u / %u\n", Status.MonitorCount, Status.MaxMonitors);
        wprintf(L"caps       :%s%s%s%s\n",
                (Status.CapFlags & NYANVDD_CAP_HDR10_READY) ? L" hdr10-ready" : L"",
                (Status.CapFlags & NYANVDD_CAP_RT_GPU_PRIORITY) ? L" rt-gpu-priority" : L"",
                (Status.CapFlags & NYANVDD_CAP_PRECISE_DIRTY) ? L" precise-dirty" : L"",
                (Status.CapFlags == 0) ? L" (none)" : L"");
        wprintf(L"watchdog   : %s (%u ms)\n", Status.WatchdogTimeoutMs ? L"armed" : L"off",
                Status.WatchdogTimeoutMs);
        return 0;
    }

    int CmdList(HANDLE Device)
    {
        NYANVDD_LIST_OUT List = {};
        if (!Ioctl(Device, IOCTL_NYANVDD_LIST, nullptr, 0, &List, sizeof(List)))
        {
            return 1;
        }
        if (List.Count == 0)
        {
            wprintf(L"no monitors plugged\n");
            return 0;
        }
        for (UINT32 i = 0; i < List.Count; ++i)
        {
            const NYANVDD_MONITOR_INFO& M = List.Monitors[i];
            wprintf(L"connector %u: cookie 0x%08X  %ux%u@%u%s\n",
                    M.ConnectorIndex, M.Cookie, M.Width, M.Height, M.RefreshHz,
                    (M.Flags & NYANVDD_PLUG_FLAG_HDR10) ? L"  [hdr10]" : L"");
        }
        return 0;
    }

    int CmdPlug(HANDLE Device, int argc, wchar_t** argv)
    {
        NYANVDD_PLUG_IN In = {};
        In.Width = 1920;
        In.Height = 1080;
        In.RefreshHz = 60;

        for (int i = 0; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"--hdr") == 0)
            {
                In.Flags |= NYANVDD_PLUG_FLAG_HDR10;
            }
            else if (wcscmp(argv[i], L"--cookie") == 0 && i + 1 < argc)
            {
                In.Cookie = wcstoul(argv[++i], nullptr, 0);
            }
            else if (swscanf_s(argv[i], L"%ux%u@%u", &In.Width, &In.Height, &In.RefreshHz) == 3)
            {
                // parsed WxH@Hz
            }
            else
            {
                fwprintf(stderr, L"unrecognized argument: %s\n", argv[i]);
                return 2;
            }
        }

        if (In.Cookie == 0)
        {
            std::random_device Rd;
            do { In.Cookie = Rd(); } while (In.Cookie == 0);
        }

        NYANVDD_PLUG_OUT Out = {};
        if (!Ioctl(Device, IOCTL_NYANVDD_PLUG, &In, sizeof(In), &Out, sizeof(Out)))
        {
            return 1;
        }
        wprintf(L"plugged cookie 0x%08X on connector %u (%ux%u@%u)\n",
                In.Cookie, Out.ConnectorIndex, In.Width, In.Height, In.RefreshHz);
        return 0;
    }

    int CmdUnplug(HANDLE Device, const wchar_t* Arg)
    {
        NYANVDD_UNPLUG_IN In = {};
        if (wcscmp(Arg, L"all") == 0)
        {
            In.Cookie = 0;
        }
        else
        {
            In.Cookie = wcstoul(Arg, nullptr, 0);
            if (In.Cookie == 0)
            {
                fwprintf(stderr, L"bad cookie: %s\n", Arg);
                return 2;
            }
        }
        if (!Ioctl(Device, IOCTL_NYANVDD_UNPLUG, &In, sizeof(In), nullptr, 0))
        {
            return 1;
        }
        wprintf(L"unplugged\n");
        return 0;
    }

    int CmdWatchdog(HANDLE Device, const wchar_t* Arg)
    {
        NYANVDD_WATCHDOG_IN In = {};
        In.TimeoutMs = (wcscmp(Arg, L"off") == 0) ? 0 : wcstoul(Arg, nullptr, 0);
        if (!Ioctl(Device, IOCTL_NYANVDD_SET_WATCHDOG, &In, sizeof(In), nullptr, 0))
        {
            return 1;
        }
        wprintf(In.TimeoutMs ? L"watchdog armed (>=10000 ms enforced by driver)\n"
                             : L"watchdog off\n");
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 2;
    }

    const wchar_t* Cmd = argv[1];

    if (wcscmp(Cmd, L"install-device") == 0)
    {
        int Result = SetDeviceLifetime(SWDeviceLifetimeParentPresent);
        if (Result == 0)
        {
            wprintf(L"device created (persistent)\n");
        }
        return Result;
    }
    if (wcscmp(Cmd, L"remove-device") == 0)
    {
        int Result = SetDeviceLifetime(SWDeviceLifetimeHandle);
        if (Result == 0)
        {
            wprintf(L"device removed\n");
        }
        return Result;
    }

    HANDLE Device = OpenDevice();
    if (Device == INVALID_HANDLE_VALUE)
    {
        return 1;
    }

    int Result = 2;
    if (wcscmp(Cmd, L"status") == 0)
    {
        Result = CmdStatus(Device);
    }
    else if (wcscmp(Cmd, L"list") == 0)
    {
        Result = CmdList(Device);
    }
    else if (wcscmp(Cmd, L"plug") == 0)
    {
        Result = CmdPlug(Device, argc - 2, argv + 2);
    }
    else if (wcscmp(Cmd, L"unplug") == 0 && argc >= 3)
    {
        Result = CmdUnplug(Device, argv[2]);
    }
    else if (wcscmp(Cmd, L"watchdog") == 0 && argc >= 3)
    {
        Result = CmdWatchdog(Device, argv[2]);
    }
    else
    {
        PrintUsage();
    }

    CloseHandle(Device);
    return Result;
}
