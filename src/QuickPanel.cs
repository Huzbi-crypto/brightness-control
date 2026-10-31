using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace BrightnessControl
{
    /// <summary>
    /// The panel shown for Win+A (and from the tray icon). Styled to sit where the
    /// Windows Quick Settings card sits, with a brightness row per display.
    /// </summary>
    internal sealed class QuickPanel : Form
    {
        public event Action<int> LaptopChanged;
        public event Action<int> ExternalChanged;
        public event Action<bool> LinkChanged;
        public event Action OpenNativeRequested;

        private readonly SliderRow _sliderLaptop = new SliderRow();
        private readonly SliderRow _sliderExt = new SliderRow();
        private readonly ToggleSwitch _toggleLink = new ToggleSwitch();
        private readonly Label _lblHeader = new Label();
        private readonly Label _lblLaptop = new Label();
        private readonly Label _lblLaptopPct = new Label();
        private readonly Label _lblExt = new Label();
        private readonly Label _lblExtPct = new Label();
        private readonly Label _lblLink = new Label();
        private readonly FlatLink _lnkNative = new FlatLink();

        private bool _suppress;

        private static readonly Color Bg = Color.FromArgb(40, 42, 44);
        private static readonly Color Border = Color.FromArgb(60, 60, 60);
        private static readonly Color Fg = Color.FromArgb(255, 255, 255);
        private static readonly Color Muted = Color.FromArgb(168, 168, 168);
        private static readonly Color Divider = Color.FromArgb(52, 52, 52);

        public const int PanelWidth = 380;
        public const int PanelHeight = 246;

        public QuickPanel(string externalName)
        {
            Text = "Brightness Control";
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            TopMost = true;
            StartPosition = FormStartPosition.Manual;
            AutoScaleMode = AutoScaleMode.None;
            Font = UiFont(9f);
            BackColor = Bg;
            ClientSize = new Size(PanelWidth, PanelHeight);
            DoubleBuffered = true;
            KeyPreview = true;

            StyleLabel(_lblHeader, "Brightness", Muted, ContentAlignment.MiddleLeft);
            _lblHeader.Font = UiFont(9f);

            StyleLabel(_lblLaptop, "Laptop display", Muted, ContentAlignment.MiddleLeft);
            StyleLabel(_lblExt, externalName, Muted, ContentAlignment.MiddleLeft);
            StyleLabel(_lblLaptopPct, "0%", Fg, ContentAlignment.MiddleRight);
            StyleLabel(_lblExtPct, "0%", Fg, ContentAlignment.MiddleRight);
            StyleLabel(_lblLink, "Link both monitors", Fg, ContentAlignment.MiddleLeft);

            _lblLink.Cursor = Cursors.Hand;
            _lnkNative.Text = "Windows quick settings  \u203A";
            _lnkNative.Font = UiFont(9f);
            _lnkNative.Click += delegate { if (OpenNativeRequested != null) OpenNativeRequested(); };

            _sliderLaptop.ValueChanged += delegate
            {
                _lblLaptopPct.Text = _sliderLaptop.Value + "%";
                if (_suppress) return;
                if (LaptopChanged != null) LaptopChanged(_sliderLaptop.Value);
            };
            _sliderExt.ValueChanged += delegate
            {
                _lblExtPct.Text = _sliderExt.Value + "%";
                if (_suppress) return;
                if (ExternalChanged != null) ExternalChanged(_sliderExt.Value);
            };
            _toggleLink.CheckedChanged += delegate
            {
                if (LinkChanged != null) LinkChanged(_toggleLink.Checked);
            };
            _lblLink.Click += delegate { _toggleLink.Checked = !_toggleLink.Checked; };

            Controls.AddRange(new Control[]
            {
                _lblHeader, _lblLaptop, _lblLaptopPct, _sliderLaptop,
                _lblExt, _lblExtPct, _sliderExt, _toggleLink, _lblLink, _lnkNative
            });

            LayoutControls();
        }

        /// <summary>Segoe UI Variable is the Windows 11 UI font; fall back on older builds.</summary>
        internal static Font UiFont(float size)
        {
            Font f = new Font("Segoe UI Variable Text", size);
            if (f.Name == "Segoe UI Variable Text") return f;
            f.Dispose();
            return new Font("Segoe UI", size);
        }

        private void StyleLabel(Label l, string text, Color color, ContentAlignment align)
        {
            l.Text = text;
            l.ForeColor = color;
            l.BackColor = Color.Transparent;
            l.TextAlign = align;
            l.AutoSize = false;
            l.Font = UiFont(10f);
        }

        private void LayoutControls()
        {
            const int pad = 20;
            int w = ClientSize.Width - pad * 2;
            int y = 14;
            int labelH = 20;

            _lblHeader.SetBounds(pad, y, w, 18);
            y = 40;

            _lblLaptop.SetBounds(pad, y, w - 60, labelH);
            _lblLaptopPct.SetBounds(pad + w - 60, y, 60, labelH);
            y += labelH;
            _sliderLaptop.SetBounds(pad - 11, y, w + 22, 26);
            y += 26 + 16;

            _lblExt.SetBounds(pad, y, w - 60, labelH);
            _lblExtPct.SetBounds(pad + w - 60, y, 60, labelH);
            y += labelH;
            _sliderExt.SetBounds(pad - 11, y, w + 22, 26);
            y += 26 + 18;

            _toggleLink.SetBounds(pad, y + 1, 38, 20);
            _lblLink.SetBounds(pad + 48, y - 2, w - 48, 24);

            _lnkNative.SetBounds(pad, ClientSize.Height - 38, w, 22);
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= 0x20000; // CS_DROPSHADOW
                return cp;
            }
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            using (GraphicsPath p = Rounded(ClientRectangle, 12)) Region = new Region(p);
            LayoutControls();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (GraphicsPath p = Rounded(new Rectangle(0, 0, ClientSize.Width - 1, ClientSize.Height - 1), 12))
            {
                using (SolidBrush b = new SolidBrush(Bg)) g.FillPath(b, p);
                using (Pen pen = new Pen(Border, 1f)) g.DrawPath(pen, p);
            }

            int y = _toggleLink.Top - 18;
            using (Pen pen = new Pen(Divider, 1f))
                g.DrawLine(pen, 20, y, ClientSize.Width - 20, y);
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

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Escape) Hide();
            base.OnKeyDown(e);
        }

        public void SetLinkState(bool linked)
        {
            _toggleLink.Checked = linked;
        }

        public void UpdateValues(int inner, int outer)
        {
            if (InvokeRequired)
            {
                try { BeginInvoke(new Action<int, int>(UpdateValues), inner, outer); } catch { }
                return;
            }
            _suppress = true;
            if (inner >= 0)
            {
                _sliderLaptop.SetValueSilently(inner);
                _lblLaptopPct.Text = inner + "%";
            }
            if (outer >= 0)
            {
                _sliderExt.SetValueSilently(outer);
                _lblExtPct.Text = outer + "%";
            }
            _suppress = false;
        }

        public void ShowAtTopRight()
        {
            Rectangle wa = Screen.PrimaryScreen.WorkingArea;
            int top = 48; // clear of the 40px taskbar
            Location = new Point(wa.Right - Width - 12, top);
            Show();
            Activate();
            BringToFront();
        }

        public bool ContainsPoint(Point screen)
        {
            return Visible && Bounds.Contains(screen);
        }
    }
}
