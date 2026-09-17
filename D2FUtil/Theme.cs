using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace D2Chain.D2FUtil;

/// <summary>
/// The launcher's look: a dark, gold-on-black palette in the spirit of the game's own screens, with
/// the handful of custom-drawn controls it takes to get there.
///
/// <para>WinForms has no dark mode. Setting colours on the standard controls gets most of the way, but
/// three of them paint themselves from system colours whatever you ask for — the check box glyph, the
/// spin buttons on a numeric field, and the caption bar — and a single light-grey patch is what makes
/// a dark window look broken rather than styled. Those three are handled here: the first two by small
/// owner-drawn replacements, the caption bar by asking the desktop window manager for the dark one.</para>
///
/// <para>Because the drawing is by hand it is also on us to honour the display's scaling. Fonts are in
/// points and grow by themselves; every pixel measurement goes through <see cref="Px"/>, which is why
/// the window is the same size on a 100% display and a 150% one instead of two thirds of it.</para>
/// </summary>
internal static class Theme
{
    public static readonly Color Ink      = Color.FromArgb(0x0D, 0x0B, 0x0A);   // behind everything
    public static readonly Color Card     = Color.FromArgb(0x17, 0x14, 0x11);   // a panel
    public static readonly Color Head     = Color.FromArgb(0x21, 0x1B, 0x15);   // a panel's title strip
    public static readonly Color Field    = Color.FromArgb(0x0B, 0x0A, 0x09);   // something you type in
    public static readonly Color Edge     = Color.FromArgb(0x3B, 0x31, 0x26);
    public static readonly Color EdgeLit  = Color.FromArgb(0x85, 0x69, 0x3E);   // hover or focus
    public static readonly Color Text     = Color.FromArgb(0xD2, 0xC6, 0xAC);   // parchment
    public static readonly Color Dim      = Color.FromArgb(0x8B, 0x80, 0x6C);
    public static readonly Color Gold     = Color.FromArgb(0xD6, 0xA8, 0x52);
    public static readonly Color Blood    = Color.FromArgb(0x6B, 0x1A, 0x13);
    public static readonly Color BloodLit = Color.FromArgb(0x96, 0x26, 0x1B);
    public static readonly Color Good     = Color.FromArgb(0x84, 0xAD, 0x63);
    public static readonly Color Warn     = Color.FromArgb(0xD2, 0x95, 0x38);
    public static readonly Color Bad      = Color.FromArgb(0xC3, 0x53, 0x45);

    public static readonly Font Body     = new("Segoe UI", 9.5f);
    public static readonly Font Strong   = new("Segoe UI Semibold", 9.5f);
    public static readonly Font Title    = new("Segoe UI", 8.5f, FontStyle.Bold);
    public static readonly Font Wordmark = new("Segoe UI Light", 20f);
    public static readonly Font Mono     = new("Consolas", 9f);

    /// <summary>Pixels per pixel: 1 on a normal display, 1.5 at 150%, and so on.</summary>
    public static float Scale { get; private set; } = 1f;

    public static void Init()
    {
        try
        {
            using var g = Graphics.FromHwnd(IntPtr.Zero);
            Scale = Math.Clamp(g.DpiX / 96f, 1f, 4f);
        }
        catch { Scale = 1f; }
        AllowDarkMode();
    }

    // Scroll bars are drawn by the system and ignore every colour a control is given. Windows will
    // draw them dark, but only for a process that has asked for dark mode and only on controls that
    // have been told to use the dark variant of the theme. Both calls are undocumented or
    // version-dependent, so both are optional: without them the bars are light and nothing else breaks.
    [DllImport("uxtheme.dll", EntryPoint = "#135", SetLastError = true)]
    static extern int SetPreferredAppMode(int mode);

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    static extern int SetWindowTheme(IntPtr hwnd, string? appName, string? idList);

    static void AllowDarkMode()
    {
        try { SetPreferredAppMode(2); }             // ForceDark
        catch (EntryPointNotFoundException) { }
        catch (DllNotFoundException) { }
    }

