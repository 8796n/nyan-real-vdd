// nyan Real VDD — public control protocol
//
// This header is the single source of truth for the contract between the
// nyanvdd driver and its clients (Spatial Wall, nyanvddctl, third parties).
// It is self-contained C: copy it verbatim into consuming projects.
//
// Include <windows.h> before this header in user mode; it pulls in
// <winioctl.h> itself for CTL_CODE. The driver includes it after the WDK
// headers.
//
// Versioning rules:
//  - Any layout or semantic change bumps NYANVDD_PROTOCOL_VERSION.
//  - Clients must call IOCTL_NYANVDD_GET_STATUS first and refuse to drive a
//    driver whose ProtocolVersion they do not know.
//
// Design notes (why this protocol looks the way it does):
//  - Every monitor is identified by a client-chosen non-zero 32-bit cookie.
//    The cookie is burned into the monitor's EDID serial number (bytes 12-15)
//    and into the EDID serial-string descriptor ("NW-XXXXXXXX"), so a client
//    can always correlate an OS display with its own request — connector
//    indices are never reused as identity.
//  - There is NO mandatory keepalive. Monitors persist until unplugged or the
//    device is removed. Clients are expected to reconcile at startup: LIST,
//    then UNPLUG any cookie they do not recognize. An OPTIONAL watchdog can
//    be armed by the controlling client; any successful IOCTL refreshes it.

#ifndef NYANVDD_PROTOCOL_H
#define NYANVDD_PROTOCOL_H

#if !defined(CTL_CODE)
#include <winioctl.h>
#endif

