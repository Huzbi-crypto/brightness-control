using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace BrightnessControl
{
    /// <summary>A Windows 11 style brightness slider: rounded track, accent fill, round knob.</summary>
    internal sealed class SliderRow : Control
    {
        private int _value = 50;
        private bool _dragging;

        public event EventHandler ValueChanged;

        public static Color TrackColor = Color.FromArgb(82, 82, 82);
        public static Color FillColor = Color.FromArgb(76, 194, 255);
        public static Color KnobColor = Color.FromArgb(240, 240, 240);

        private const int Pad = 11;
        private const int TrackH = 5;
        private const int KnobR = 9;

        public int Value
        {
            get { return _value; }
            set
            {
                int v = Math.Max(0, Math.Min(100, value));
                if (v == _value) return;
                _value = v;
                Invalidate();
                if (ValueChanged != null) ValueChanged(this, EventArgs.Empty);
            }
        }

        public void SetValueSilently(int value)
        {
            int v = Math.Max(0, Math.Min(100, value));
            if (v == _value) return;
            _value = v;
            Invalidate();
        }

        public SliderRow()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Height = 26;
            Cursor = Cursors.Hand;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;

            int left = Pad, right = Math.Max(Pad + 1, Width - Pad);
            int cy = Height / 2;

            using (Pen p = new Pen(TrackColor, TrackH))
            {
                p.StartCap = LineCap.Round; p.EndCap = LineCap.Round;
                g.DrawLine(p, left, cy, right, cy);
            }

            int kx = left + (int)Math.Round((right - left) * (_value / 100.0));
            if (kx > left + 3)
            {
                using (Pen p = new Pen(FillColor, TrackH))
                {
                    p.StartCap = LineCap.Round; p.EndCap = LineCap.Round;
                    g.DrawLine(p, left, cy, kx, cy);
                }
            }

            Rectangle knob = new Rectangle(kx - KnobR, cy - KnobR, KnobR * 2, KnobR * 2);
            using (SolidBrush b = new SolidBrush(KnobColor)) g.FillEllipse(b, knob);
            if (_dragging)
            {
                using (Pen p = new Pen(FillColor, 2f)) g.DrawEllipse(p, knob);
            }
        }

        private void SetFromMouse(int x)
        {
            int left = Pad, right = Math.Max(Pad + 1, Width - Pad);
            double t = (x - left) / (double)(right - left);
            Value = (int)Math.Round(Math.Max(0, Math.Min(1, t)) * 100);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            _dragging = true;
            Capture = true;
            SetFromMouse(e.X);
            Invalidate();
            base.OnMouseDown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_dragging) SetFromMouse(e.X);
            base.OnMouseMove(e);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            _dragging = false;
            Capture = false;
            Invalidate();
            base.OnMouseUp(e);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            Value += e.Delta > 0 ? 5 : -5;
            base.OnMouseWheel(e);
        }
    }

    /// <summary>Small pill toggle switch.</summary>
    internal sealed class ToggleSwitch : Control
    {
        private bool _checked;

        public event EventHandler CheckedChanged;

        public bool Checked
        {
            get { return _checked; }
            set
            {
                if (_checked == value) return;
                _checked = value;
                Invalidate();
                if (CheckedChanged != null) CheckedChanged(this, EventArgs.Empty);
            }
        }

        public ToggleSwitch()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Size = new Size(38, 20);
            Cursor = Cursors.Hand;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            int h = Math.Min(Height, 20);
            int w = Math.Min(Width, 38);
            Rectangle track = new Rectangle(0, (Height - h) / 2, w, h);
            using (GraphicsPath path = Round(track, h / 2))
            {
                using (SolidBrush b = new SolidBrush(_checked ? SliderRow.FillColor : Color.FromArgb(92, 92, 92)))
                    g.FillPath(b, path);
            }
            int d = h - 6;
            int x = _checked ? track.Right - d - 3 : track.Left + 3;
            using (SolidBrush b = new SolidBrush(_checked ? Color.FromArgb(20, 20, 20) : Color.FromArgb(230, 230, 230)))
                g.FillEllipse(b, x, track.Top + 3, d, d);
        }

        private static GraphicsPath Round(Rectangle r, int radius)
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

        protected override void OnMouseDown(MouseEventArgs e)
        {
            Checked = !Checked;
            base.OnMouseDown(e);
        }
    }

    /// <summary>Flat text link with a hover highlight, used for the footer action.</summary>
    internal sealed class FlatLink : Label
    {
        public Color Normal = Color.FromArgb(168, 168, 168);
        public Color Hover = Color.FromArgb(76, 194, 255);

        public FlatLink()
        {
            AutoSize = false;
            BackColor = Color.Transparent;
            ForeColor = Normal;
            TextAlign = ContentAlignment.MiddleRight;
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e)
        {
            ForeColor = Hover;
            base.OnMouseEnter(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            ForeColor = Normal;
            base.OnMouseLeave(e);
        }
    }
}