    [DllImport("user32.dll")]
    static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wp, IntPtr lp);

    public static void DarkScrollBars(Control c)
    {
        try
        {
            if (!c.IsHandleCreated) return;
            SetWindowTheme(c.Handle, "DarkMode_Explorer", null);
            SendMessage(c.Handle, 0x031A, IntPtr.Zero, IntPtr.Zero);    // WM_THEMECHANGED: redraw them
        }
        catch (DllNotFoundException) { }
    }

    public static int Px(int n) => (int)Math.Round(n * Scale);
    public static Padding Pad(int l, int t, int r, int b) => new(Px(l), Px(t), Px(r), Px(b));

    // Windows 10 1809 and later will paint the caption bar dark if asked. The attribute number changed
    // once along the way, so try the current one and fall back to the older spelling.
    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    public static void DarkCaption(IntPtr hwnd)
    {
        int on = 1;
        try
        {
            if (DwmSetWindowAttribute(hwnd, 20, ref on, sizeof(int)) != 0)
                DwmSetWindowAttribute(hwnd, 19, ref on, sizeof(int));
        }
        catch (DllNotFoundException) { /* older Windows: it keeps the light caption */ }
        catch (EntryPointNotFoundException) { }
    }

    /// <summary>A label for explanatory small print under a setting.</summary>
    public static Label Note(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Dim, BackColor = Card, Font = Body, Margin = Pad(0, 8, 0, 2),
    };

    /// <summary>The caption that sits to the left of a field.</summary>
    public static Label Cap(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Text, BackColor = Card, Font = Body, Margin = Pad(0, 8, 10, 4),
    };

    /// <summary>A plain-language unit or hint printed after a field.</summary>
    public static Label Unit(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Dim, BackColor = Card, Font = Body, Margin = Pad(4, 8, 6, 4),
    };

    public static Button Btn(string text, bool primary = false)
    {
        var b = new Button
        {
            Text = text, AutoSize = !primary, FlatStyle = FlatStyle.Flat, Font = primary ? Strong : Body,
            BackColor = primary ? Blood : Card, ForeColor = primary ? Color.FromArgb(0xF0, 0xE2, 0xC4) : Text,
            Padding = Pad(10, 5, 10, 5), Margin = Pad(8, 3, 0, 3), Cursor = Cursors.Hand,
            UseVisualStyleBackColor = false,
        };
        b.FlatAppearance.BorderColor = primary ? Gold : Edge;
        b.FlatAppearance.BorderSize = 1;
        b.FlatAppearance.MouseOverBackColor = primary ? BloodLit : Head;
        b.FlatAppearance.MouseDownBackColor = primary ? Blood : Field;
        return b;
    }

    /// <summary>
    /// Sends a wheel turn to the panel behind a control instead of letting the control act on it.
    /// A drop-down or a number that changes because the page was scrolled past it is a trap: the
    /// setting is altered without anyone meaning to and usually without anyone noticing.
    /// </summary>
    public static void ScrollBehind(Control c, int delta)
    {
        for (Control? p = c.Parent; p is not null; p = p.Parent)
        {
            if (p is not ScrollableControl { AutoScroll: true } s || !s.VerticalScroll.Visible) continue;
            int lines = SystemInformation.MouseWheelScrollLines is var n && n > 0 ? n : 3;
            int step = lines * Px(20) * Math.Sign(delta);
            int y = Math.Max(0, -s.AutoScrollPosition.Y - step);
            s.AutoScrollPosition = new Point(-s.AutoScrollPosition.X, y);
            return;
        }
    }

    public static TextBox Entry(bool multiline = false) => new()
    {
        BackColor = Field, ForeColor = Text, BorderStyle = BorderStyle.FixedSingle,
        Font = multiline ? Mono : Body, Multiline = multiline,
    };
}

/// <summary>
/// A titled panel: the dark replacement for <see cref="GroupBox"/>, whose frame and caption are drawn
/// by the system in colours that cannot be changed.
/// </summary>
internal sealed class Card : Panel
{
    readonly string _title;
    readonly int _head;

