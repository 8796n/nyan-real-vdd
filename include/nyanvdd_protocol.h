// nyan Real VDD — public control protocol
//
// This header is the single source of truth for the contract between the
// nyanvdd driver and its clients (Spatial Wall, nyanvddctl, third parties).
// It is self-contained C: copy it verbatim into consuming projects.
//
// Requires <windows.h> and <winioctl.h> (for CTL_CODE) to be included first
// in user mode. The driver includes it after the WDK headers.
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

#ifdef __cplusplus
extern "C" {
#endif

#define NYANVDD_PROTOCOL_VERSION 1

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

#define NYANVDD_MAX_MONITORS 4

#define NYANVDD_DEVICE_TYPE 0x8E56 // custom range (bit 15 set)
#define NYANVDD_IOCTL(Fn) \
    CTL_CODE(NYANVDD_DEVICE_TYPE, 0x800 + (Fn), METHOD_BUFFERED, FILE_ANY_ACCESS)

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

typedef struct NYANVDD_STATUS_OUT {
    UINT32 ProtocolVersion; // NYANVDD_PROTOCOL_VERSION of the driver build
    UINT32 DriverVersion;   // packed: (major << 16) | minor
    UINT32 IddCxOsVersion;  // IDDCX_VERSION reported by the OS (e.g. 0x1A00), 0 if unknown
    UINT32 MaxMonitors;     // NYANVDD_MAX_MONITORS of the driver build
    UINT32 CapFlags;        // NYANVDD_CAP_*
    UINT32 MonitorCount;    // currently plugged monitors
    UINT32 WatchdogTimeoutMs; // 0 = watchdog disarmed
    UINT32 Reserved;
} NYANVDD_STATUS_OUT;

// Plug flags.
#define NYANVDD_PLUG_FLAG_HDR10 0x00000001u // report 10-bit capability for this monitor (needs CAP_HDR10_READY)

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
//  - plug/unplug before adapter up   -> ERROR_NOT_READY
//  - unknown cookie on unplug        -> ERROR_NOT_FOUND
//  - malformed parameters            -> ERROR_INVALID_PARAMETER

#ifdef __cplusplus
}
#endif

#endif // NYANVDD_PROTOCOL_H
