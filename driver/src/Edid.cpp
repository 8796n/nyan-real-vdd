// EDID synthesis for nyan Real VDD monitors.
//
// Identity layout (see nyanvdd_protocol.h): manufacturer "NYN", product code
// 0x3D0F, 32-bit serial = plug cookie. The cookie also appears as the display
// serial-string descriptor ("NW-XXXXXXXX") so humans and tools can correlate
// an OS monitor with the client that plugged it. Windows shows the product
// name descriptor ("nyan Wall") as the monitor's friendly name.
//
// The preferred detailed timing is synthesized with reduced blanking. The DTD
// pixel-clock field is 16-bit in 10 kHz units (max 655.35 MHz); modes beyond
// that (e.g. 4K@120) fall back to a 1080p60 DTD — harmless, because the OS
// mode list comes from ParseMonitorDescription, not from the EDID itself.

#include "Driver.h"

namespace
{
    constexpr BYTE kMfrBytes[2] = { 0x3B, 0x2E };  // "NYN" in EDID 5-bit packing
    constexpr BYTE kProductLo = NYANVDD_EDID_PRODUCT_CODE & 0xFF;
    constexpr BYTE kProductHi = (NYANVDD_EDID_PRODUCT_CODE >> 8) & 0xFF;

    // Reduced-blanking-ish constants for the synthesized DTD.
    constexpr UINT32 kHBlank = 160, kHSyncOffset = 48, kHSyncWidth = 32;
    constexpr UINT32 kVBlank = 35, kVSyncOffset = 3, kVSyncWidth = 5;
    constexpr UINT32 kImageWidthMm = 600, kImageHeightMm = 340;

    void WriteDtd(BYTE* D, UINT32 Width, UINT32 Height, UINT32 RefreshHz)
    {
        UINT64 PixelClock = (UINT64)(Width + kHBlank) * (Height + kVBlank) * RefreshHz;
        if (PixelClock > 655350000ull || Width > 4095 || Height > 4095)
        {
            // Not DTD-encodable; stamp a canonical 1080p60 instead.
            Width = 1920; Height = 1080; RefreshHz = 60;
            PixelClock = (UINT64)(Width + kHBlank) * (Height + kVBlank) * RefreshHz;
        }

        const UINT32 Clk10kHz = (UINT32)(PixelClock / 10000);
        D[0] = Clk10kHz & 0xFF;
        D[1] = (Clk10kHz >> 8) & 0xFF;
        D[2] = Width & 0xFF;
        D[3] = kHBlank & 0xFF;
        D[4] = (BYTE)(((Width >> 8) << 4) | (kHBlank >> 8));
        D[5] = Height & 0xFF;
        D[6] = kVBlank & 0xFF;
        D[7] = (BYTE)(((Height >> 8) << 4) | (kVBlank >> 8));
        D[8] = kHSyncOffset & 0xFF;
        D[9] = kHSyncWidth & 0xFF;
        D[10] = (BYTE)(((kVSyncOffset & 0xF) << 4) | (kVSyncWidth & 0xF));
        D[11] = (BYTE)(((kHSyncOffset >> 8) << 6) | ((kHSyncWidth >> 8) << 4) |
                       ((kVSyncOffset >> 4) << 2) | (kVSyncWidth >> 4));
        D[12] = kImageWidthMm & 0xFF;
        D[13] = kImageHeightMm & 0xFF;
        D[14] = (BYTE)(((kImageWidthMm >> 8) << 4) | (kImageHeightMm >> 8));
        D[15] = 0; // h border
        D[16] = 0; // v border
        D[17] = 0x1E; // digital, separate sync, +hsync +vsync
    }

    // 18-byte display descriptor with the given tag and 13 bytes of ASCII
    // data, 0x0A-terminated and space-padded per spec.
    void WriteTextDescriptor(BYTE* D, BYTE Tag, const char* Text)
    {
        D[0] = 0; D[1] = 0; D[2] = 0; D[3] = Tag; D[4] = 0;
        int i = 0;
        for (; i < 13 && Text[i]; ++i) D[5 + i] = (BYTE)Text[i];
        if (i < 13) D[5 + i++] = 0x0A;
        for (; i < 13; ++i) D[5 + i] = 0x20;
    }
}

namespace nyan
{
    namespace vdd
    {
        void BuildEdid(UINT32 Cookie, UINT32 Width, UINT32 Height, UINT32 RefreshHz, BYTE Out[128])
        {
            BYTE* E = Out;
            memset(E, 0, 128);

            // Header
            static const BYTE Header[8] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
            memcpy(E, Header, 8);

            E[8] = kMfrBytes[0]; E[9] = kMfrBytes[1];
            E[10] = kProductLo; E[11] = kProductHi;
            E[12] = Cookie & 0xFF;
            E[13] = (Cookie >> 8) & 0xFF;
            E[14] = (Cookie >> 16) & 0xFF;
            E[15] = (Cookie >> 24) & 0xFF;
            E[16] = 1;    // week
            E[17] = 36;   // 1990 + 36 = 2026
            E[18] = 1; E[19] = 4; // EDID 1.4

            E[20] = 0xA5; // digital, 8bpc, DisplayPort
            E[21] = kImageWidthMm / 10;  // cm
            E[22] = kImageHeightMm / 10; // cm
            E[23] = 120;  // gamma 2.2
            E[24] = 0x06; // sRGB default + preferred timing is native

            // sRGB-ish chromaticity (borrowed from a common desktop panel)
            static const BYTE Chroma[10] = { 0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54 };
            memcpy(E + 25, Chroma, 10);

            // No established timings; standard timings all unused
            for (int i = 38; i < 54; ++i) E[i] = 0x01;

            WriteDtd(E + 54, Width, Height, RefreshHz);
            WriteTextDescriptor(E + 72, 0xFC, "nyan Wall");

            char Serial[16];
            sprintf_s(Serial, "NW-%08X", Cookie);
            WriteTextDescriptor(E + 90, 0xFF, Serial);

            // Range limits: 24-120 Hz, 30-140 kHz, max pixel clock 1190 MHz,
            // "range limits only" (no timing formula)
            static const BYTE Range[18] = {
                0x00,0x00,0x00,0xFD,0x00,
                0x18,0x78,0x1E,0x8C,0x77,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20
            };
            memcpy(E + 108, Range, 18);

            E[126] = 0; // no extension blocks

            BYTE Sum = 0;
            for (int i = 0; i < 127; ++i) Sum += E[i];
            E[127] = (BYTE)(0x100 - Sum);
        }

        UINT32 CookieFromEdid(const void* Data, UINT32 Size)
        {
            if (Size != 128 || Data == nullptr) return 0;
            const BYTE* E = (const BYTE*)Data;
            if (E[8] != kMfrBytes[0] || E[9] != kMfrBytes[1]) return 0;
            if (E[10] != kProductLo || E[11] != kProductHi) return 0;
            return (UINT32)E[12] | ((UINT32)E[13] << 8) | ((UINT32)E[14] << 16) | ((UINT32)E[15] << 24);
        }
    }
}
