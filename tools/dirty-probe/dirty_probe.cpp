// dirty-probe — measures Windows.Graphics.Capture dirty-region behaviour on a
// monitor, to verify (or refute) the assumptions nyanvdd's "minimal capture
// area" goal rests on:
//
//   1. a fully static screen produces no capture frames at all,
//   2. a small update produces dirty rects of roughly the update's size
//      (use --stimulus for a deterministic NxN animation),
//   3. how much damage mouse-cursor movement causes (nyanvdd implements no
//      hardware-cursor DDI, so the OS composes the pointer into the desktop
//      image of its monitors — every pointer move may be real damage; use
//      --wiggle for a deterministic cursor motion),
//   4. whether IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS changes what
//      WGC reports (compare runs with the driver's DisableAdapterFlags=0x20).
//
// It deliberately knows nothing about nyanvdd: it takes a GDI display name
// ("\\.\DISPLAY3", e.g. from `nyanvddctl resolve`), so the same measurement
// runs against physical monitors and other VDDs for comparison.
//
// Dirty-region reporting (GraphicsCaptureSession.DirtyRegionMode and
// Direct3D11CaptureFrame.DirtyRegions) is a Windows 11 24H2 API — conveniently
// also nyanvdd's OS floor. On older builds the tool still reports frame counts,
// with a warning that dirty data is unavailable.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace winrt;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{
    void PrintUsage()
    {
        wprintf(
            L"dirty-probe — measure WGC dirty regions on one monitor\n"
            L"\n"
            L"  dirty-probe --list\n"
            L"  dirty-probe [options]\n"
            L"\n"
            L"  --monitor <name|n>   \\\\.\\DISPLAYn, DISPLAYn or just n (default: primary)\n"
            L"  --seconds <n>        measurement duration (default 10)\n"
            L"  --no-cursor          IsCursorCaptureEnabled(false) on the session\n"
            L"  --stimulus <px[@hz]> animate a px-square at hz (default 10 Hz) on the target\n"
            L"  --wiggle             move the mouse in a circle on the target monitor\n"
            L"                       (waits for >=3 s of input idle, restores the position)\n"
            L"  --csv <path>         append per-frame records as CSV\n"
            L"  --label <name>       scenario name for the summary/CSV (default: run)\n"
            L"  --verbose            print one line per captured frame\n");
    }

    // ---- monitor selection ----

    struct MonitorEntry
    {
        HMONITOR Handle;
        std::wstring Device; // "\\.\DISPLAY3"
        RECT Rect;
        bool Primary;
    };

    std::vector<MonitorEntry> EnumerateMonitors()
    {
        std::vector<MonitorEntry> Result;
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR Handle, HDC, LPRECT, LPARAM Context) -> BOOL
            {
                MONITORINFOEXW Info = {};
                Info.cbSize = sizeof(Info);
                if (GetMonitorInfoW(Handle, &Info))
                {
                    auto* List = reinterpret_cast<std::vector<MonitorEntry>*>(Context);
                    List->push_back({ Handle, Info.szDevice, Info.rcMonitor,
                                      (Info.dwFlags & MONITORINFOF_PRIMARY) != 0 });
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&Result));
        return Result;
    }

    // Accepts "\\.\DISPLAY3", "DISPLAY3" or "3". Exact match only: a substring
    // match would make "DISPLAY1" ambiguous with "DISPLAY10".
    const MonitorEntry* PickMonitor(const std::vector<MonitorEntry>& Monitors, const wchar_t* Arg)
    {
        if (!Arg)
        {
            for (const auto& M : Monitors) { if (M.Primary) return &M; }
            return Monitors.empty() ? nullptr : &Monitors[0];
        }

        std::wstring Wanted = Arg;
        if (!Wanted.empty() && iswdigit(Wanted[0]))
        {
            Wanted = L"DISPLAY" + Wanted;
        }
        if (Wanted.rfind(L"\\\\.\\", 0) != 0)
        {
            Wanted = L"\\\\.\\" + Wanted;
        }
        for (const auto& M : Monitors)
        {
            if (_wcsicmp(M.Device.c_str(), Wanted.c_str()) == 0) return &M;
        }
        return nullptr;
    }

    // ---- stimulus window (deterministic small-area damage) ----

    struct StimulusState
    {
        int Size = 0;
        int Hz = 10;
        RECT MonitorRect = {};
        std::atomic<bool> Stop{ false };
        std::atomic<bool> Tick{ false }; // current color phase
        HANDLE Thread = nullptr;
    };

    StimulusState g_Stimulus;

    LRESULT CALLBACK StimulusWndProc(HWND Wnd, UINT Msg, WPARAM WParam, LPARAM LParam)
    {
        switch (Msg)
        {
        case WM_TIMER:
            if (g_Stimulus.Stop)
            {
                DestroyWindow(Wnd);
                return 0;
            }
            g_Stimulus.Tick = !g_Stimulus.Tick;
            InvalidateRect(Wnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT Paint;
            HDC Dc = BeginPaint(Wnd, &Paint);
            RECT Client;
            GetClientRect(Wnd, &Client);
            HBRUSH Brush = CreateSolidBrush(g_Stimulus.Tick ? RGB(255, 96, 32) : RGB(32, 96, 255));
            FillRect(Dc, &Client, Brush);
            DeleteObject(Brush);
            EndPaint(Wnd, &Paint);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(Wnd, Msg, WParam, LParam);
    }

    DWORD CALLBACK StimulusThread(LPVOID)
    {
        WNDCLASSW Class = {};
        Class.lpfnWndProc = StimulusWndProc;
        Class.hInstance = GetModuleHandleW(nullptr);
        Class.lpszClassName = L"NyanDirtyProbeStimulus";
        RegisterClassW(&Class);

        // Off-centre so it does not overlap the cursor-wiggle circle.
        const RECT& Rect = g_Stimulus.MonitorRect;
        const int X = Rect.left + (Rect.right - Rect.left) / 4;
        const int Y = Rect.top + (Rect.bottom - Rect.top) / 4;

        // WS_POPUP: no frame, no DWM shadow — the damage is exactly Size x Size.
        // NOACTIVATE/TOOLWINDOW: do not steal focus, do not appear in the taskbar.
        HWND Wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                   Class.lpszClassName, L"dirty-probe stimulus", WS_POPUP,
                                   X, Y, g_Stimulus.Size, g_Stimulus.Size,
                                   nullptr, nullptr, Class.hInstance, nullptr);
        if (!Wnd)
        {
            return 1;
        }
        ShowWindow(Wnd, SW_SHOWNOACTIVATE);
        SetTimer(Wnd, 1, 1000 / (g_Stimulus.Hz > 0 ? g_Stimulus.Hz : 10), nullptr);

        MSG Msg;
        while (GetMessageW(&Msg, nullptr, 0, 0))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        return 0;
    }

    // ---- cursor wiggle (deterministic pointer damage) ----

    struct WiggleState
    {
        RECT MonitorRect = {};
        std::atomic<bool> Stop{ false };
        HANDLE Thread = nullptr;
    };

    WiggleState g_Wiggle;

    DWORD CALLBACK WiggleThread(LPVOID)
    {
        POINT Original = {};
        GetCursorPos(&Original);

        const RECT& Rect = g_Wiggle.MonitorRect;
        const int CenterX = (Rect.left + Rect.right) / 2;
        const int CenterY = (Rect.top + Rect.bottom) / 2;
        const double Radius = 80.0;

        double Angle = 0.0;
        while (!g_Wiggle.Stop)
        {
            SetCursorPos(CenterX + (int)(Radius * cos(Angle)),
                         CenterY + (int)(Radius * sin(Angle)));
            Angle += 0.20; // ~1 revolution / 2 s at 16 ms steps
            Sleep(16);
        }

        SetCursorPos(Original.x, Original.y);
        return 0;
    }

    // Real input would fight SetCursorPos and pollute the measurement, so wait
    // for the user to let go of the mouse/keyboard first.
    bool WaitForInputIdle3s()
    {
        const ULONGLONG Deadline = GetTickCount64() + 60 * 1000;
        while (GetTickCount64() < Deadline)
        {
            LASTINPUTINFO Last = { sizeof(LASTINPUTINFO) };
            if (GetLastInputInfo(&Last) && GetTickCount() - Last.dwTime >= 3000)
            {
                return true;
            }
            Sleep(250);
        }
        return false;
    }

    // ---- capture records ----

    struct FrameRecord
    {
        int64_t TimeRel100ns = 0; // Direct3D11CaptureFrame.SystemRelativeTime
        uint32_t RectCount = 0;
        uint64_t DirtyPx = 0;
        RECT BoundingBox = {};
        bool HasDirtyInfo = false;
    };

    uint64_t Percentile(std::vector<uint64_t>& Sorted, double P)
    {
        if (Sorted.empty()) return 0;
        const size_t Index = (size_t)(P * (Sorted.size() - 1) + 0.5);
        return Sorted[Index];
    }
}

