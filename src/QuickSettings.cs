using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace BrightnessControl
{
    /// <summary>
    /// A small card that attaches itself under the Windows Quick Settings panel, showing the
    /// external monitor's brightness as one more row in the same stack.
    ///
    /// Why it is built this way:
    /// - The Control Center is a ShellHost window in a higher DWM band. Nothing can be drawn
    ///   over the card itself, but its container window is 480 px wide and ~1400 px tall with a
    ///   transparent area below the card, and a normal topmost window does show through there.
    /// - The container cannot be found with EnumWindows/FindWindow (the shell creates it in a
    ///   band), so it is located with WindowFromPoint and identified by class name.
    /// - The card's own bounds are not exposed (no UIA provider at all), so the bottom edge is
    ///   measured from a screenshot by comparing a column inside the card against a column in
    ///   the container's padding at the same row.
    /// </summary>
    internal sealed class QuickSettingsCard : Form
    {
        public event Action<int> ValueChanged;

        private readonly SliderRow _slider = new SliderRow();
        private readonly Label _label = new Label();
        private readonly Label _pct = new Label();
        private bool _suppress;

        private static readonly Color CardBg = Color.FromArgb(40, 42, 44); // sampled from the Quick Settings card
        private static readonly Color Muted = Color.FromArgb(180, 180, 180);
        private static readonly Color Fg = Color.FromArgb(255, 255, 255);

        public const int CardWidth = 450;
        public const int CardHeight = 74;
        public const int Gap = 3;

        public QuickSettingsCard(string monitorName)
        {
            Text = "Brightness Control";
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            TopMost = true;
            StartPosition = FormStartPosition.Manual;
            AutoScaleMode = AutoScaleMode.None;
            BackColor = CardBg;
            ClientSize = new Size(CardWidth, CardHeight);
            DoubleBuffered = true;

            _label.Text = monitorName;
            _label.ForeColor = Muted;
            _label.BackColor = Color.Transparent;
            _label.AutoSize = false;
            _label.TextAlign = ContentAlignment.MiddleLeft;
            _label.Font = QuickPanel.UiFont(10f);

            _pct.Text = "";
            _pct.ForeColor = Fg;
            _pct.BackColor = Color.Transparent;
            _pct.AutoSize = false;
            _pct.TextAlign = ContentAlignment.MiddleRight;
            _pct.Font = _label.Font;

            _slider.ValueChanged += delegate
            {
                _pct.Text = _slider.Value + "%";
                if (_suppress) return;
                if (ValueChanged != null) ValueChanged(_slider.Value);
            };

            Controls.AddRange(new Control[] { _label, _pct, _slider });

            _label.SetBounds(44, 12, CardWidth - 100, 18);
            _pct.SetBounds(CardWidth - 92, 12, 74, 18);
            _slider.SetBounds(25, 34, 361, 26);
        }

        protected override bool ShowWithoutActivation { get { return true; } }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ExStyle |= 0x08000000; // WS_EX_NOACTIVATE - never steal focus from the flyout
                cp.ExStyle |= 0x00000080; // WS_EX_TOOLWINDOW
                cp.ExStyle |= 0x00000008; // WS_EX_TOPMOST
                return cp;
            }
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            using (GraphicsPath p = Rounded(ClientRectangle, 8)) Region = new Region(p);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (GraphicsPath p = Rounded(new Rectangle(0, 0, ClientSize.Width - 1, ClientSize.Height - 1), 8))
            {
                using (SolidBrush b = new SolidBrush(CardBg)) g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(56, 58, 60), 1f)) g.DrawPath(pen, p);
            }

            // leading monitor glyph, matching the sun/speaker icons on the native rows
            using (Pen pen = new Pen(Color.FromArgb(200, 200, 200), 1.4f))
            {
                g.DrawRectangle(pen, 19f, 43f, 15f, 10f);
                g.DrawLine(pen, 26.5f, 53f, 26.5f, 56f);
                g.DrawLine(pen, 22f, 56.5f, 31f, 56.5f);
            }
        }

        private static GraphicsPath Rounded(Rectangle r, int radius)
        {
            GraphicsPath p = new GraphicsPath();
            int d = radius * 2;
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public void SetValueSilently(int v)
        {
            _suppress = true;
            _slider.SetValueSilently(v);
            _pct.Text = v + "%";
            _suppress = false;
        }
    }

    /// <summary>Finds the Quick Settings container, measures the card and parks the card below it.</summary>
    internal sealed class QuickSettingsWatcher
    {
        private const string WindowClass = "ControlCenterWindow";

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT { public int x; public int y; }

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int L, T, R, B; }

        [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(POINT p);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr h, StringBuilder s, int n);
        [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);

        private readonly QuickSettingsCard _card;
        private readonly Action<int> _onUserChange;
        private readonly Func<int> _readExternal;
        private Thread _thread;
        private volatile bool _running = true;

        private IntPtr _lastHwnd = IntPtr.Zero;
        private Rectangle _lastBounds = Rectangle.Empty;
        private long _lastMeasure;
        private volatile int _pendingValue = -1;

        public QuickSettingsWatcher(QuickSettingsCard card, Action<int> onUserChange, Func<int> readExternal)
        {
            _card = card;
            _onUserChange = onUserChange;
            _readExternal = readExternal;
        }

        public void Start()
        {
            _thread = new Thread(Loop);
            _thread.IsBackground = true;
            _thread.Start();
        }

        public void Stop() { _running = false; }

        /// <summary>Called from the UI thread when the user drags our slider.</summary>
        public void OnCardValue(int v) { _onUserChange(v); }

        private void Loop()
        {
            while (_running)
            {
                try { Tick(); }
                catch { }
                Thread.Sleep(160);
            }
        }

        private static IntPtr FindContainer()
        {
            // A point inside the flyout, above where our card sits.
            POINT p = new POINT();
            p.x = 2400; p.y = 300;
            IntPtr h = WindowFromPoint(p);
            if (h == IntPtr.Zero) return IntPtr.Zero;

            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (sb.ToString() == WindowClass) return h;

            // Fallback probes in case the panel is taller/shorter than expected.
            int[] ys = new int[] { 200, 420, 620, 820 };
            foreach (int y in ys)
            {
                p.y = y;
                h = WindowFromPoint(p);
                if (h == IntPtr.Zero) continue;
                sb.Length = 0;
                GetClassName(h, sb, 64);
                if (sb.ToString() == WindowClass) return h;
            }
            return IntPtr.Zero;
        }

        private bool _wasOpen;
        private Rectangle _lastLogged = Rectangle.Empty;
        private bool _loggedShow;
        private Rectangle _ourBounds = Rectangle.Empty;
        private Rectangle _candidate = Rectangle.Empty;
        private int _candidateCount;
        private int _firstSeen;

        private void Tick()
        {
            IntPtr hwnd = FindContainer();
            if (hwnd == IntPtr.Zero)
            {
                if (_wasOpen) { Log.Write("qs: container gone"); _wasOpen = false; }
                if (_card.Visible) { HideCard(); _loggedShow = false; _ourBounds = Rectangle.Empty; }
                _lastHwnd = IntPtr.Zero;
                _lastBounds = Rectangle.Empty;
                return;
            }

            if (!_wasOpen)
            {
                Log.Write("qs: container found hwnd=0x" + hwnd.ToInt64().ToString("X"));
                _wasOpen = true;
                _firstSeen = Environment.TickCount;
                _candidate = Rectangle.Empty;
                _candidateCount = 0;
                _lastBounds = Rectangle.Empty;
            }
            // the flyout animates in; measuring mid-animation yields a too-small card
            if (Environment.TickCount - _firstSeen < 320) return;

            RECT r;
            if (!GetWindowRect(hwnd, out r)) { Log.Write("qs: GetWindowRect failed"); return; }
            Rectangle container = Rectangle.FromLTRB(r.L, r.T, r.R, r.B);

            bool remeasure = (hwnd != _lastHwnd) || (Environment.TickCount - _lastMeasure > 900);
            if (!remeasure && _lastBounds != Rectangle.Empty)
            {
                PlaceCard(_lastBounds);
                return;
            }

            Rectangle bounds = MeasureCard(container, _ourBounds);
            if (bounds == Rectangle.Empty) { Log.Write("qs: measure failed container=" + container); return; }

            // Only trust a value we have seen twice in a row.
            if (bounds == _candidate) _candidateCount++;
            else { _candidate = bounds; _candidateCount = 1; }
            if (_candidateCount < 2) return;

            if (_card.Visible && _ourBounds != Rectangle.Empty && bounds.Y >= _ourBounds.Top - 2)
            {
                // The panel grew under us (e.g. a media card appeared). Drop the card and
                // re-measure from the bottom of the container on the next tick.
                Log.Write("qs: overlap detected, re-measuring");
                HideCard();
                _ourBounds = Rectangle.Empty;
                _candidate = Rectangle.Empty;
                _candidateCount = 0;
                _lastMeasure = 0;
                return;
            }

            if (bounds != _lastLogged) { Log.Write("qs: card measured " + bounds); _lastLogged = bounds; }
            _lastHwnd = hwnd;
            _lastBounds = bounds;
            _lastMeasure = Environment.TickCount;
            PlaceCard(bounds);
        }

        /// <summary>
        /// The card is inset 15 px from the container on the left, right and top. Its bottom edge
        /// is found by comparing a column inside the card with a column in the container's
        /// padding on the same row: below the card both show whatever is behind the flyout.
        /// </summary>
        private static Rectangle MeasureCard(Rectangle container, Rectangle skip)
        {
            if (container.Width <= 0 || container.Height <= 0) return Rectangle.Empty;
            int inset = 15;
            int left = container.Left + inset;
            int width = container.Width - inset * 2;

            using (Bitmap strip = new Bitmap(container.Width, container.Height))
            {
                using (Graphics g = Graphics.FromImage(strip))
                    g.CopyFromScreen(container.Left, container.Top, 0, 0, container.Size);

                int midX = container.Width / 2;
                int padX = 8; // inside the container, left of the card

                // Stop just above our own card: everything below it is our own pixels or empty
                // background, and including it makes the scan measure itself.
                int startY = container.Height - 1;
                if (skip != Rectangle.Empty && skip.Top > container.Top)
                    startY = Math.Min(startY, skip.Top - container.Top - 1);

                int bottom = -1;
                for (int y = startY; y > inset; y--)
                {
                    Color a = strip.GetPixel(midX, y);
                    Color b = strip.GetPixel(padX, y);
                    int diff = Math.Abs(a.R - b.R) + Math.Abs(a.G - b.G) + Math.Abs(a.B - b.B);
                    if (diff > 8)
                    {
                        // require a few consistent rows so a stray shadow pixel does not win
                        int run = 0;
                        for (int k = 0; k < 6 && y - k > 0; k++)
                        {
                            Color a2 = strip.GetPixel(midX, y - k);
                            Color b2 = strip.GetPixel(padX, y - k);
                            if (Math.Abs(a2.R - b2.R) + Math.Abs(a2.G - b2.G) + Math.Abs(a2.B - b2.B) > 8) run++;
                        }
                        if (run >= 5) { bottom = y; break; }
                    }
                }

                if (bottom < 0) return Rectangle.Empty;
                // 'bottom' is a row inside the captured strip; convert back to screen space.
                return new Rectangle(left, container.Top + bottom + 1, width, QuickSettingsCard.CardHeight);
            }
        }

        private void PlaceCard(Rectangle bounds)
        {
            Rectangle target = new Rectangle(bounds.X, bounds.Y + QuickSettingsCard.Gap, bounds.Width, QuickSettingsCard.CardHeight);

            try
            {
            _card.BeginInvoke(new Action(delegate
            {
                if (_card.Width != target.Width || _card.Height != target.Height)
                    _card.ClientSize = new Size(target.Width, target.Height);

                if (_card.Bounds != target) _card.SetBounds(target.X, target.Y, target.Width, target.Height);

                if (!_card.Visible)
                {
                    int v = _readExternal();
                    if (v >= 0) _card.SetValueSilently(v);
                    _card.Show();
                    SetWindowPos(_card.Handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
                    _ourBounds = target;
                    if (!_loggedShow) { Log.Write("qs: card shown at " + target); _loggedShow = true; }
                }
            }));
            }
            catch (Exception ex) { Log.Write("qs: place failed " + ex.Message); }
        }

        private void HideCard()
        {
            try { _card.BeginInvoke(new Action(delegate { _card.Hide(); })); } catch { }
        }

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

        private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
        private const uint SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOACTIVATE = 0x0010;
    }
}
