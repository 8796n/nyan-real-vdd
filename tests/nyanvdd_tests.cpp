// Unit tests for the driver's OS-independent logic (driver/src/Edid.cpp).
//
// Plain console executable: no WDK, no driver install, no elevation, no test
// framework. Run it from scripts/build.ps1 or directly; non-zero exit means a
// failure. Everything here is reachable because Edid.cpp deliberately depends
// on nothing but the standard library.

#include "../driver/src/Edid.h"

#include <windows.h>
#include "../include/nyanvdd_protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace nyan::vdd;

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        ++g_Checks;                                                 \
        if (!(cond)) {                                              \
            ++g_Failures;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

    // --- EDID field accessors, written independently of the builder so the
    // --- tests actually decode the bytes rather than trusting the producer.

    const uint8_t* Dtd(const uint8_t* E) { return E + 54; }

    uint32_t DtdWidth(const uint8_t* E) { return Dtd(E)[2] | ((Dtd(E)[4] >> 4) << 8); }
    uint32_t DtdHeight(const uint8_t* E) { return Dtd(E)[5] | ((Dtd(E)[7] >> 4) << 8); }
    uint32_t DtdPixelClockHz(const uint8_t* E) { return (Dtd(E)[0] | (Dtd(E)[1] << 8)) * 10000u; }
    uint32_t DtdWidthMm(const uint8_t* E) { return Dtd(E)[12] | ((Dtd(E)[14] >> 4) << 8); }
    uint32_t DtdHeightMm(const uint8_t* E) { return Dtd(E)[13] | ((Dtd(E)[14] & 0x0F) << 8); }

    struct DecodedRange
    {
        uint32_t MinVHz, MaxVHz, MinHkHz, MaxHkHz, MaxPclkHz;
    };

    DecodedRange DecodeRange(const uint8_t* E)
    {
        const uint8_t* D = E + 108;
        const uint8_t V = D[4] & 0x03;
        const uint8_t H = (D[4] >> 2) & 0x03;
        DecodedRange R = {};
        R.MinVHz = D[5] + ((V == 0x03) ? 255u : 0u);
        R.MaxVHz = D[6] + ((V == 0x02 || V == 0x03) ? 255u : 0u);
        R.MinHkHz = D[7] + ((H == 0x03) ? 255u : 0u);
        R.MaxHkHz = D[8] + ((H == 0x02 || H == 0x03) ? 255u : 0u);
        R.MaxPclkHz = D[9] * 10000000u;
        return R;
    }

    bool FindDescriptor(const uint8_t* E, uint8_t Tag, char* Text13)
    {
        static const int kDescriptorOffsets[] = { 54, 72, 90, 108 };
        for (int Offset : kDescriptorOffsets)
        {
            const uint8_t* D = E + Offset;
            if (D[0] == 0 && D[1] == 0 && D[2] == 0 && D[3] == Tag)
            {
                memcpy(Text13, D + 5, 13);
                Text13[13] = '\0';
                for (int i = 0; i < 13; ++i)
                {
                    if (Text13[i] == 0x0A) { Text13[i] = '\0'; break; }
                }
                return true;
            }
        }
        return false;
    }

    // Supported modes the static table does not cover.
    const NyanMode kOffTableModes[] = {
        { 1920, 1200,  60 },  // the resolution that silently became 1080p
        { 2560, 1600,  75 },
        { 1366,  768,  60 },
        { 3440, 1440, 100 },
        { 3840, 2160, 200 },  // beyond DTD encoding, still describable
        { 1024,  768,  24 },
    };

    // Modes the driver must refuse: each exceeds something an EDID 1.4 base
    // block can express, so accepting them would promise a monitor that
    // cannot be delivered.
    const NyanMode kUnsupportedModes[] = {
        { 3840, 2160, 240 },  // 527 kHz line rate, past the 510 kHz ceiling
        { 7680, 4320,  60 },  // active pixels past the 12-bit descriptor field
        { 1920, 1080, 241 },  // above the documented refresh ceiling
        {  640,  479,  60 },  // below the documented minimum
        {  639,  480,  60 },
        { 1920, 1080,  23 },
    };

    // --- tests ---

    // The plug contract: everything accepted must be fully describable, and
    // everything describable that the table offers must be accepted.
    void TestSupportedModePredicate()
    {
        for (uint32_t i = 0; i < kModeTableCount; ++i)
        {
            CHECK(IsSupportedMode(kModeTable[i]),
                  "static table mode %ux%u@%u is not accepted by IsSupportedMode",
                  kModeTable[i].Width, kModeTable[i].Height, kModeTable[i].RefreshHz);
        }
        for (const NyanMode& M : kOffTableModes)
        {
            CHECK(IsSupportedMode(M), "%ux%u@%u should be supported",
                  M.Width, M.Height, M.RefreshHz);
        }
        for (const NyanMode& M : kUnsupportedModes)
        {
            CHECK(!IsSupportedMode(M), "%ux%u@%u should be refused",
                  M.Width, M.Height, M.RefreshHz);
        }

        // Every supported mode must survive the descriptor with its pixel
        // count intact, so the "keep pixels, drop refresh" fallback never has
        // to give up entirely.
        CHECK(IsSupportedMode(NyanMode{ 4095, 4095, 24 }), "largest describable mode should be supported");
        const NyanMode Extreme = DtdModeFor(NyanMode{ 4095, 4095, 24 });
        CHECK(Extreme.Width == 4095 && Extreme.Height == 4095,
              "largest describable mode lost its pixel count in the descriptor");
    }

    void TestEdidStructure()
    {
        uint8_t E[128];
        BuildEdid(0xC0FFEE01, NyanMode{ 1920, 1080, 120 }, E);

        static const uint8_t Header[8] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
        CHECK(memcmp(E, Header, 8) == 0, "EDID header mismatch");

        uint8_t Sum = 0;
        for (int i = 0; i < 128; ++i) Sum = static_cast<uint8_t>(Sum + E[i]);
        CHECK(Sum == 0, "checksum does not zero the block (got %u)", Sum);

        CHECK(E[18] == 1 && E[19] == 4, "not EDID 1.4 (%u.%u)", E[18], E[19]);
        CHECK(E[126] == 0, "unexpected extension block count %u", E[126]);

        // Manufacturer id decodes to "NYN" (5 bits per letter, 'A' == 1).
        const uint16_t Mfr = static_cast<uint16_t>((E[8] << 8) | E[9]);
        const char Decoded[4] = {
            static_cast<char>('A' + ((Mfr >> 10) & 0x1F) - 1),
            static_cast<char>('A' + ((Mfr >> 5) & 0x1F) - 1),
            static_cast<char>('A' + (Mfr & 0x1F) - 1),
            '\0'
        };
        CHECK(strcmp(Decoded, "NYN") == 0, "manufacturer id decodes to \"%s\", expected \"NYN\"", Decoded);

        char Text[14];
        CHECK(FindDescriptor(E, 0xFC, Text) && strcmp(Text, "nyan Wall") == 0,
              "product name descriptor missing or wrong");
        CHECK(FindDescriptor(E, 0xFF, Text) && strcmp(Text, "NW-C0FFEE01") == 0,
              "serial descriptor is \"%s\", expected \"NW-C0FFEE01\"", Text);
        CHECK(FindDescriptor(E, 0xFD, Text), "range limits descriptor missing");
        CHECK(E[108 + 10] == 0x01, "range limits should declare no timing formula");
    }

    void TestCookieRoundTrip()
    {
        const uint32_t kCookies[] = { 1u, 0xC0FFEE01u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x00010000u };
        for (uint32_t Cookie : kCookies)
        {
            uint8_t E[128];
            BuildEdid(Cookie, NyanMode{ 1920, 1080, 60 }, E);
            CHECK(CookieFromEdid(E, 128) == Cookie,
                  "cookie round-trip failed for 0x%08X (got 0x%08X)", Cookie, CookieFromEdid(E, 128));

            char Text[14], Expected[16];
            snprintf(Expected, sizeof(Expected), "NW-%08X", Cookie);
            CHECK(FindDescriptor(E, 0xFF, Text) && strcmp(Text, Expected) == 0,
                  "serial string for 0x%08X is \"%s\", expected \"%s\"", Cookie, Text, Expected);
        }

        uint8_t E[128];
        BuildEdid(0x1234u, NyanMode{ 1920, 1080, 60 }, E);
        CHECK(CookieFromEdid(E, 127) == 0, "wrong size should not resolve");
        CHECK(CookieFromEdid(nullptr, 128) == 0, "null data should not resolve");

        uint8_t Foreign[128];
        memcpy(Foreign, E, 128);
        Foreign[8] ^= 0xFF; // different manufacturer
        CHECK(CookieFromEdid(Foreign, 128) == 0, "foreign EDID must not resolve to a cookie");
    }

    // Regression for the defect where a plugged resolution outside the static
    // table was reported as a monitor mode but not as a target mode, so the OS
    // could not realize it: plugging 1920x1200 produced a 1920x1080 display
    // while PLUG and LIST both reported success at 1920x1200.
    void TestModeListCarriesPluggedMode()
    {
        for (const NyanMode& M : kOffTableModes)
        {
            NyanMode Modes[kMaxModes];
            const uint32_t Count = BuildModeList(&M, Modes);

            CHECK(Count == kModeTableCount + 1,
                  "%ux%u@%u: expected %u modes, got %u",
                  M.Width, M.Height, M.RefreshHz, kModeTableCount + 1, Count);
            CHECK(SameMode(Modes[0], M),
                  "%ux%u@%u must be the preferred (index 0) mode",
                  M.Width, M.Height, M.RefreshHz);

            bool Found = false;
            for (uint32_t i = 0; i < Count; ++i) Found = Found || SameMode(Modes[i], M);
            CHECK(Found, "%ux%u@%u missing from the reported mode list",
                  M.Width, M.Height, M.RefreshHz);
        }

        // A plugged mode that is already in the table must not be duplicated.
        for (uint32_t t = 0; t < kModeTableCount; ++t)
        {
            NyanMode Modes[kMaxModes];
            const uint32_t Count = BuildModeList(&kModeTable[t], Modes);
            CHECK(Count == kModeTableCount,
                  "table mode %u duplicated (count %u)", t, Count);
            CHECK(SameMode(Modes[0], kModeTable[t]), "table mode %u not preferred", t);

            uint32_t Occurrences = 0;
            for (uint32_t i = 0; i < Count; ++i)
            {
                if (SameMode(Modes[i], kModeTable[t])) ++Occurrences;
            }
            CHECK(Occurrences == 1, "table mode %u appears %u times", t, Occurrences);
        }

        NyanMode Modes[kMaxModes];
        CHECK(BuildModeList(nullptr, Modes) == kModeTableCount,
              "no preferred mode should yield exactly the static table");
    }

    // Regression for the defect where every mode reported the same fixed
    // 600x340 mm panel, so the described aspect ratio never matched the mode
    // and the effective DPI drifted with the resolution.
    //
    // The panel must stay in Windows' desktop scaling class: measured on
    // 24H2, a 96 DPI 4K panel (57 cm tall) defaults to 300 % scaling, which
    // would leave a 1280x720 desktop on a 4K wall.
    void TestPhysicalSizeIsDesktopClass()
    {
        NyanMode All[kModeTableCount + sizeof(kOffTableModes) / sizeof(kOffTableModes[0])];
        uint32_t Count = 0;
        for (uint32_t i = 0; i < kModeTableCount; ++i) All[Count++] = kModeTable[i];
        for (const NyanMode& M : kOffTableModes) All[Count++] = M;

        for (uint32_t i = 0; i < Count; ++i)
        {
            const NyanMode& M = All[i];
            uint32_t WidthMm = 0, HeightMm = 0;
            PhysicalSizeMm(M, &WidthMm, &HeightMm);

            CHECK(HeightMm <= kMaxPanelHeightMm,
                  "%ux%u: %u mm tall panel leaves the desktop scaling class",
                  M.Width, M.Height, HeightMm);

            // dpi = px / (mm / 25.4); scaled by 100 to stay in integers.
            const uint32_t DpiX100 = (M.Width * 2540u) / WidthMm;
            const uint32_t DpiY100 = (M.Height * 2540u) / HeightMm;

            // Never describe a panel larger than 96 DPI would give: that is
            // the point at which Windows starts inflating the UI.
            CHECK(DpiX100 >= 9500, "%ux%u: horizontal DPI %u.%02u is below 96",
                  M.Width, M.Height, DpiX100 / 100, DpiX100 % 100);
            CHECK(DpiY100 >= 9500, "%ux%u: vertical DPI %u.%02u is below 96",
                  M.Width, M.Height, DpiY100 / 100, DpiY100 % 100);

            // Where a 96 DPI panel fits the desktop class, use exactly that.
            const bool FitsAt96 = ((M.Height * 254u + 480u) / 960u) <= kMaxPanelHeightMm;
            if (FitsAt96)
            {
                CHECK(DpiX100 <= 9700 && DpiY100 <= 9700,
                      "%ux%u: fits at 96 DPI but reports %u.%02u x %u.%02u DPI",
                      M.Width, M.Height, DpiX100 / 100, DpiX100 % 100, DpiY100 / 100, DpiY100 % 100);
            }

            // The described panel must have the mode's aspect ratio, within
            // the rounding of whole millimetres (the old fixed 600x340 mm
            // panel claimed 1.76:1 for every mode).
            const uint32_t ModeAspect1000 = (M.Width * 1000u) / M.Height;
            const uint32_t PanelAspect1000 = (WidthMm * 1000u) / HeightMm;
            const uint32_t AspectDelta = ModeAspect1000 > PanelAspect1000
                ? ModeAspect1000 - PanelAspect1000 : PanelAspect1000 - ModeAspect1000;
            CHECK(AspectDelta <= 15,
                  "%ux%u: panel %ux%u mm has aspect %u.%03u, mode is %u.%03u",
                  M.Width, M.Height, WidthMm, HeightMm,
                  PanelAspect1000 / 1000, PanelAspect1000 % 1000,
                  ModeAspect1000 / 1000, ModeAspect1000 % 1000);

            uint8_t E[128];
            BuildEdid(0xABCDEF01, M, E);

            CHECK(DtdWidthMm(E) == WidthMm && DtdHeightMm(E) == HeightMm,
                  "%ux%u: DTD image size %ux%u mm != panel %ux%u mm",
                  M.Width, M.Height, DtdWidthMm(E), DtdHeightMm(E), WidthMm, HeightMm);

            // Bytes 21/22 carry the same panel in centimetres.
            CHECK(E[21] == (WidthMm + 5) / 10 && E[22] == (HeightMm + 5) / 10,
                  "%ux%u: screen size bytes %ux%u cm disagree with %ux%u mm",
                  M.Width, M.Height, E[21], E[22], WidthMm, HeightMm);
            CHECK(E[21] != 0 && E[22] != 0, "%ux%u: screen size must not be undefined",
                  M.Width, M.Height);
        }

        // Cases measured on hardware.
        uint32_t WidthMm = 0, HeightMm = 0;
        PhysicalSizeMm(NyanMode{ 1920, 1080, 60 }, &WidthMm, &HeightMm);
        CHECK(WidthMm == 508 && HeightMm == 286,
              "1080p panel should be 508x286 mm (96 DPI), got %ux%u", WidthMm, HeightMm);

        PhysicalSizeMm(NyanMode{ 3840, 2160, 60 }, &WidthMm, &HeightMm);
        CHECK(HeightMm == kMaxPanelHeightMm,
              "4K panel should be clamped to %u mm tall, got %u", kMaxPanelHeightMm, HeightMm);
        CHECK(WidthMm == 675, "4K panel should be 675 mm wide, got %u", WidthMm);
    }

    // The descriptor timing is what Windows treats as the native resolution,
    // so a mode that does not fit must lose refresh rate, never pixels.
    void TestDtdPreservesPixelCount()
    {
        NyanMode All[kModeTableCount + sizeof(kOffTableModes) / sizeof(kOffTableModes[0])];
        uint32_t Count = 0;
        for (uint32_t i = 0; i < kModeTableCount; ++i) All[Count++] = kModeTable[i];
        for (const NyanMode& M : kOffTableModes) All[Count++] = M;

        for (uint32_t i = 0; i < Count; ++i)
        {
            const NyanMode& M = All[i];
            const NyanMode D = DtdModeFor(M);

            CHECK(D.Width == M.Width && D.Height == M.Height,
                  "%ux%u@%u: descriptor resolution changed to %ux%u",
                  M.Width, M.Height, M.RefreshHz, D.Width, D.Height);
            CHECK(D.RefreshHz <= M.RefreshHz,
                  "%ux%u@%u: descriptor refresh rose to %u",
                  M.Width, M.Height, M.RefreshHz, D.RefreshHz);
            CHECK(DtdEncodable(D), "%ux%u@%u: chosen descriptor timing still does not fit",
                  M.Width, M.Height, M.RefreshHz);

            uint8_t E[128];
            BuildEdid(0x5A5A5A5A, M, E);
            CHECK(DtdWidth(E) == M.Width && DtdHeight(E) == M.Height,
                  "%ux%u@%u: EDID descriptor encodes %ux%u",
                  M.Width, M.Height, M.RefreshHz, DtdWidth(E), DtdHeight(E));

            const uint32_t Pclk = DtdPixelClockHz(E);
            CHECK(Pclk != 0, "%ux%u@%u: descriptor pixel clock is zero",
                  M.Width, M.Height, M.RefreshHz);
            CHECK(Pclk <= 655350000u, "%ux%u@%u: descriptor pixel clock %u overflows the field",
                  M.Width, M.Height, M.RefreshHz, Pclk);
        }

        // 4K@120 cannot be expressed at 120 Hz; it must stay 4K.
        const NyanMode Fallback = DtdModeFor(NyanMode{ 3840, 2160, 120 });
        CHECK(Fallback.Width == 3840 && Fallback.Height == 2160,
              "4K@120 descriptor fell back to %ux%u instead of keeping 4K",
              Fallback.Width, Fallback.Height);
        CHECK(Fallback.RefreshHz < 120, "4K@120 descriptor should have stepped the refresh down");
    }

    // The range limits descriptor has to enclose every mode the driver reports,
    // otherwise the EDID contradicts its own mode list.
    void TestRangeLimitsCoverReportedModes()
    {
        NyanMode All[kModeTableCount + sizeof(kOffTableModes) / sizeof(kOffTableModes[0])];
        uint32_t Count = 0;
        for (uint32_t i = 0; i < kModeTableCount; ++i) All[Count++] = kModeTable[i];
        for (const NyanMode& M : kOffTableModes) All[Count++] = M;

        for (uint32_t i = 0; i < Count; ++i)
        {
            const NyanMode& Preferred = All[i];
            uint8_t E[128];
            BuildEdid(0x11223344, Preferred, E);
            const DecodedRange R = DecodeRange(E);

            NyanMode Modes[kMaxModes];
            const uint32_t ModeCount = BuildModeList(&Preferred, Modes);

            for (uint32_t m = 0; m < ModeCount; ++m)
            {
                const NyanMode& M = Modes[m];
                CHECK(M.RefreshHz >= R.MinVHz && M.RefreshHz <= R.MaxVHz,
                      "preferred %ux%u@%u: mode %ux%u@%u outside vertical range %u-%u Hz",
                      Preferred.Width, Preferred.Height, Preferred.RefreshHz,
                      M.Width, M.Height, M.RefreshHz, R.MinVHz, R.MaxVHz);

                const uint32_t LineHz = HorizontalHz(M);
                CHECK(LineHz / 1000 >= R.MinHkHz && (LineHz + 999) / 1000 <= R.MaxHkHz,
                      "preferred %ux%u@%u: mode %ux%u@%u at %u.%u kHz outside %u-%u kHz",
                      Preferred.Width, Preferred.Height, Preferred.RefreshHz,
                      M.Width, M.Height, M.RefreshHz, LineHz / 1000, (LineHz / 100) % 10,
                      R.MinHkHz, R.MaxHkHz);

                CHECK(PixelClockHz(M) <= R.MaxPclkHz,
                      "preferred %ux%u@%u: mode %ux%u@%u needs %llu Hz > max %u Hz",
                      Preferred.Width, Preferred.Height, Preferred.RefreshHz,
                      M.Width, M.Height, M.RefreshHz,
                      static_cast<unsigned long long>(PixelClockHz(M)), R.MaxPclkHz);
            }
        }

        // 4K@120 is the case that needs the EDID 1.4 "+255 kHz" offset flag.
        uint8_t E[128];
        BuildEdid(1, NyanMode{ 3840, 2160, 120 }, E);
        const DecodedRange R = DecodeRange(E);
        CHECK(R.MaxHkHz >= 264, "4K@120 needs a 264 kHz line rate, range says %u kHz", R.MaxHkHz);
        CHECK((E[108 + 4] & 0x0C) != 0, "horizontal offset flag not set for a >255 kHz maximum");
    }

    // The container id is the client's supported route from an OS display back
    // to the cookie that created it, so the derivation has to be exactly
    // reversible and must reject container ids that are not ours.
    void TestContainerIdCorrelation()
    {
        const uint32_t kCookies[] = { 1u, 0xC0FFEE01u, 0xDEADBEEFu, 0xFFFFFFFFu, 0x00FF00FFu };
        for (uint32_t Cookie : kCookies)
        {
            GUID Id = {};
            NyanVddMakeContainerId(Cookie, &Id);
            CHECK(NyanVddCookieFromContainerId(&Id) == Cookie,
                  "container id round-trip failed for 0x%08X (got 0x%08X)",
                  Cookie, NyanVddCookieFromContainerId(&Id));

            // Distinct cookies must not collide.
            for (uint32_t Other : kCookies)
            {
                if (Other == Cookie) continue;
                GUID OtherId = {};
                NyanVddMakeContainerId(Other, &OtherId);
                CHECK(memcmp(&Id, &OtherId, sizeof(GUID)) != 0,
                      "cookies 0x%08X and 0x%08X share a container id", Cookie, Other);
            }
        }

        // The byte layout observed on hardware: cookie 0xDEADBEEF appears as
        // {408B3FE4-8AC2-4E97-83D8-BE29EFBEADDE}.
        GUID Known = {};
        NyanVddMakeContainerId(0xDEADBEEFu, &Known);
        const unsigned char Expected[8] = { 0x83, 0xD8, 0xBE, 0x29, 0xEF, 0xBE, 0xAD, 0xDE };
        CHECK(Known.Data1 == 0x408B3FE4 && Known.Data2 == 0x8AC2 && Known.Data3 == 0x4E97 &&
              memcmp(Known.Data4, Expected, 8) == 0,
              "container id layout changed; clients decoding it would break");

        // Foreign container ids must not resolve to a cookie.
        GUID Foreign = {};
        NyanVddMakeContainerId(0x12345678u, &Foreign);
        Foreign.Data1 ^= 0xFFu;
        CHECK(NyanVddCookieFromContainerId(&Foreign) == 0,
              "a container id from another device resolved to a cookie");

        GUID Zero = {};
        CHECK(NyanVddCookieFromContainerId(&Zero) == 0, "the null GUID resolved to a cookie");

        // A cookie is never 0, so a valid container id never decodes to 0.
        GUID ZeroCookie = {};
        NyanVddMakeContainerId(0u, &ZeroCookie);
        CHECK(NyanVddCookieFromContainerId(&ZeroCookie) == 0,
              "cookie 0 is reserved and must not round-trip");
    }

    // The EDID identity the correlation recipe pre-filters on has to match the
    // bytes the driver actually writes.
    void TestEdidIdentityConstants()
    {
        uint8_t E[128];
        BuildEdid(0x1234ABCDu, NyanMode{ 1920, 1080, 60 }, E);

        const uint16_t ManufactureId = static_cast<uint16_t>(E[8] | (E[9] << 8));
        const uint16_t ProductCodeId = static_cast<uint16_t>(E[10] | (E[11] << 8));
        CHECK(ManufactureId == NYANVDD_EDID_MANUFACTURE_ID,
              "NYANVDD_EDID_MANUFACTURE_ID is 0x%04X but the EDID carries 0x%04X",
              (unsigned)NYANVDD_EDID_MANUFACTURE_ID, ManufactureId);
        CHECK(ProductCodeId == NYANVDD_EDID_PRODUCT_CODE_ID,
              "NYANVDD_EDID_PRODUCT_CODE_ID is 0x%04X but the EDID carries 0x%04X",
              (unsigned)NYANVDD_EDID_PRODUCT_CODE_ID, ProductCodeId);
    }

    // NYANVDD_MONITOR_INFO.Flags carries plug flags and state bits in one
    // field, so the two namespaces must not overlap: a client masking for one
    // must never see the other.
    void TestMonitorFlagNamespaces()
    {
        CHECK((NYANVDD_MONITOR_STATE_MASK & NYANVDD_PLUG_FLAG_HDR10) == 0,
              "the state-bit mask overlaps NYANVDD_PLUG_FLAG_HDR10");
        CHECK((NYANVDD_MONITOR_FLAG_ACTIVE & NYANVDD_MONITOR_STATE_MASK) == NYANVDD_MONITOR_FLAG_ACTIVE,
              "NYANVDD_MONITOR_FLAG_ACTIVE is not inside the state-bit range");
        CHECK((NYANVDD_MONITOR_FLAG_ACTIVE & ~NYANVDD_MONITOR_STATE_MASK) == 0,
              "NYANVDD_MONITOR_FLAG_ACTIVE leaks into the plug-flag range");

        // A monitor plugged with HDR and reported active must decode as both,
        // and the plug flags must survive a round-trip through the state mask.
        const UINT32 Combined = NYANVDD_PLUG_FLAG_HDR10 | NYANVDD_MONITOR_FLAG_ACTIVE;
        CHECK((Combined & NYANVDD_PLUG_FLAG_HDR10) != 0, "plug flag lost when combined with state");
        CHECK((Combined & NYANVDD_MONITOR_FLAG_ACTIVE) != 0, "state bit lost when combined");
        CHECK((Combined & ~NYANVDD_MONITOR_STATE_MASK) == NYANVDD_PLUG_FLAG_HDR10,
              "masking off the state bits does not recover exactly the plug flags");
    }

    // The plug range advertised to clients must be the range the driver enforces.
    void TestAdvertisedPlugRange()
    {
        CHECK(IsSupportedMode(NyanMode{ NYANVDD_MIN_WIDTH, NYANVDD_MIN_HEIGHT, NYANVDD_MIN_REFRESH_HZ }),
              "the documented minimum mode is rejected");
        CHECK(!IsSupportedMode(NyanMode{ NYANVDD_MIN_WIDTH - 1, NYANVDD_MIN_HEIGHT, NYANVDD_MIN_REFRESH_HZ }),
              "a mode below the documented minimum width is accepted");
        CHECK(!IsSupportedMode(NyanMode{ NYANVDD_MIN_WIDTH, NYANVDD_MIN_HEIGHT, NYANVDD_MAX_REFRESH_HZ + 1 }),
              "a mode above the documented maximum refresh is accepted");
        CHECK(!IsSupportedMode(NyanMode{ NYANVDD_MAX_DIMENSION + 1, NYANVDD_MIN_HEIGHT, 60 }),
              "a mode above the documented maximum width is accepted");
        CHECK(kMinWidth == NYANVDD_MIN_WIDTH && kMinHeight == NYANVDD_MIN_HEIGHT &&
              kMaxActivePixels == NYANVDD_MAX_DIMENSION &&
              kMinRefreshHz == NYANVDD_MIN_REFRESH_HZ && kMaxRefreshHz == NYANVDD_MAX_REFRESH_HZ,
              "the header's advertised plug range drifted from the driver's limits");
    }
}

int main()
{
    TestContainerIdCorrelation();
    TestEdidIdentityConstants();
    TestMonitorFlagNamespaces();
    TestAdvertisedPlugRange();
    TestSupportedModePredicate();
    TestEdidStructure();
    TestCookieRoundTrip();
    TestModeListCarriesPluggedMode();
    TestPhysicalSizeIsDesktopClass();
    TestDtdPreservesPixelCount();
    TestRangeLimitsCoverReportedModes();

    printf("%d checks, %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
