// Display modes and EDID synthesis. See Edid.h for the contract; this file
// depends only on the C++ standard library so tests/ can link it directly.

#include "Edid.h"

#include <cstdio>
#include <cstring>

namespace
{
    using nyan::vdd::NyanMode;

    constexpr uint8_t kMfrBytes[2] = { 0x3B, 0x2E };  // "NYN" in EDID 5-bit packing
    constexpr uint16_t kProductCode = 0x3D0F;

    struct RangeLimits
    {
        uint32_t MinVHz;
        uint32_t MaxVHz;
        uint32_t MinHkHz;
        uint32_t MaxHkHz;
        uint32_t MaxPclk10MHz;
    };

    uint8_t Clamp8(uint32_t Value)
    {
        return static_cast<uint8_t>(Value > 255 ? 255 : Value);
    }

    // Range limits must enclose every timing the monitor reports, otherwise the
    // EDID contradicts the mode list and validators (and some consumers) reject
    // the out-of-range modes.
    RangeLimits ComputeRangeLimits(const NyanMode* Modes, uint32_t Count)
    {
        RangeLimits R = {};
        R.MinVHz = 0xFFFFFFFFu;
        R.MinHkHz = 0xFFFFFFFFu;

        for (uint32_t i = 0; i < Count; ++i)
        {
            const NyanMode& M = Modes[i];
            if (M.RefreshHz < R.MinVHz) R.MinVHz = M.RefreshHz;
            if (M.RefreshHz > R.MaxVHz) R.MaxVHz = M.RefreshHz;

            const uint32_t LineHz = nyan::vdd::HorizontalHz(M);
            const uint32_t FloorKHz = LineHz / 1000;
            const uint32_t CeilKHz = (LineHz + 999) / 1000;
            if (FloorKHz < R.MinHkHz) R.MinHkHz = FloorKHz;
            if (CeilKHz > R.MaxHkHz) R.MaxHkHz = CeilKHz;

            const uint32_t Pclk10MHz =
                static_cast<uint32_t>((nyan::vdd::PixelClockHz(M) + 9999999ull) / 10000000ull);
            if (Pclk10MHz > R.MaxPclk10MHz) R.MaxPclk10MHz = Pclk10MHz;
        }

        if (Count == 0)
        {
            R.MinVHz = R.MaxVHz = 60;
            R.MinHkHz = R.MaxHkHz = 67;
            R.MaxPclk10MHz = 15;
        }
        return R;
    }

    void WriteRangeLimits(uint8_t* D, const RangeLimits& R)
    {
        D[0] = 0; D[1] = 0; D[2] = 0; D[3] = 0xFD;

        uint32_t MinV = R.MinVHz, MaxV = R.MaxVHz;
        uint32_t MinH = R.MinHkHz, MaxH = R.MaxHkHz;
        uint8_t Flags = 0;

        // EDID 1.4 offset flags: bits 3:2 horizontal, bits 1:0 vertical.
        // 0b10 = "max has 255 added", 0b11 = "min and max both have 255 added".
        // Needed because 4K@120 runs at 263 kHz, past the 8-bit field.
        if (MaxV > 255)
        {
            Flags |= (MinV > 255) ? 0x03 : 0x02;
            MaxV -= 255;
            if (MinV > 255) MinV -= 255;
        }
        if (MaxH > 255)
        {
            Flags |= (MinH > 255) ? 0x0C : 0x08;
            MaxH -= 255;
            if (MinH > 255) MinH -= 255;
        }

        D[4] = Flags;
        D[5] = Clamp8(MinV);
        D[6] = Clamp8(MaxV);
        D[7] = Clamp8(MinH);
        D[8] = Clamp8(MaxH);
        D[9] = Clamp8(R.MaxPclk10MHz);
        D[10] = 0x01; // range limits only, no timing formula
        D[11] = 0x0A;
        for (int i = 12; i < 18; ++i) D[i] = 0x20;
    }