int wmain(int argc, wchar_t** argv)
{
    // Physical-pixel coordinates everywhere (monitor rects, SetCursorPos,
    // stimulus placement) — without this a scaled monitor skews all of them.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const wchar_t* MonitorArg = nullptr;
    const wchar_t* CsvPath = nullptr;
    const wchar_t* Label = L"run";
    int Seconds = 10;
    bool CursorCapture = true;
    bool Wiggle = false;
    bool Verbose = false;
    bool ListOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--list") == 0) { ListOnly = true; }
        else if (wcscmp(argv[i], L"--monitor") == 0 && i + 1 < argc) { MonitorArg = argv[++i]; }
        else if (wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc) { Seconds = _wtoi(argv[++i]); }
        else if (wcscmp(argv[i], L"--no-cursor") == 0) { CursorCapture = false; }
        else if (wcscmp(argv[i], L"--wiggle") == 0) { Wiggle = true; }
        else if (wcscmp(argv[i], L"--csv") == 0 && i + 1 < argc) { CsvPath = argv[++i]; }
        else if (wcscmp(argv[i], L"--label") == 0 && i + 1 < argc) { Label = argv[++i]; }
        else if (wcscmp(argv[i], L"--verbose") == 0) { Verbose = true; }
        else if (wcscmp(argv[i], L"--stimulus") == 0 && i + 1 < argc)
        {
            ++i;
            if (swscanf_s(argv[i], L"%d@%d", &g_Stimulus.Size, &g_Stimulus.Hz) < 1 ||
                g_Stimulus.Size <= 0)
            {
                fwprintf(stderr, L"bad --stimulus value: %s\n", argv[i]);
                return 2;
            }
        }
        else
        {
            PrintUsage();
            return 2;
        }
    }
    if (Seconds <= 0) { Seconds = 10; }

    const auto Monitors = EnumerateMonitors();
    if (Monitors.empty())
    {
        fwprintf(stderr, L"no monitors found\n");
        return 1;
    }

    if (ListOnly)
    {
        for (const auto& M : Monitors)
        {
            wprintf(L"%-14s %ldx%ld at (%ld,%ld)%s\n", M.Device.c_str(),
                    M.Rect.right - M.Rect.left, M.Rect.bottom - M.Rect.top,
                    M.Rect.left, M.Rect.top, M.Primary ? L"  [primary]" : L"");
        }
        return 0;
    }

    const MonitorEntry* Monitor = PickMonitor(Monitors, MonitorArg);
    if (!Monitor)
    {
        fwprintf(stderr, L"monitor not found: %s (try --list)\n", MonitorArg ? MonitorArg : L"?");
        return 1;
    }

    init_apartment(apartment_type::multi_threaded);

    // D3D device for the frame pool. The frames are never read back — only
    // their metadata (timing + dirty regions) matters here.
    com_ptr<ID3D11Device> D3dDevice;
    HRESULT Hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, D3dDevice.put(), nullptr, nullptr);
    if (FAILED(Hr))
    {
        fwprintf(stderr, L"D3D11CreateDevice failed: 0x%08X\n", Hr);
        return 1;
    }

    IDirect3DDevice Device{ nullptr };
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        D3dDevice.as<IDXGIDevice>().get(),
        reinterpret_cast<::IInspectable**>(put_abi(Device))));

    GraphicsCaptureItem Item{ nullptr };
    auto Interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    Hr = Interop->CreateForMonitor(Monitor->Handle, guid_of<GraphicsCaptureItem>(), put_abi(Item));
    if (FAILED(Hr))
    {
        fwprintf(stderr, L"CreateForMonitor failed: 0x%08X (is this monitor active,"
                 L" and is this process running on the visible desktop?)\n", Hr);
        return 1;
    }

    const SizeInt32 ItemSize = Item.Size();
    const uint64_t ScreenPx = (uint64_t)ItemSize.Width * ItemSize.Height;

    auto Pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        Device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, ItemSize);
    auto Session = Pool.CreateCaptureSession(Item);

    Session.IsCursorCaptureEnabled(CursorCapture);

    const bool DirtySupported = ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"DirtyRegionMode");
    if (DirtySupported)
    {
        // ReportOnly: full frames, dirty rects as metadata — measurement mode.
        Session.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly);
    }
    else
    {
        wprintf(L"WARNING: DirtyRegionMode is not available on this OS (needs Windows 11\n"
                L"         24H2 / build 26100+). Frame counts only.\n\n");
    }

    // The yellow capture border lives on the captured desktop itself, so leave
    // no doubt about whether it influences the measurement.
    bool Borderless = false;
    try
    {
        GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
        Session.IsBorderRequired(false);
        Borderless = true;
    }
    catch (...) {}

    std::mutex RecordsLock;
    std::vector<FrameRecord> Records;
    Records.reserve(4096);

    auto Revoker = Pool.FrameArrived(auto_revoke,
        [&](Direct3D11CaptureFramePool const& Sender, winrt::Windows::Foundation::IInspectable const&)
        {
            auto Frame = Sender.TryGetNextFrame();
            if (!Frame) return;

            FrameRecord Record;
            Record.TimeRel100ns = Frame.SystemRelativeTime().count();
            if (DirtySupported)
            {
                Record.HasDirtyInfo = true;
                bool First = true;
                for (const RectInt32& Rect : Frame.DirtyRegions())
                {
                    Record.RectCount++;
                    Record.DirtyPx += (uint64_t)Rect.Width * Rect.Height;
                    const RECT Native = { Rect.X, Rect.Y, Rect.X + Rect.Width, Rect.Y + Rect.Height };
                    if (First) { Record.BoundingBox = Native; First = false; }
                    else { UnionRect(&Record.BoundingBox, &Record.BoundingBox, &Native); }
                }
            }

            {
                std::lock_guard<std::mutex> Guard(RecordsLock);
                Records.push_back(Record);
            }
            if (Verbose)
            {
                wprintf(L"frame %5zu: %2u rect(s) %8llu px  bbox (%ld,%ld)-(%ld,%ld)\n",
                        Records.size(), Record.RectCount, Record.DirtyPx,
                        Record.BoundingBox.left, Record.BoundingBox.top,
                        Record.BoundingBox.right, Record.BoundingBox.bottom);
            }
        });

    wprintf(L"label      : %s\n", Label);
    wprintf(L"monitor    : %s  %dx%d (%llu px)\n", Monitor->Device.c_str(),
            ItemSize.Width, ItemSize.Height, ScreenPx);
    wprintf(L"cursor     : %s   border: %s   dirty info: %s\n",
            CursorCapture ? L"captured" : L"excluded",
            Borderless ? L"disabled" : L"system default",
            DirtySupported ? L"ReportOnly" : L"UNAVAILABLE");
    if (g_Stimulus.Size > 0)
    {
        wprintf(L"stimulus   : %dx%d @ %d Hz (expected dirty %d px/update)\n",
                g_Stimulus.Size, g_Stimulus.Size, g_Stimulus.Hz,
                g_Stimulus.Size * g_Stimulus.Size);
    }
    if (Wiggle)
    {
        wprintf(L"wiggle     : cursor circling the monitor centre\n");
        wprintf(L"waiting for 3 s of input idle before taking the cursor...\n");
        if (!WaitForInputIdle3s())
        {
            fwprintf(stderr, L"gave up waiting for input idle (60 s)\n");
            return 1;
        }
    }
    wprintf(L"measuring %d s...\n\n", Seconds);

    if (g_Stimulus.Size > 0)
    {
        g_Stimulus.MonitorRect = Monitor->Rect;
        g_Stimulus.Thread = CreateThread(nullptr, 0, StimulusThread, nullptr, 0, nullptr);
    }
    if (Wiggle)
    {
        g_Wiggle.MonitorRect = Monitor->Rect;
        g_Wiggle.Thread = CreateThread(nullptr, 0, WiggleThread, nullptr, 0, nullptr);
    }

    const ULONGLONG StartedTick = GetTickCount64();
    Session.StartCapture();
    Sleep((DWORD)Seconds * 1000);
    const double Elapsed = (GetTickCount64() - StartedTick) / 1000.0;

    Revoker.revoke();
    Session.Close();
    Pool.Close();

    if (g_Stimulus.Thread)
    {
        g_Stimulus.Stop = true;
        WaitForSingleObject(g_Stimulus.Thread, 3000);
        CloseHandle(g_Stimulus.Thread);
    }
    if (g_Wiggle.Thread)
    {
        g_Wiggle.Stop = true;
        WaitForSingleObject(g_Wiggle.Thread, 3000);
        CloseHandle(g_Wiggle.Thread);
    }

    // ---- summary ----

    std::vector<FrameRecord> Frames;
    {
        std::lock_guard<std::mutex> Guard(RecordsLock);
        Frames = Records;
    }

    wprintf(L"frames     : %zu in %.1f s (%.1f fps)\n", Frames.size(), Elapsed,
            Elapsed > 0 ? Frames.size() / Elapsed : 0.0);

    if (Frames.empty())
    {
        wprintf(L"result     : NO frames — the compositor saw no damage at all\n");
    }
    else if (DirtySupported)
    {
        // The first frame is the initial full-screen copy; report it apart so
        // it does not drown the steady-state numbers.
        const FrameRecord& First = Frames.front();
        wprintf(L"first frame: %u rect(s), %llu px (%.1f%% of screen)\n",
                First.RectCount, First.DirtyPx,
                ScreenPx ? 100.0 * First.DirtyPx / ScreenPx : 0.0);

        std::vector<uint64_t> Areas;
        uint64_t TotalPx = 0;
        uint32_t MaxRects = 0;
        uint64_t RectSum = 0;
        for (size_t i = 1; i < Frames.size(); ++i)
        {
            Areas.push_back(Frames[i].DirtyPx);
            TotalPx += Frames[i].DirtyPx;
            RectSum += Frames[i].RectCount;
            MaxRects = (std::max)(MaxRects, Frames[i].RectCount);
        }

        if (!Areas.empty())
        {
            std::sort(Areas.begin(), Areas.end());
            const size_t Count = Areas.size();
            const uint64_t Avg = TotalPx / Count;
            wprintf(L"steady     : %zu frame(s) after the first\n", Count);
            wprintf(L"dirty px   : avg %llu (%.2f%%)  median %llu  p95 %llu  max %llu\n",
                    Avg, ScreenPx ? 100.0 * Avg / ScreenPx : 0.0,
                    Percentile(Areas, 0.5), Percentile(Areas, 0.95), Areas.back());
            wprintf(L"dirty rects: avg %.1f  max %u\n", (double)RectSum / Count, MaxRects);
            wprintf(L"bandwidth  : %.0f dirty px/s (full-screen equivalent %.2f fps)\n",
                    TotalPx / Elapsed, ScreenPx ? TotalPx / Elapsed / ScreenPx : 0.0);
        }
    }

    if (CsvPath)
    {
        FILE* Csv = nullptr;
        if (_wfopen_s(&Csv, CsvPath, L"a") == 0 && Csv)
        {
            fseek(Csv, 0, SEEK_END);
            if (ftell(Csv) == 0)
            {
                fwprintf(Csv, L"label,frame,t_rel_ms,rects,dirty_px,bbox_left,bbox_top,bbox_right,bbox_bottom\n");
            }
            for (size_t i = 0; i < Frames.size(); ++i)
            {
                const FrameRecord& Record = Frames[i];
                fwprintf(Csv, L"%s,%zu,%.3f,%u,%llu,%ld,%ld,%ld,%ld\n", Label, i,
                         (Record.TimeRel100ns - Frames[0].TimeRel100ns) / 10000.0,
                         Record.RectCount, Record.DirtyPx,
                         Record.BoundingBox.left, Record.BoundingBox.top,
                         Record.BoundingBox.right, Record.BoundingBox.bottom);
            }
            fclose(Csv);
            wprintf(L"csv        : appended %zu row(s) to %s\n", Frames.size(), CsvPath);
        }
        else
        {
            fwprintf(stderr, L"could not open CSV file: %s\n", CsvPath);
        }
    }

    return 0;
}
