// Display modes and EDID synthesis — the driver's only OS-independent logic.
//
// This header and Edid.cpp deliberately depend on nothing but the C++ standard
// library so they can be linked into tests/ and exercised without a WDK, a
// driver install, or a machine reboot. Everything that decides *what the OS
// sees* about a monitor lives here; Driver.cpp only translates it into IddCx
// structures.

#pragma once

#include <cstdint>

namespace nyan
{
    namespace vdd
    {
        struct NyanMode
        {
            uint32_t Width;
            uint32_t Height;
            uint32_t RefreshHz;
        };

        inline constexpr bool SameMode(const NyanMode& A, const NyanMode& B)
        {
            return A.Width == B.Width && A.Height == B.Height && A.RefreshHz == B.RefreshHz;
        }

        // Static mode table, geared to XR-glasses walls. A monitor also always
        // offers the mode it was plugged with (see BuildModeList), so this table
        // is a convenience set, not the limit of what can be realized.
        inline constexpr NyanMode kModeTable[] = {
            { 1920, 1080,  60 }, { 1920, 1080,  72 }, { 1920, 1080,  90 }, { 1920, 1080, 120 },
            { 2560, 1440,  60 }, { 2560, 1440,  90 }, { 2560, 1440, 120 },
            { 3840, 2160,  60 }, { 3840, 2160,  90 }, { 3840, 2160, 120 },
            { 1280,  720,  60 },
        };
        inline constexpr uint32_t kModeTableCount =
            static_cast<uint32_t>(sizeof(kModeTable) / sizeof(kModeTable[0]));
        inline constexpr uint32_t kMaxModes = kModeTableCount + 1;

        // Blanking used for every synthesized timing (reduced-blanking-ish).
        inline constexpr uint32_t kHBlank = 160;
        inline constexpr uint32_t kVBlank = 35;

        // Limits of what an EDID 1.4 base block can describe: a detailed timing
        // stores the active pixels in 12 bits, and the range-limits descriptor
        // stores rates in 8 bits with an optional +255 offset. A mode past any
        // of these cannot be fully described, so the driver must refuse it
        // rather than accept a plug it cannot honour.
        inline constexpr uint32_t kMinWidth = 640;
        inline constexpr uint32_t kMinHeight = 480;
        inline constexpr uint32_t kMaxActivePixels = 4095;
        inline constexpr uint32_t kMinRefreshHz = 24;
        inline constexpr uint32_t kMaxRefreshHz = 240;
        inline constexpr uint32_t kMaxLineRateHz = 510000;             // (255 + 255) kHz
        inline constexpr uint64_t kMaxPixelClockHz = 2550000000ull;    // 255 * 10 MHz

        // Whether a plug request can be both described in the EDID and realized.
        bool IsSupportedMode(const NyanMode& Mode);

        // The mode list reported for a monitor: the plugged mode first (it is
        // the preferred one), then the static table minus a duplicate.
        // Preferred == nullptr yields the static table alone.
        //
        // Both EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION (monitor modes) and
        // EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES (target modes) must report the
        // same set: the OS realizes only their intersection, so a mode present
        // in one list and absent from the other silently cannot be selected.
        uint32_t BuildModeList(const NyanMode* Preferred, NyanMode Out[kMaxModes]);

        uint64_t PixelClockHz(const NyanMode& Mode);
        uint32_t HorizontalHz(const NyanMode& Mode); // line rate

        // An EDID detailed timing descriptor stores the pixel clock in 16 bits
        // of 10 kHz units (max 655.35 MHz) and the active pixels in 12 bits.
        bool DtdEncodable(const NyanMode& Mode);

        // The timing actually written into the EDID's preferred descriptor.
        // When the requested mode does not fit, the refresh rate is stepped
        // down but the pixel count is preserved: Windows derives the monitor's
        // native resolution — and therefore its DPI and default scaling — from
        // this descriptor, so substituting a smaller resolution would misreport
        // the panel.
        NyanMode DtdModeFor(const NyanMode& Requested);

        // Tallest panel we are willing to describe. Windows derives the default
        // display scaling from the physical size, and a panel taller than a
        // desktop monitor is treated as a television and given 10-foot UI
        // scaling. Measured on Windows 11 24H2, all at 96 DPI:
        //   29 cm tall (1080p) -> 100 %      38 cm tall (1440p) -> 100 %
        //   48 cm tall (1800p) -> 250 %      57 cm tall (2160p) -> 300 %
        // so a 4K panel at a true 96 DPI would come up at 300 %, leaving a
        // 1280x720 desktop on a 4K wall.
        inline constexpr uint32_t kMaxPanelHeightMm = 380;

        // Physical size for a mode: 96 DPI (one logical pixel per physical
        // pixel, so Windows defaults to 100 % scaling) wherever that fits a
        // desktop-class panel, otherwise the largest desktop-class panel with
        // the same aspect ratio. The aspect ratio always matches the mode.
        void PhysicalSizeMm(const NyanMode& Mode, uint32_t* WidthMm, uint32_t* HeightMm);

        // Builds a 128-byte EDID 1.4 base block: vendor "NYN", product 0x3D0F,
        // serial = Cookie (also spelled out as "NW-XXXXXXXX"), product name
        // "nyan Wall", a preferred detailed timing, and display range limits
        // computed to cover every mode BuildModeList reports.
        void BuildEdid(uint32_t Cookie, const NyanMode& Preferred, uint8_t Out[128]);

        // Reads the cookie back out of an EDID base block. 0 if not ours.
        uint32_t CookieFromEdid(const void* Data, uint32_t Size);
    }
}