    void WriteDtd(uint8_t* D, const NyanMode& M, uint32_t ImageWidthMm, uint32_t ImageHeightMm)
    {
        const uint32_t Clk10kHz = static_cast<uint32_t>(nyan::vdd::PixelClockHz(M) / 10000);
        const uint32_t HBlank = nyan::vdd::kHBlank;
        const uint32_t VBlank = nyan::vdd::kVBlank;
        constexpr uint32_t HSyncOffset = 48, HSyncWidth = 32;
        constexpr uint32_t VSyncOffset = 3, VSyncWidth = 5;

        D[0] = Clk10kHz & 0xFF;
        D[1] = (Clk10kHz >> 8) & 0xFF;
        D[2] = M.Width & 0xFF;
        D[3] = HBlank & 0xFF;
        D[4] = static_cast<uint8_t>(((M.Width >> 8) << 4) | (HBlank >> 8));
        D[5] = M.Height & 0xFF;
        D[6] = VBlank & 0xFF;
        D[7] = static_cast<uint8_t>(((M.Height >> 8) << 4) | (VBlank >> 8));
        D[8] = HSyncOffset & 0xFF;
        D[9] = HSyncWidth & 0xFF;
        D[10] = static_cast<uint8_t>(((VSyncOffset & 0xF) << 4) | (VSyncWidth & 0xF));
        D[11] = static_cast<uint8_t>(((HSyncOffset >> 8) << 6) | ((HSyncWidth >> 8) << 4) |
                                     ((VSyncOffset >> 4) << 2) | (VSyncWidth >> 4));
        D[12] = ImageWidthMm & 0xFF;
        D[13] = ImageHeightMm & 0xFF;
        D[14] = static_cast<uint8_t>(((ImageWidthMm >> 8) << 4) | ((ImageHeightMm >> 8) & 0x0F));
        D[15] = 0; // h border
        D[16] = 0; // v border
        D[17] = 0x1E; // digital, separate sync, +hsync +vsync
    }

    // 18-byte display descriptor: tag plus 13 bytes of ASCII, 0x0A-terminated
    // and space-padded per spec.
    void WriteTextDescriptor(uint8_t* D, uint8_t Tag, const char* Text)
    {
        D[0] = 0; D[1] = 0; D[2] = 0; D[3] = Tag; D[4] = 0;
        int i = 0;
        for (; i < 13 && Text[i]; ++i) D[5 + i] = static_cast<uint8_t>(Text[i]);
        if (i < 13) D[5 + i++] = 0x0A;
        for (; i < 13; ++i) D[5 + i] = 0x20;
    }
}

namespace nyan
{
    namespace vdd
    {
        uint64_t PixelClockHz(const NyanMode& Mode)
        {
            return static_cast<uint64_t>(Mode.Width + kHBlank) * (Mode.Height + kVBlank) * Mode.RefreshHz;
        }

        uint32_t HorizontalHz(const NyanMode& Mode)
        {
            return (Mode.Height + kVBlank) * Mode.RefreshHz;
        }

        bool IsSupportedMode(const NyanMode& Mode)
        {
            if (Mode.Width < kMinWidth || Mode.Height < kMinHeight) return false;
            if (Mode.Width > kMaxActivePixels || Mode.Height > kMaxActivePixels) return false;
            if (Mode.RefreshHz < kMinRefreshHz || Mode.RefreshHz > kMaxRefreshHz) return false;
            if (HorizontalHz(Mode) > kMaxLineRateHz) return false;
            if (PixelClockHz(Mode) > kMaxPixelClockHz) return false;
            return true;
        }

        bool DtdEncodable(const NyanMode& Mode)
        {
            return Mode.Width <= 4095 && Mode.Height <= 4095 && PixelClockHz(Mode) <= 655350000ull;
        }

        NyanMode DtdModeFor(const NyanMode& Requested)
        {
            if (DtdEncodable(Requested))
            {
                return Requested;
            }

            // Keep the pixel count and give up refresh rate instead: the
            // descriptor's resolution is what Windows treats as native.
            static const uint32_t kStepDown[] = { 120, 100, 90, 75, 72, 60, 50, 30, 24 };
            for (uint32_t Hz : kStepDown)
            {
                if (Hz >= Requested.RefreshHz) continue;
                const NyanMode Candidate = { Requested.Width, Requested.Height, Hz };
                if (DtdEncodable(Candidate)) return Candidate;
            }

            // Pixel count itself is not expressible (over 4095 in a dimension).
            return NyanMode{ 1920, 1080, 60 };
        }

