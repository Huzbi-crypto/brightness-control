using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace BrightnessControl
{
    internal static class Log
    {
        private static readonly string Path =
            System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "BrightnessControl", "app.log");

        public static void Write(string msg)
        {
            try
            {
                System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
                FileInfo fi = new FileInfo(Path);
                if (fi.Exists && fi.Length > 256 * 1024) File.Delete(Path);
                File.AppendAllText(Path, DateTime.Now.ToString("HH:mm:ss.fff") + "  " + msg + Environment.NewLine);
            }
            catch { }
        }
    }

    internal sealed class TrayApp : ApplicationContext
    {
        private readonly NotifyIcon _icon;
        private readonly QuickPanel _panel;
        private readonly GlobalHooks _hooks;
        private readonly QuickSettingsCard _qsCard;
        private readonly QuickSettingsWatcher _qsWatcher;
        private readonly Thread _worker;
        private readonly string _configPath;
        private readonly string _startupVbs;
        private readonly EventWaitHandle _showEvent =
            new EventWaitHandle(false, EventResetMode.AutoReset, "Global\\BrightnessControlShow");

        private volatile bool _running = true;
        private volatile bool _linked = true;
        private volatile bool _intercept = false;
        private volatile int _pendingInternal = -1;
        private volatile int _pendingExternal = -1;
        private volatile int _cachedInternal = 75;
        private volatile int _cachedExternal = 75;
        private volatile bool _refreshExternal;
        private volatile bool _refreshNow;

        private readonly System.Windows.Forms.Timer _raiseTimer;
        // Lets the UI wake the worker immediately instead of polling a short sleep.
        private readonly AutoResetEvent _wake = new AutoResetEvent(false);
        private ToolStripMenuItem _menuLink;
        private ToolStripMenuItem _menuIntercept;

        public TrayApp()
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BrightnessControl");
            Directory.CreateDirectory(dir);
            _configPath = Path.Combine(dir, "config.ini");
            _startupVbs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Startup), "brightness-control.vbs");
            LoadConfig();

            _panel = new QuickPanel(Display.ExternalName());
            _panel.SetLinkState(_linked);
            _panel.LaptopChanged += OnLaptopChanged;
            _panel.ExternalChanged += OnExternalChanged;
            _panel.LinkChanged += OnLinkChanged;
            _panel.OpenNativeRequested += OpenNativeQuickSettings;

            _raiseTimer = new System.Windows.Forms.Timer();
            _raiseTimer.Interval = 500;
            _raiseTimer.Tick += delegate { RaisePanel(); };

            _icon = new NotifyIcon();
            _icon.Icon = MakeIcon();
            _icon.Text = "Brightness Control";
            _icon.Visible = true;
            _icon.MouseClick += delegate(object s, MouseEventArgs e)
            {
                if (e.Button == MouseButtons.Left) TogglePanel();
            };
            _icon.ContextMenuStrip = BuildMenu();

            _qsCard = new QuickSettingsCard(Display.ExternalName());
            _qsCard.ValueChanged += OnCardValue;
            _qsWatcher = new QuickSettingsWatcher(_qsCard, OnCardValue, delegate { return _cachedExternal; });
            // Force handle creation on the UI thread; the watcher marshals onto it.
            IntPtr qsHandle = _qsCard.Handle;
            _qsWatcher.Start();

            _hooks = new GlobalHooks(_intercept);
            _hooks.InterceptWinA = _intercept;
            Log.Write("hooks: keyboard=" + _hooks.KeyboardHookOk + " mouse=" + _hooks.MouseHookOk + " intercept=" + _intercept);
            _hooks.OnWinA = delegate
            {
                Log.Write("Win+A intercepted");
                try { _panel.BeginInvoke(new Action(TogglePanel)); } catch (Exception ex) { Log.Write("winA marshal failed: " + ex.Message); }
            };
            _hooks.OnMouseDown = delegate(Point pt)
            {
                if (!_panel.Visible) return;
                if (_panel.ContainsPoint(pt)) return;
                try { _panel.BeginInvoke(new Action(HidePanel)); } catch { }
            };

            _worker = new Thread(WorkerLoop);
            _worker.IsBackground = true;
            _worker.Start();

            IntPtr unused = _panel.Handle;
            Thread showWaiter = new Thread(delegate()
            {
                while (_running)
                {
                    try
                    {
                        if (_showEvent.WaitOne(500)) _panel.BeginInvoke(new Action(ShowPanel));
                    }
                    catch { }
                }
            });
            showWaiter.IsBackground = true;
            showWaiter.Start();
        }

        private ContextMenuStrip BuildMenu()
        {
            ContextMenuStrip menu = new ContextMenuStrip();
            menu.Items.Add("Brightness panel", null, delegate { TogglePanel(); });

            _menuLink = new ToolStripMenuItem("Link both monitors");
            _menuLink.Checked = _linked;
            _menuLink.CheckOnClick = true;
            _menuLink.CheckedChanged += delegate { SetLinked(_menuLink.Checked); };
            menu.Items.Add(_menuLink);

            _menuIntercept = new ToolStripMenuItem("Use Win+A for this panel");
            _menuIntercept.Checked = _intercept;
            _menuIntercept.CheckOnClick = true;
            _menuIntercept.CheckedChanged += delegate
            {
                _intercept = _menuIntercept.Checked;
                _hooks.InterceptWinA = _intercept;
                SaveConfig();
            };
            menu.Items.Add(_menuIntercept);

            ToolStripMenuItem auto = new ToolStripMenuItem("Start with Windows");
            auto.Checked = File.Exists(_startupVbs);
            auto.CheckOnClick = true;
            auto.CheckedChanged += delegate(object s, EventArgs e)
            {
                try
                {
                    if (((ToolStripMenuItem)s).Checked) WriteStartupEntry();
                    else if (File.Exists(_startupVbs)) File.Delete(_startupVbs);
                }
                catch { }
            };
            menu.Items.Add(auto);

            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Exit", null, delegate { ExitApp(); });
            return menu;
        }

        private void ShowPanel()
        {
            if (_panel.Visible)
            {
                _panel.Activate();
                return;
            }
            try
            {
                _refreshExternal = true;
                _refreshNow = true;
                _panel.UpdateValues(_cachedInternal, _cachedExternal);
                _panel.ShowAtTopRight();
                RaisePanel();
                _raiseTimer.Start();
                Log.Write("panel shown: visible=" + _panel.Visible + " bounds=" + _panel.Bounds);
            }
            catch (Exception ex) { Log.Write("show failed: " + ex); }
        }

        private void HidePanel()
        {
            Log.Write("panel hidden");
            _raiseTimer.Stop();
            _panel.Hide();
            _refreshExternal = false;
        }

        private void TogglePanel()
        {
            if (_panel.Visible) HidePanel();
            else ShowPanel();
        }

        /// <summary>Drag on the card attached under Quick Settings.</summary>
        private void OnCardValue(int v)
        {
            _cachedExternal = v;
            _pendingExternal = v;
            if (_linked)
            {
                _cachedInternal = v;
                _pendingInternal = v;
                if (_panel.Visible) _panel.UpdateValues(v, v);
            }
            else if (_panel.Visible) _panel.UpdateValues(-1, v);
            _wake.Set();
        }

        private void OnLaptopChanged(int v)
        {
            _cachedInternal = v;
            _pendingInternal = v;
            _wake.Set();
            if (_linked)
            {
                _pendingExternal = v;
                _panel.UpdateValues(-1, v);
            }
        }

        private void OnExternalChanged(int v)
        {
            _cachedExternal = v;
            _pendingExternal = v;
            _wake.Set();
            if (_linked)
            {
                _pendingInternal = v;
                _panel.UpdateValues(v, -1);
            }
        }

        private void OnLinkChanged(bool linked)
        {
            if (_menuLink != null) _menuLink.Checked = linked;
            SetLinked(linked);
        }

        private void SetLinked(bool linked)
        {
            _linked = linked;
            SaveConfig();
            if (linked) _pendingExternal = _cachedInternal;
            _wake.Set();
        }

        [DllImport("user32.dll")]
        private static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

        private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
        private const uint SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOACTIVATE = 0x0010;

        /// <summary>
        /// The shell's own flyouts (media card) live in a higher compositor band, so re-assert
        /// topmost while the panel is open instead of trusting a single BringToFront.
        /// </summary>
        private void RaisePanel()
        {
            if (!_panel.Visible) return;
            SetWindowPos(_panel.Handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }

        /// <summary>Hand Win+A back to the shell for one press, then re-arm the hook.</summary>
        private void OpenNativeQuickSettings()
        {
            HidePanel();
            _hooks.InterceptWinA = false;
            keybd_event(0x5B, 0, 0, UIntPtr.Zero);
            Thread.Sleep(80);
            keybd_event(0x41, 0, 0, UIntPtr.Zero);
            Thread.Sleep(80);
            keybd_event(0x41, 0, 2, UIntPtr.Zero);
            Thread.Sleep(40);
            keybd_event(0x5B, 0, 2, UIntPtr.Zero);

            System.Windows.Forms.Timer t = new System.Windows.Forms.Timer();
            t.Interval = 500;
            t.Tick += delegate
            {
                t.Stop();
                t.Dispose();
                _hooks.InterceptWinA = _intercept;
            };
            t.Start();
        }

        private void WorkerLoop()
        {
            Stopwatch clock = Stopwatch.StartNew();
            long lastPoll = -1000;
            long burstUntil = 0;

            try
            {
                int inner = Display.ReadInternal();
                if (inner >= 0) _cachedInternal = inner;
                int outer = Display.ReadExternal();
                if (outer >= 0) _cachedExternal = outer;
                if (_linked) _pendingExternal = _cachedInternal;
            }
            catch { }

            while (_running)
            {
                bool active = false;
                try
                {
                    int pi = _pendingInternal;
                    if (pi >= 0)
                    {
                        _pendingInternal = -1;
                        Display.WriteInternal(pi);
                        _cachedInternal = pi;
                        active = true;
                    }

                    int pe = _pendingExternal;
                    if (pe >= 0)
                    {
                        _pendingExternal = -1;
                        Display.WriteExternal(pe);
                        _cachedExternal = pe;
                        if (_panel.Visible) _panel.UpdateValues(-1, pe);
                        active = true;
                    }

                    long now = clock.ElapsedMilliseconds;
                    long interval = now < burstUntil ? 120 : 400;

                    if (now - lastPoll >= interval)
                    {
                        lastPoll = now;
                        int cur = Display.ReadInternal();
                        if (cur >= 0 && cur != _cachedInternal)
                        {
                            _cachedInternal = cur;
                            if (_panel.Visible) _panel.UpdateValues(cur, -1);
                            if (_linked) _pendingExternal = cur;
                            burstUntil = now + 1500;
                            active = true;
                        }
                    }

                    if (_refreshExternal && (_refreshNow || now - lastPoll >= 400))
                    {
                        _refreshNow = false;
                        int ext = Display.ReadExternal();
                        if (ext >= 0 && ext != _cachedExternal)
                        {
                            _cachedExternal = ext;
                            if (_panel.Visible) _panel.UpdateValues(-1, ext);
                        }
                    }
                }
                catch { }

                // 30 ms while work is in flight, otherwise wait for the next poll tick or a
                // UI nudge. Idle wakeups drop from ~11/s to 2.5/s, which matters on battery.
                _wake.WaitOne(active ? 30 : 400);
            }
        }

        private void LoadConfig()
        {
            _linked = true;
            _intercept = false;
            try
            {
                if (!File.Exists(_configPath))
                {
                    SaveConfig();
                    return;
                }
                foreach (string raw in File.ReadAllLines(_configPath))
                {
                    string line = raw.Trim();
                    if (line.Length == 0 || line.StartsWith("#")) continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = line.Substring(0, eq).Trim().ToLowerInvariant();
                    string v = line.Substring(eq + 1).Trim().ToLowerInvariant();
                    bool on = !(v == "0" || v == "false");
                    if (k == "linked") _linked = on;
                    else if (k == "intercept_wina") _intercept = on;
                }
            }
            catch { }
        }

        private void SaveConfig()
        {
            try
            {
                File.WriteAllText(_configPath,
                    "# BrightnessControl\r\n" +
                    "# linked=1           mirror the laptop panel brightness onto external DDC/CI monitors\r\n" +
                    "# intercept_wina=1   Win+A opens this panel instead of the Windows Quick Settings\r\n" +
                    "linked=" + (_linked ? "1" : "0") + "\r\n" +
                    "intercept_wina=" + (_intercept ? "1" : "0") + "\r\n");
            }
            catch { }
        }

        private void WriteStartupEntry()
        {
            string exe = Application.ExecutablePath;
            File.WriteAllText(_startupVbs,
                "' Starts Brightness Control at logon.\r\n" +
                "Set sh = CreateObject(\"WScript.Shell\")\r\n" +
                "sh.Run \"\"\"" + exe + "\"\"\", 0, False\r\n");
        }

        private void ExitApp()
        {
            _running = false;
            _qsWatcher.Stop();
            _hooks.Dispose();
            _icon.Visible = false;
            _icon.Dispose();
            Application.Exit();
        }

        private static Icon MakeIcon()
        {
            using (Bitmap bmp = new Bitmap(32, 32))
            {
                using (Graphics g = Graphics.FromImage(bmp))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.Clear(Color.Transparent);
                    Color c = Color.FromArgb(255, 210, 90);
                    using (SolidBrush b = new SolidBrush(c))
                    {
                        g.FillEllipse(b, 11, 11, 10, 10);
                        using (Pen p = new Pen(c, 2.2f))
                        {
                            p.StartCap = LineCap.Round;
                            p.EndCap = LineCap.Round;
                            for (int i = 0; i < 8; i++)
                            {
                                double a = i * Math.PI / 4.0;
                                g.DrawLine(p,
                                    (float)(16 + 8.5 * Math.Cos(a)), (float)(16 + 8.5 * Math.Sin(a)),
                                    (float)(16 + 12.5 * Math.Cos(a)), (float)(16 + 12.5 * Math.Sin(a)));
                            }
                        }
                    }
                }
                IntPtr h = bmp.GetHicon();
                using (Icon tmp = Icon.FromHandle(h)) return (Icon)tmp.Clone();
            }
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            if (args.Length > 0 && args[0] == "--show")
            {
                try
                {
                    using (EventWaitHandle e = EventWaitHandle.OpenExisting("Global\\BrightnessControlShow")) e.Set();
                }
                catch { }
                return;
            }

            bool created;
            using (Mutex mutex = new Mutex(true, "Global\\BrightnessControl", out created))
            {
                if (!created) return;
                try { Display.SetProcessDPIAware(); } catch { }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new TrayApp());
            }
        }
    }
}