    public Card(string title)
    {
        _title = title.ToUpperInvariant();
        _head = TextRenderer.MeasureText("X", Theme.Title).Height + Theme.Px(14);
        Dock = DockStyle.Top;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        BackColor = Theme.Card;
        Padding = new Padding(Theme.Px(16), _head + Theme.Px(10), Theme.Px(16), Theme.Px(14));
        Margin = Theme.Pad(0, 0, 0, 12);
        DoubleBuffered = true;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        using (var head = new SolidBrush(Theme.Head)) g.FillRectangle(head, 1, 1, Width - 2, _head - 1);
        using (var edge = new Pen(Theme.Edge))
        {
            g.DrawLine(edge, 1, _head, Width - 2, _head);
            g.DrawRectangle(edge, 0, 0, Width - 1, Height - 1);
        }
        using (var accent = new SolidBrush(Theme.Gold)) g.FillRectangle(accent, 1, 1, Theme.Px(3), _head - 1);
        TextRenderer.DrawText(g, _title, Theme.Title,
            new Rectangle(Theme.Px(16), 0, Width - Theme.Px(32), _head),
            Theme.Gold, TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
    }
}

/// <summary>
/// One entry in the list of sections down the left-hand side. Selecting one shows that section on the
/// right, which keeps each group of settings on its own page instead of in one long scroll.
/// </summary>
internal sealed class NavItem : Control
{
    bool _hot;

    public NavItem(string text)
    {
        Text = text.ToUpperInvariant();
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        Font = Theme.Title;
        Cursor = Cursors.Hand;
        Margin = Padding.Empty;
        Height = TextRenderer.MeasureText("X", Theme.Title).Height + Theme.Px(22);
    }

    public bool Selected { get; set; }

    protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        using (var bg = new SolidBrush(Selected ? Theme.Card : _hot ? Theme.Head : Theme.Ink))
            g.FillRectangle(bg, ClientRectangle);
        if (Selected)
            using (var bar = new SolidBrush(Theme.Gold)) g.FillRectangle(bar, 0, 0, Theme.Px(3), Height);
        TextRenderer.DrawText(g, Text, Font,
            new Rectangle(Theme.Px(18), 0, Width - Theme.Px(20), Height),
            Selected ? Theme.Gold : _hot ? Theme.Text : Theme.Dim,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
    }
}

/// <summary>The wordmark across the top of the window.</summary>
internal sealed class Banner : Panel
{
    public static int Tall =>
        TextRenderer.MeasureText("X", Theme.Wordmark).Height + TextRenderer.MeasureText("X", Theme.Body).Height + Theme.Px(18);

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        using (var bg = new LinearGradientBrush(new Rectangle(0, 0, Math.Max(Width, 1), Math.Max(Height, 1)),
                   Color.FromArgb(0x24, 0x1B, 0x13), Theme.Ink, LinearGradientMode.Vertical))
            g.FillRectangle(bg, ClientRectangle);
        using (var line = new Pen(Theme.Edge)) g.DrawLine(line, 0, Height - 1, Width, Height - 1);
        using (var glow = new SolidBrush(Theme.Gold)) g.FillRectangle(glow, 0, Height - 1, Theme.Px(120), 1);

        int markHeight = TextRenderer.MeasureText("X", Theme.Wordmark).Height;
        int x = Theme.Px(18), y = Theme.Px(6);
        TextRenderer.DrawText(g, "D2F UTILITIES", Theme.Wordmark, new Point(x, y), Theme.Gold);
        TextRenderer.DrawText(g, "Diablo II 1.07 – 1.13d   ·   multiple clients, maphack, travel, potions",
            Theme.Body, new Point(x + Theme.Px(2), y + markHeight + Theme.Px(2)), Theme.Dim);
    }
}

/// <summary>
/// A check box drawn by us. The stock one paints its glyph from the system theme, so on a dark panel it
/// stays a light square however the colours are set.
/// </summary>
internal sealed class DarkCheck : CheckBox
{
    bool _hot;

    public DarkCheck(string text)
    {
        Text = text;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        AutoSize = true;
        BackColor = Theme.Card;
        ForeColor = Theme.Text;
        Font = Theme.Body;
        Cursor = Cursors.Hand;
        Margin = Theme.Pad(0, 5, 22, 5);
    }

    int BoxSize => Math.Max(Theme.Px(13), TextRenderer.MeasureText("X", Font).Height - Theme.Px(2));
    int Gap => Theme.Px(9);

    public override Size GetPreferredSize(Size proposed)
    {
        var t = TextRenderer.MeasureText(Text, Font);
        return new Size(t.Width + BoxSize + Gap + Theme.Px(4), Math.Max(BoxSize + Theme.Px(6), t.Height + Theme.Px(6)));
    }

    protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(BackColor);
        int s = BoxSize;
        var box = new Rectangle(0, (Height - s) / 2, s, s);
        using (var fill = new SolidBrush(Enabled ? Theme.Field : Theme.Card)) g.FillRectangle(fill, box);
        using (var pen = new Pen(Enabled && (_hot || Focused) ? Theme.EdgeLit : Theme.Edge)) g.DrawRectangle(pen, box);
        if (Checked)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using var tick = new Pen(Enabled ? Theme.Gold : Theme.Dim, Math.Max(2f, s / 7f));
            g.DrawLines(tick, new[]
            {
                new PointF(box.Left + s * 0.22f, box.Top + s * 0.50f),
                new PointF(box.Left + s * 0.43f, box.Top + s * 0.73f),
                new PointF(box.Left + s * 0.80f, box.Top + s * 0.25f),
            });
            g.SmoothingMode = SmoothingMode.Default;
        }
        TextRenderer.DrawText(g, Text, Font, new Rectangle(s + Gap, 0, Width - s - Gap, Height),
            Enabled ? ForeColor : Theme.Dim, TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
    }
}

/// <summary>A drop-down in the same colours, drawing its own items so the list matches the window.</summary>
internal sealed class DarkCombo : ComboBox
{
    public DarkCombo()
    {
        DropDownStyle = ComboBoxStyle.DropDownList;
        FlatStyle = FlatStyle.Flat;
        DrawMode = DrawMode.OwnerDrawFixed;
        BackColor = Theme.Field;
        ForeColor = Theme.Text;
        Font = Theme.Body;
        ItemHeight = TextRenderer.MeasureText("X", Theme.Body).Height + Theme.Px(6);
        Margin = Theme.Pad(0, 3, 8, 3);
    }

    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    // Scrolling the page must never rebind a key. The wheel moves through the list only while the list
    // is open; otherwise the turn goes to the panel behind and the selection is left alone.
    protected override void OnMouseWheel(MouseEventArgs e)
    {
        if (DroppedDown) { base.OnMouseWheel(e); return; }
        if (e is HandledMouseEventArgs h) h.Handled = true;
        Theme.ScrollBehind(this, e.Delta);
    }

    // A flat combo box still draws its own frame and drop-down button from system colours — a white
    // rectangle with a white button, which is exactly the thing that gives a dark window away. Owner
    // drawing only covers the list items, so the frame and the arrow are painted over afterwards.
    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
        const int WM_PAINT = 0x000F;
        if (m.Msg != WM_PAINT || !IsHandleCreated) return;
        using var g = Graphics.FromHwnd(Handle);
        int aw = Math.Max(Theme.Px(17), SystemInformation.VerticalScrollBarWidth);
        var button = new Rectangle(Width - aw - 1, 1, aw, Height - 2);
        using (var fill = new SolidBrush(Enabled ? Theme.Field : Theme.Card)) g.FillRectangle(fill, button);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        int cx = button.Left + button.Width / 2, cy = Height / 2, w = Theme.Px(4);
        using (var arrow = new SolidBrush(Enabled ? Theme.Gold : Theme.Edge))
            g.FillPolygon(arrow, new[]
            {
                new Point(cx - w, cy - w / 2), new Point(cx + w, cy - w / 2), new Point(cx, cy + w),
            });
        g.SmoothingMode = SmoothingMode.Default;
        using (var pen = new Pen(Focused ? Theme.EdgeLit : Theme.Edge)) g.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
    }

    protected override void OnDrawItem(DrawItemEventArgs e)
    {
        if (e.Index < 0) return;
        bool picked = (e.State & DrawItemState.Selected) != 0 && (e.State & DrawItemState.ComboBoxEdit) == 0;
        using (var bg = new SolidBrush(picked ? Theme.Head : Theme.Field)) e.Graphics.FillRectangle(bg, e.Bounds);
        var fg = !Enabled ? Theme.Dim : picked ? Theme.Gold : Theme.Text;
        var r = e.Bounds; r.X += Theme.Px(6); r.Width -= Theme.Px(8);
        TextRenderer.DrawText(e.Graphics, GetItemText(Items[e.Index]), Font, r, fg,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
    }
}