        void PhysicalSizeMm(const NyanMode& Mode, uint32_t* WidthMm, uint32_t* HeightMm)
        {
            // Start at 96 DPI: mm = px * 25.4 / 96, rounded to nearest.
            uint32_t W = (Mode.Width * 254u + 480u) / 960u;
            uint32_t H = (Mode.Height * 254u + 480u) / 960u;

            // Never claim a panel taller than a desktop monitor (see
            // kMaxPanelHeightMm): Windows would switch to television scaling
            // and hand a 4K wall a 1280x720 desktop. Shrinking keeps the
            // aspect ratio and costs only some effective DPI.
            if (H > kMaxPanelHeightMm)
            {
                W = (W * kMaxPanelHeightMm + H / 2) / H;
                H = kMaxPanelHeightMm;
            }

            *WidthMm = W;
            *HeightMm = H;
        }

        uint32_t BuildModeList(const NyanMode* Preferred, NyanMode Out[kMaxModes])
        {
            uint32_t Count = 0;
            if (Preferred)
            {
                Out[Count++] = *Preferred;
            }
            for (uint32_t i = 0; i < kModeTableCount; ++i)
            {
                if (Preferred && SameMode(kModeTable[i], *Preferred)) continue;
                Out[Count++] = kModeTable[i];
            }
            return Count;
        }

        void BuildEdid(uint32_t Cookie, const NyanMode& Preferred, uint8_t Out[128])
        {
            uint8_t* E = Out;
            memset(E, 0, 128);

            static const uint8_t Header[8] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
            memcpy(E, Header, 8);

            E[8] = kMfrBytes[0]; E[9] = kMfrBytes[1];
            E[10] = kProductCode & 0xFF;
            E[11] = (kProductCode >> 8) & 0xFF;
            E[12] = Cookie & 0xFF;
            E[13] = (Cookie >> 8) & 0xFF;
            E[14] = (Cookie >> 16) & 0xFF;
            E[15] = (Cookie >> 24) & 0xFF;
            E[16] = 1;    // week
            E[17] = 36;   // 1990 + 36 = 2026
            E[18] = 1; E[19] = 4; // EDID 1.4

            // The panel size follows the plugged mode so that the desktop comes
            // up at 100 % scaling; the descriptor timing may drop refresh but
            // never changes the panel.
            uint32_t WidthMm = 0, HeightMm = 0;
            PhysicalSizeMm(Preferred, &WidthMm, &HeightMm);

            E[20] = 0xA5; // digital, 8bpc, DisplayPort
            E[21] = Clamp8((WidthMm + 5) / 10);  // cm
            E[22] = Clamp8((HeightMm + 5) / 10); // cm
            E[23] = 120;  // gamma 2.2
            E[24] = 0x06; // sRGB default + preferred timing is native

            static const uint8_t Chroma[10] = { 0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54 };
            memcpy(E + 25, Chroma, 10);

            // No established timings; standard timings all unused
            for (int i = 38; i < 54; ++i) E[i] = 0x01;

            WriteDtd(E + 54, DtdModeFor(Preferred), WidthMm, HeightMm);
            WriteTextDescriptor(E + 72, 0xFC, "nyan Wall");

            char Serial[16];
            snprintf(Serial, sizeof(Serial), "NW-%08X", Cookie);
            WriteTextDescriptor(E + 90, 0xFF, Serial);

            NyanMode Modes[kMaxModes];
            const uint32_t Count = BuildModeList(&Preferred, Modes);
            WriteRangeLimits(E + 108, ComputeRangeLimits(Modes, Count));

            E[126] = 0; // no extension blocks

            uint8_t Sum = 0;
            for (int i = 0; i < 127; ++i) Sum = static_cast<uint8_t>(Sum + E[i]);
            E[127] = static_cast<uint8_t>(0x100 - Sum);
        }

        uint32_t CookieFromEdid(const void* Data, uint32_t Size)
        {
            if (Size != 128 || Data == nullptr) return 0;
            const uint8_t* E = static_cast<const uint8_t*>(Data);
            if (E[8] != kMfrBytes[0] || E[9] != kMfrBytes[1]) return 0;
            if (E[10] != (kProductCode & 0xFF) || E[11] != ((kProductCode >> 8) & 0xFF)) return 0;
            return static_cast<uint32_t>(E[12]) |
                   (static_cast<uint32_t>(E[13]) << 8) |
                   (static_cast<uint32_t>(E[14]) << 16) |
                   (static_cast<uint32_t>(E[15]) << 24);
        }
    }
}
