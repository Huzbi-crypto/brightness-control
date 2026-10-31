using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;

namespace BrightnessControl
{
    /// <summary>
    /// Low level Win+A interceptor plus a mouse-down watcher used to dismiss the panel.
    /// Low level hooks are only allowed to live on a thread that pumps messages, which is
    /// the UI thread here.
    /// </summary>
    internal sealed class GlobalHooks : IDisposable
    {
        private const int WH_KEYBOARD_LL = 13;
        private const int WH_MOUSE_LL = 14;

        private const int WM_KEYDOWN = 0x0100;
        private const int WM_KEYUP = 0x0101;
        private const int WM_SYSKEYDOWN = 0x0104;
        private const int WM_SYSKEYUP = 0x0105;
        private const int WM_LBUTTONDOWN = 0x0201;
        private const int WM_RBUTTONDOWN = 0x0204;
        private const int WM_MBUTTONDOWN = 0x0207;

        private const int VK_A = 0x41;
        private const int VK_F24 = 0x87;
        private const uint KEYEVENTF_KEYUP = 0x0002;
        private const int VK_LWIN = 0x5B;
        private const int VK_RWIN = 0x5C;

        [StructLayout(LayoutKind.Sequential)]
        private struct KBDLLHOOKSTRUCT
        {
            public uint vkCode;
            public uint scanCode;
            public uint flags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int x;
            public int y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MSLLHOOKSTRUCT
        {
            public POINT pt;
            public uint mouseData;
            public uint flags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookEx(int idHook, HookProc proc, IntPtr hMod, uint threadId);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UnhookWindowsHookEx(IntPtr hhk);

        [DllImport("user32.dll")]
        private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandle(string name);

        [DllImport("user32.dll")]
        private static extern short GetAsyncKeyState(int vk);

        [DllImport("user32.dll")]
        private static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

        public Action OnWinA;
        public Action<Point> OnMouseDown;
        public bool InterceptWinA = true;

        private IntPtr _kbHook = IntPtr.Zero;
        private IntPtr _mouseHook = IntPtr.Zero;
        private readonly HookProc _kbProc;
        private readonly HookProc _mouseProc;
        private bool _swallowAUp;

        /// <param name="withKeyboard">
        /// Only install the keyboard hook when Win+A interception is actually wanted. It is off
        /// by default now that brightness lives inside the real Quick Settings panel.
        /// </param>
        public GlobalHooks(bool withKeyboard)
        {
            _kbProc = KeyboardProc;
            _mouseProc = MouseProc;
            IntPtr mod = GetModuleHandle(null);
            if (withKeyboard) _kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, _kbProc, mod, 0);
            _mouseHook = SetWindowsHookEx(WH_MOUSE_LL, _mouseProc, mod, 0);
        }

        public bool KeyboardHookOk { get { return _kbHook != IntPtr.Zero; } }
        public bool MouseHookOk { get { return _mouseHook != IntPtr.Zero; } }

        private static bool WinDown()
        {
            return (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
        }

        private static void InjectKey(int vk)
        {
            keybd_event((byte)vk, 0, 0, UIntPtr.Zero);
            keybd_event((byte)vk, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        }

        private IntPtr KeyboardProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            // A throwing low-level hook gets silently removed by Windows, and any state it was
            // half way through changing stays changed. Fail open instead: drop interception and
            // let every keystroke through untouched.
            try
            {
                return KeyboardProcCore(nCode, wParam, lParam);
            }
            catch
            {
                InterceptWinA = false;
                return CallNextHookEx(_kbHook, nCode, wParam, lParam);
            }
        }

        private IntPtr KeyboardProcCore(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0)
            {
                int msg = wParam.ToInt32();
                KBDLLHOOKSTRUCT k = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));

                if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
                {
                    if (k.vkCode == VK_A && InterceptWinA && WinDown())
                    {
                        _swallowAUp = true;
                        // The shell opens Start when it sees Win pressed and released with no key
                        // in between. Swallowing the A hides that, so stand in for it with an
                        // unbound key: the shell then sees "Win + F24" and does nothing.
                        // NEVER swallow a modifier key-up - that desyncs the OS modifier state
                        // and turns every later keystroke into a Win+key shortcut.
                        InjectKey(VK_F24);
                        if (OnWinA != null) OnWinA();
                        return (IntPtr)1;
                    }
                }
                else if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                {
                    if (k.vkCode == VK_A && _swallowAUp)
                    {
                        _swallowAUp = false;
                        return (IntPtr)1;
                    }
                }
            }
            return CallNextHookEx(_kbHook, nCode, wParam, lParam);
        }

        private IntPtr MouseProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                return MouseProcCore(nCode, wParam, lParam);
            }
            catch
            {
                return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
            }
        }

        private IntPtr MouseProcCore(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0)
            {
                int msg = wParam.ToInt32();
                if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
                {
                    if (OnMouseDown != null)
                    {
                        MSLLHOOKSTRUCT m = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
                        try { OnMouseDown(new Point(m.pt.x, m.pt.y)); } catch { }
                    }
                }
            }
            return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
        }

        public void Dispose()
        {
            if (_kbHook != IntPtr.Zero)
            {
                UnhookWindowsHookEx(_kbHook);
                _kbHook = IntPtr.Zero;
            }
            if (_mouseHook != IntPtr.Zero)
            {
                UnhookWindowsHookEx(_mouseHook);
                _mouseHook = IntPtr.Zero;
            }
        }
    }
}