/// <summary>
/// A number with a minus and a plus, replacing <see cref="NumericUpDown"/>, whose spin buttons are
/// drawn from system colours and stay light on a dark panel. Takes the mouse wheel and the arrow keys.
/// </summary>
internal sealed class Stepper : Control
{
    int _value;
    int _hot;                    // -1 over minus, 1 over plus, 0 neither

    public int Minimum { get; }
    public int Maximum { get; }
    public string Suffix { get; set; } = "";

    public Stepper(int min, int max)
    {
        Minimum = min; Maximum = max; _value = min;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.Selectable | ControlStyles.ResizeRedraw, true);
        Font = Theme.Body;
        int h = TextRenderer.MeasureText("X", Theme.Body).Height + Theme.Px(10);
        Size = new Size(Theme.Px(104), h);
        Margin = Theme.Pad(0, 3, 8, 3);
        BackColor = Theme.Field;
        ForeColor = Theme.Text;
        TabStop = true;
    }

    public int Value
    {
        get => _value;
        set
        {
            int v = Math.Clamp(value, Minimum, Maximum);
            if (v == _value) return;
            _value = v;
            Invalidate();
            ValueChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public event EventHandler? ValueChanged;

    int ButtonWidth => Math.Max(Theme.Px(22), Height - 2);
    Rectangle MinusBox => new(1, 1, ButtonWidth, Height - 2);
    Rectangle PlusBox => new(Width - 1 - ButtonWidth, 1, ButtonWidth, Height - 2);

    protected override void OnMouseDown(MouseEventArgs e)
    {
        Focus();
        if (MinusBox.Contains(e.Location)) Value--;
        else if (PlusBox.Contains(e.Location)) Value++;
        base.OnMouseDown(e);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        int hot = MinusBox.Contains(e.Location) ? -1 : PlusBox.Contains(e.Location) ? 1 : 0;
        if (hot != _hot) { _hot = hot; Invalidate(); }
        base.OnMouseMove(e);
    }

    protected override void OnMouseLeave(EventArgs e) { if (_hot != 0) { _hot = 0; Invalidate(); } base.OnMouseLeave(e); }

    // The wheel adjusts the number only once this field has been clicked into. Scrolling the page over
    // it scrolls the page, rather than quietly changing the percentage you drink at.
    protected override void OnMouseWheel(MouseEventArgs e)
    {
        if (e is HandledMouseEventArgs h) h.Handled = true;
        if (Enabled && Focused) Value += Math.Sign(e.Delta);
        else Theme.ScrollBehind(this, e.Delta);
    }
    protected override void OnEnter(EventArgs e) { Invalidate(); base.OnEnter(e); }
    protected override void OnLeave(EventArgs e) { Invalidate(); base.OnLeave(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override bool IsInputKey(Keys key) => key is Keys.Up or Keys.Down or Keys.Left or Keys.Right || base.IsInputKey(key);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode is Keys.Up or Keys.Right) { Value++; e.Handled = true; }
        else if (e.KeyCode is Keys.Down or Keys.Left) { Value--; e.Handled = true; }
        base.OnKeyDown(e);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Card);
        var body = new Rectangle(0, 0, Width - 1, Height - 1);
        using (var fill = new SolidBrush(Enabled ? Theme.Field : Theme.Card)) g.FillRectangle(fill, body);
        using (var pen = new Pen(Enabled && Focused ? Theme.EdgeLit : Theme.Edge)) g.DrawRectangle(pen, body);
        Glyph(g, MinusBox, "−", _hot == -1);
        Glyph(g, PlusBox, "+", _hot == 1);
        int w = ButtonWidth;
        TextRenderer.DrawText(g, _value + Suffix, Font, new Rectangle(w, 0, Width - 2 * w, Height),
            Enabled ? ForeColor : Theme.Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }

    void Glyph(Graphics g, Rectangle r, string glyph, bool hot)
    {
        if (hot && Enabled) using (var fill = new SolidBrush(Theme.Head)) g.FillRectangle(fill, r);
        TextRenderer.DrawText(g, glyph, Font, r, !Enabled ? Theme.Edge : hot ? Theme.Gold : Theme.Dim,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}
