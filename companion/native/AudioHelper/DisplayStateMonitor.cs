using System.Runtime.InteropServices;

// Streams the Windows console display power state on stdout, one line per
// change (plus an initial line):
//
//   display: unknown        -- printed immediately, before the first notification
//   display: on             -- monitor powered
//   display: off            -- monitor powered off (DPMS timeout, "turn off screen")
//   display: dimmed         -- monitor dimmed (a DPMS step before full off)
//
// GUID_CONSOLE_DISPLAY_STATE is notification-only; there is no "get current
// state" call, so a subscribed message loop is the correct approach. Windows
// delivers one notification with the current state right after subscribing.
//
// Exits on stdin line "stop" or Ctrl-C.
sealed class DisplayStateMonitor
{
    private const int WM_POWERBROADCAST = 0x0218;
    private const int PBT_POWERSETTINGCHANGE = 0x8013;
    private const int WM_CLOSE = 0x0010;
    private const uint DEVICE_NOTIFY_WINDOW_HANDLE = 0x00000000;

    // {6FE69556-704A-47A0-8F24-C28D936FDA47}
    private static readonly Guid GuidConsoleDisplayState =
        new("6FE69556-704A-47A0-8F24-C28D936FDA47");

    private IntPtr hwnd;
    private IntPtr registration;
    private WndProcDelegate? wndProcKeepAlive;
    private string lastState = "";

    public void Run()
    {
        Console.Out.WriteLine("display: unknown");
        Console.Out.Flush();

        var stopThread = new Thread(ReadStopLoop) { IsBackground = true, Name = "DisplayStateMonitorStop" };
        stopThread.Start();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; PostClose(); };

        wndProcKeepAlive = WndProc;
        var wc = new WNDCLASS
        {
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(wndProcKeepAlive),
            hInstance = GetModuleHandle(null),
            lpszClassName = "DS5BridgeDisplayStateMonitor"
        };
        RegisterClass(ref wc);

        // HWND_MESSAGE = -3: a message-only window, no UI, still pumps WM_POWERBROADCAST.
        hwnd = CreateWindowEx(0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
            new IntPtr(-3), IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (hwnd == IntPtr.Zero)
        {
            Console.Error.WriteLine("status: display-state-monitor-window-failed");
            return;
        }

        var displayStateGuid = GuidConsoleDisplayState;
        registration = RegisterPowerSettingNotification(hwnd, ref displayStateGuid,
            DEVICE_NOTIFY_WINDOW_HANDLE);
        if (registration == IntPtr.Zero)
        {
            Console.Error.WriteLine("status: display-state-monitor-register-failed");
        }

        Console.Error.WriteLine("status: display-state-monitor-started");

        while (GetMessage(out var msg, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }

        if (registration != IntPtr.Zero)
        {
            UnregisterPowerSettingNotification(registration);
        }
        Console.Error.WriteLine("status: display-state-monitor-stopped");
    }

    private IntPtr WndProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam)
    {
        if (message == WM_POWERBROADCAST && wParam.ToInt32() == PBT_POWERSETTINGCHANGE)
        {
            var setting = Marshal.PtrToStructure<POWERBROADCAST_SETTING>(lParam);
            if (setting.PowerSetting == GuidConsoleDisplayState)
            {
                Emit(setting.Data switch
                {
                    0 => "off",
                    1 => "on",
                    2 => "dimmed",
                    _ => "unknown"
                });
            }
            return IntPtr.Zero;
        }
        return DefWindowProc(window, message, wParam, lParam);
    }

    private void Emit(string state)
    {
        if (state == lastState)
        {
            return;
        }
        lastState = state;
        Console.Out.WriteLine($"display: {state}");
        Console.Out.Flush();
    }

    private void PostClose()
    {
        if (hwnd != IntPtr.Zero)
        {
            PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }
    }

    private void ReadStopLoop()
    {
        try
        {
            string? line;
            while ((line = Console.In.ReadLine()) is not null)
            {
                if (line.Trim().Equals("stop", StringComparison.OrdinalIgnoreCase))
                {
                    break;
                }
            }
        }
        catch
        {
            // stdin closed -- treat as stop.
        }
        PostClose();
    }

    // --- Win32 ---

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct WNDCLASS
    {
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int ptX;
        public int ptY;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POWERBROADCAST_SETTING
    {
        public Guid PowerSetting;
        public uint DataLength;
        public byte Data;
    }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern ushort RegisterClass(ref WNDCLASS lpWndClass);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowEx(uint dwExStyle, string lpClassName, string lpWindowName,
        uint dwStyle, int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu,
        IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern int GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref MSG lpMsg);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr RegisterPowerSettingNotification(IntPtr hRecipient,
        ref Guid PowerSettingGuid, uint Flags);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterPowerSettingNotification(IntPtr Handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);
}