#ifdef __cplusplus
extern "C" {
#define NYANVDD_INLINE inline
#else
#define NYANVDD_INLINE __inline
#endif

// v2: NYANVDD_STATUS_OUT.Reserved became AdapterState (layout unchanged), and
//     the container-id correlation helpers below became part of the contract.
#define NYANVDD_PROTOCOL_VERSION 2

// Device interface exposed by the driver. Enumerate with
// CM_Get_Device_Interface_ListW and open with CreateFileW
// (GENERIC_READ | GENERIC_WRITE, share read+write).
// {C9AC49E6-0024-4979-96C7-A3E4B911CFFC}
#define NYANVDD_INTERFACE_GUID_INIT \
    { 0xC9AC49E6, 0x0024, 0x4979, { 0x96, 0xC7, 0xA3, 0xE4, 0xB9, 0x11, 0xCF, 0xFC } }

// PnP identity. The SWD instance is created by the installing client
// (nyanvddctl install-device) with this hardware id; the INF matches it.
#define NYANVDD_HARDWARE_ID   L"NyanVDD"
#define NYANVDD_INSTANCE_ID   L"NyanVDD"

// EDID identity of every monitor this driver reports.
// Manufacturer "NYN", product code 0x3D0F, serial = plug cookie.
#define NYANVDD_EDID_VENDOR       "NYN"
#define NYANVDD_EDID_PRODUCT_CODE 0x3D0F

// The same identity as DISPLAYCONFIG_TARGET_DEVICE_NAME reports it, for use
// as a cheap pre-filter over QueryDisplayConfig results.
#define NYANVDD_EDID_MANUFACTURE_ID  0x2E3B
#define NYANVDD_EDID_PRODUCT_CODE_ID 0x3D0F

// ---------------------------------------------------------------------------
// Correlating a cookie with an OS display
//
// Each monitor's container id is derived from its plug cookie, so a client can
// map either direction without parsing EDID blobs. Recipe, all non-privileged:
//
//   1. GetDisplayConfigBufferSizes + QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)
//   2. per path: DisplayConfigGetDeviceInfo(DISPLAYCONFIG_TARGET_DEVICE_NAME)
//      -> monitorDevicePath, e.g.
//         \\?\DISPLAY#NYN3D0F#1&37a367&0&UID256#{e6f07b5f-...}
//      (optionally skip non-ours early via edidManufactureId /
//       edidProductCodeId against the two constants above)
//   3. turn that path into a device instance id: drop the leading "\\?\",
//      drop the trailing "#{...}", replace '#' with '\'
//   4. CM_Locate_DevNodeW + CM_Get_DevNode_PropertyW(DEVPKEY_Device_ContainerId)
//   5. NyanVddCookieFromContainerId() -> the cookie, or 0 if not ours
//   6. same path's adapterId/sourceId + DISPLAYCONFIG_SOURCE_DEVICE_NAME ->
//      viewGdiDeviceName ("\\.\DISPLAY3"), which EnumDisplaySettingsEx and
//      EnumDisplayMonitors accept.
//
// cli/nyanvddctl.cpp implements exactly this ("nyanvddctl resolve") and is
// meant to be read as the reference implementation.
//
// TIMING: a successful PLUG means the monitor arrival was accepted, not that
// the OS has finished applying the new topology. The display will not appear
// in QueryDisplayConfig / EnumDisplayDevices for a short while afterwards.
// Subscribe to WM_DISPLAYCHANGE (or CM_Register_Notification on
// GUID_DEVINTERFACE_MONITOR) before calling PLUG, or poll the lookup above
// with a timeout; do not assume the display exists the instant PLUG returns.
// ---------------------------------------------------------------------------

// {408B3FE4-8AC2-4E97-83D8-BE29xxxxxxxx} — the low 4 bytes carry the cookie.
#define NYANVDD_CONTAINER_ID_BASE \
    { 0x408B3FE4, 0x8AC2, 0x4E97, { 0x83, 0xD8, 0xBE, 0x29, 0x00, 0x00, 0x00, 0x00 } }

NYANVDD_INLINE void NyanVddMakeContainerId(UINT32 Cookie, GUID* ContainerId)
{
    const GUID NyanVddBase = NYANVDD_CONTAINER_ID_BASE;
    *ContainerId = NyanVddBase;
    ContainerId->Data4[4] = (unsigned char)(Cookie & 0xFF);
    ContainerId->Data4[5] = (unsigned char)((Cookie >> 8) & 0xFF);
    ContainerId->Data4[6] = (unsigned char)((Cookie >> 16) & 0xFF);
    ContainerId->Data4[7] = (unsigned char)((Cookie >> 24) & 0xFF);
}

// Returns 0 if the container id does not belong to this driver.
NYANVDD_INLINE UINT32 NyanVddCookieFromContainerId(const GUID* ContainerId)
{
    const GUID NyanVddBase = NYANVDD_CONTAINER_ID_BASE;
    int i;
    if (ContainerId->Data1 != NyanVddBase.Data1 ||
        ContainerId->Data2 != NyanVddBase.Data2 ||
        ContainerId->Data3 != NyanVddBase.Data3)
    {
        return 0;
    }
    for (i = 0; i < 4; ++i)
    {
        if (ContainerId->Data4[i] != NyanVddBase.Data4[i]) return 0;
    }
    return ((UINT32)ContainerId->Data4[4]) |
           ((UINT32)ContainerId->Data4[5] << 8) |
           ((UINT32)ContainerId->Data4[6] << 16) |
           ((UINT32)ContainerId->Data4[7] << 24);
}

#define NYANVDD_MAX_MONITORS 4

#define NYANVDD_DEVICE_TYPE 0x8E56 // custom range (bit 15 set)
#define NYANVDD_IOCTL(Fn) \
    ((ULONG)CTL_CODE(NYANVDD_DEVICE_TYPE, 0x800 + (Fn), METHOD_BUFFERED, FILE_ANY_ACCESS))

#define IOCTL_NYANVDD_GET_STATUS   NYANVDD_IOCTL(0) // out: NYANVDD_STATUS_OUT
#define IOCTL_NYANVDD_PLUG         NYANVDD_IOCTL(1) // in: NYANVDD_PLUG_IN, out: NYANVDD_PLUG_OUT
#define IOCTL_NYANVDD_UNPLUG       NYANVDD_IOCTL(2) // in: NYANVDD_UNPLUG_IN
#define IOCTL_NYANVDD_LIST         NYANVDD_IOCTL(3) // out: NYANVDD_LIST_OUT
#define IOCTL_NYANVDD_SET_WATCHDOG NYANVDD_IOCTL(4) // in: NYANVDD_WATCHDOG_IN

#pragma pack(push, 4)

// Capability flags in NYANVDD_STATUS_OUT.CapFlags.
#define NYANVDD_CAP_HDR10_READY     0x00000001u // OS IddCx >= 1.10: HDR-capable plumbing active
#define NYANVDD_CAP_RT_GPU_PRIORITY 0x00000002u // OS IddCx >= 1.9: realtime GPU priority applied
#define NYANVDD_CAP_PRECISE_DIRTY   0x00000004u // OS IddCx >= 1.8: precise present regions requested

// AdapterState: PLUG returns ERROR_NOT_READY both while the adapter is still
// coming up and when it failed for good, so this is how a client tells a race
// at startup (retry) from a broken driver (report it and stop retrying).
#define NYANVDD_ADAPTER_STATE_STARTING 0 // initializing; PLUG may fail, retry
#define NYANVDD_ADAPTER_STATE_READY    1 // monitors can be plugged
#define NYANVDD_ADAPTER_STATE_FAILED   2 // IddCx refused the adapter; permanent

typedef struct NYANVDD_STATUS_OUT {
    UINT32 ProtocolVersion; // NYANVDD_PROTOCOL_VERSION of the driver build
    UINT32 DriverVersion;   // packed: (major << 16) | minor
    UINT32 IddCxOsVersion;  // IDDCX_VERSION reported by the OS (e.g. 0x1A00), 0 if unknown
    UINT32 MaxMonitors;     // NYANVDD_MAX_MONITORS of the driver build
    UINT32 CapFlags;        // NYANVDD_CAP_*
    UINT32 MonitorCount;    // currently plugged monitors
    UINT32 WatchdogTimeoutMs; // 0 = watchdog disarmed
    UINT32 AdapterState;    // NYANVDD_ADAPTER_STATE_* (was Reserved in v1)
} NYANVDD_STATUS_OUT;

// Plug flags.
#define NYANVDD_PLUG_FLAG_HDR10 0x00000001u // report 10-bit capability for this monitor (needs CAP_HDR10_READY)

// Accepted mode range. The driver refuses anything it cannot fully describe
// to the OS, so a successful PLUG means the monitor really is offered at the
// requested mode (it is reported both as a monitor mode and as a target mode,
// and it becomes the monitor's preferred mode).
//   640 <= Width  <= 4095      active pixels fit an EDID detailed timing
//   480 <= Height <= 4095
//    24 <= RefreshHz <= 240
//   (Height + 35) * RefreshHz <= 510000     line rate fits the EDID range limits
//   (Width + 160) * (Height + 35) * RefreshHz <= 2550000000   pixel clock ditto
// Anything outside this fails with ERROR_INVALID_PARAMETER.
#define NYANVDD_MIN_WIDTH      640u
#define NYANVDD_MIN_HEIGHT     480u
#define NYANVDD_MAX_DIMENSION  4095u
#define NYANVDD_MIN_REFRESH_HZ 24u
#define NYANVDD_MAX_REFRESH_HZ 240u

typedef struct NYANVDD_PLUG_IN {
    UINT32 Cookie;    // non-zero, unique per plugged monitor; becomes the EDID serial
    UINT32 Width;     // preferred mode, e.g. 1920
    UINT32 Height;    // e.g. 1080
    UINT32 RefreshHz; // e.g. 120
    UINT32 Flags;     // NYANVDD_PLUG_FLAG_*
    UINT32 Reserved[3];
} NYANVDD_PLUG_IN;

typedef struct NYANVDD_PLUG_OUT {
    UINT32 ConnectorIndex; // 0-based connector the monitor was attached to
} NYANVDD_PLUG_OUT;

typedef struct NYANVDD_UNPLUG_IN {
    UINT32 Cookie; // 0 = unplug ALL monitors
} NYANVDD_UNPLUG_IN;

typedef struct NYANVDD_MONITOR_INFO {
    UINT32 Cookie;
    UINT32 ConnectorIndex;
    UINT32 Width;
    UINT32 Height;
    UINT32 RefreshHz;
    UINT32 Flags; // NYANVDD_PLUG_FLAG_* as accepted at plug time
} NYANVDD_MONITOR_INFO;

typedef struct NYANVDD_LIST_OUT {
    UINT32 Count;
    NYANVDD_MONITOR_INFO Monitors[NYANVDD_MAX_MONITORS];
} NYANVDD_LIST_OUT;

// Optional watchdog. Disarmed by default and after it fires.
// TimeoutMs = 0 disarms; otherwise clamped to >= 10000 (10 s) to make
// spurious removals under system stalls structurally unlikely.
// ANY successful nyanvdd IOCTL refreshes the deadline. When the deadline
// passes, ALL monitors are unplugged and the watchdog disarms itself.
typedef struct NYANVDD_WATCHDOG_IN {
    UINT32 TimeoutMs;
} NYANVDD_WATCHDOG_IN;

#pragma pack(pop)

// Win32 error mapping of driver-side failures (via NTSTATUS):
//  - plug with duplicate cookie      -> ERROR_ALREADY_EXISTS
//  - plug with no free connector     -> ERROR_NO_SYSTEM_RESOURCES
//  - plug before the adapter is up   -> ERROR_NOT_READY (see AdapterState:
//                                       STARTING means retry, FAILED does not)
//  - plug of an undescribable mode   -> ERROR_INVALID_PARAMETER (see the
//                                       accepted range above)
//  - plug racing an unplug           -> ERROR_OPERATION_ABORTED (retry)
//  - unknown cookie on unplug        -> ERROR_NOT_FOUND
//  - malformed parameters            -> ERROR_INVALID_PARAMETER
// UNPLUG does not check the adapter state, and unplugging everything
// (Cookie = 0) succeeds even when nothing is plugged.

#ifdef __cplusplus
}
#endif

#endif // NYANVDD_PROTOCOL_H
