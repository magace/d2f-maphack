using System.Diagnostics;

namespace D2Chain.D2FUtil;

/// <summary>
/// D2F Utilities — launch one or more Diablo II clients (1.07 – 1.13d) with the D2F in-game module:
/// multi-instance unlock, a level-reveal maphack, hotkeys that walk or teleport the character to an
/// exit, the waypoint or a notable place, and auto potion. Point it at any Diablo II folder; the patch
/// is read from that folder's D2Client.dll.
///
/// <para>The settings are grouped into sections down the left rather than stacked in one long scroll,
/// so each page fits on screen and Launch is always where you left it. The window is dark by hand —
/// see <see cref="Theme"/> for why WinForms needs several controls drawn from scratch to manage it.</para>
/// </summary>
internal sealed class MainForm : Form
{
    readonly Settings _s = Settings.Load();

    readonly DarkCheck _windowed = new("Windowed  (-w)");
    readonly DarkCheck _skipBnet = new("Skip to Battle.net login  (-skiptobnet)");
    readonly DarkCheck _noSound = new("No sound  (-ns)");
    readonly TextBox _extra = Theme.Entry();
    readonly Stepper _count = new(1, 8);

    readonly DarkCheck _maphack = new("Reveal the level I am in");
    readonly DarkCheck _travel = new("Travel hotkeys");
    readonly DarkCheck _mark = new("Mark targets on the automap");
    readonly DarkCheck _overlay = new("Draw lines and labels to each target");
    readonly DarkCheck _monsters = new("Show nearby monsters");
    readonly DarkCheck _list = new("List places above the control bar");
    readonly DarkCombo _moveMode = new() { Width = Theme.Px(320) };

    readonly DarkCombo _kToggle = NewKeyBox(), _kReveal = NewKeyBox();
    readonly DarkCombo _kNext = NewKeyBox(), _kPrev = NewKeyBox(), _kWaypoint = NewKeyBox(), _kCycle = NewKeyBox();

    readonly DarkCheck _autoPot = new("Drink potions automatically");
    readonly Stepper _lifePct = new(1, 99);
    readonly Stepper _manaPct = new(0, 99);
    readonly Stepper _rejuvPct = new(0, 99);

    readonly Button _launch = Theme.Btn("LAUNCH", primary: true);
    readonly TextBox _log = Theme.Entry(multiline: true);
    readonly Label _status = new() { Dock = DockStyle.Fill, ForeColor = Theme.Dim, BackColor = Theme.Ink, TextAlign = ContentAlignment.MiddleLeft };

    readonly FlowLayoutPanel _nav = new() { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Theme.Ink, Padding = Theme.Pad(0, 10, 0, 0) };
    readonly Panel _pages = new() { Dock = DockStyle.Fill, BackColor = Theme.Ink };
    readonly List<(NavItem Tab, Control Page)> _sections = new();

    // The Cactus split install (base folder + Platforms), if the launcher finds one next to itself.
    readonly PlatformSet? _platforms = PlatformSet.Detect();
    readonly List<(GameInstall Patch, DarkCheck Check, Stepper Count)> _patchRows = new();

    static DarkCombo NewKeyBox()
    {
        var c = new DarkCombo { Width = Theme.Px(150) };
        c.Items.AddRange(HotKey.All);
        return c;
    }

    public MainForm()
    {
        Text = "D2F Utilities";
        // The font goes on before any size: WinForms rescales a form when its font changes, which once
        // shrank this window to two thirds and clipped everything below the fold. Scaling for the
        // display is done explicitly instead, through Theme.Px, so the automatic kind is turned off.
        Font = Theme.Body;
        AutoScaleMode = AutoScaleMode.None;
        BackColor = Theme.Ink;
        ForeColor = Theme.Text;
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(Theme.Px(720), Theme.Px(460));
        // Asked for in pixels at 100%, then scaled — and kept inside the desktop, because at 150% the
        // height it wants can be taller than the screen. Each section scrolls if it has to.
        var work = Screen.PrimaryScreen?.WorkingArea ?? new Rectangle(0, 0, 1280, 800);
        Size = new Size(Math.Min(Theme.Px(900), work.Width - Theme.Px(40)),
                        Math.Min(Theme.Px(720), work.Height - Theme.Px(40)));

        _log.ReadOnly = true;
        _log.ScrollBars = ScrollBars.Vertical;
        _log.Dock = DockStyle.Fill;
        _log.BorderStyle = BorderStyle.None;
        _log.ForeColor = Theme.Dim;
        _extra.Dock = DockStyle.Fill;
        _extra.Margin = Theme.Pad(0, 4, 0, 4);
        _launch.Size = new Size(Theme.Px(184), Theme.Px(44));
        _lifePct.Suffix = " %"; _manaPct.Suffix = " %"; _rejuvPct.Suffix = " %";

        var banner = new Banner { Dock = DockStyle.Fill, Height = Banner.Tall, BackColor = Theme.Ink };

        var side = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Ink };
        side.Controls.Add(_nav);
        side.Paint += (_, e) =>
        {
            using var line = new Pen(Theme.Edge);
            e.Graphics.DrawLine(line, side.Width - 1, 0, side.Width - 1, side.Height);
        };

        var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, BackColor = Theme.Ink };
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, Theme.Px(168)));
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        body.Controls.Add(side, 0, 0);
        body.Controls.Add(_pages, 1, 0);

        var bottom = new Panel { Dock = DockStyle.Fill, AutoSize = true, BackColor = Theme.Ink, Padding = Theme.Pad(16, 8, 16, 12) };
        var bottomRow = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2, BackColor = Theme.Ink };
        bottomRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        bottomRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _status.Height = Theme.Px(44);
        bottomRow.Controls.Add(_status, 0, 0);
        bottomRow.Controls.Add(_launch, 1, 0);
        bottom.Controls.Add(bottomRow);
        bottom.Paint += (_, e) =>
        {
            using var line = new Pen(Theme.Edge);
            e.Graphics.DrawLine(line, 0, 0, bottom.Width, 0);
        };

        var outer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, BackColor = Theme.Ink };
        outer.RowStyles.Add(new RowStyle(SizeType.Absolute, Banner.Tall));
        outer.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        outer.Controls.Add(banner, 0, 0);
        outer.Controls.Add(body, 0, 1);
        outer.Controls.Add(bottom, 0, 2);
        Controls.Add(outer);

        // ---- Game: the patch checklist, driven by the Platforms folder next to the exe
        var gameStack = NewPage("Game", 2);
        int gameRow = 0;
        if (_platforms is not null)
            gameStack.Controls.Add(BuildPatchesCard(), 0, gameRow++);
        else
        {
            var miss = new Card("Diablo II");
            miss.Controls.Add(Theme.Note(
                "No Platforms folder was found next to this program.\n\n" +
                "Put a 'Platforms' folder (one subfolder of DLLs per patch) and a 'Diablo II' folder\n" +
                "with the game's archives beside D2FUtil.exe, then reopen."));
            gameStack.Controls.Add(miss, 0, gameRow++);
        }

        var optCard = new Card("Launch options");
        var ol = NewGrid(2);
        ol.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        ol.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        ol.Controls.Add(_windowed, 0, 0); ol.SetColumnSpan(_windowed, 2);
        ol.Controls.Add(_skipBnet, 0, 1); ol.SetColumnSpan(_skipBnet, 2);
        ol.Controls.Add(_noSound, 0, 2); ol.SetColumnSpan(_noSound, 2);
        ol.Controls.Add(Theme.Cap("Extra arguments:"), 0, 3);
        ol.Controls.Add(_extra, 1, 3);
        var countRow = new FlowLayoutPanel { AutoSize = true, Margin = Theme.Pad(0, 8, 0, 0), BackColor = Theme.Card };
        countRow.Controls.Add(Theme.Cap("Clients to launch:"));
        countRow.Controls.Add(_count);
        ol.Controls.Add(countRow, 0, 4); ol.SetColumnSpan(countRow, 2);
        optCard.Controls.Add(ol);
        gameStack.Controls.Add(optCard, 0, gameRow++);

        // ---- Maphack
        var mhStack = NewPage("Maphack", 1);
        var mhCard = new Card("Reveal");
        var ml = NewGrid(4);
        ml.Controls.Add(_maphack, 0, 0); ml.SetColumnSpan(_maphack, 4);
        ml.Controls.Add(Theme.Cap("Toggle maphack:"), 0, 1); ml.Controls.Add(_kToggle, 1, 1);
        ml.Controls.Add(Theme.Cap("Reveal again:"), 2, 1); ml.Controls.Add(_kReveal, 3, 1);
        var mhNote = Theme.Note(
            "The level you are standing in is revealed into the game's own automap as you enter it.\n" +
            "A beep confirms the toggle. Keys bound here are taken by the module, so the game never\n" +
            "sees them and they will not also fire a skill slot.");
        ml.Controls.Add(mhNote, 0, 2);
        ml.SetColumnSpan(mhNote, 4);
        mhCard.Controls.Add(ml);
        mhStack.Controls.Add(mhCard, 0, 0);

        // ---- Travel
        var tvStack = NewPage("Travel", 2);
        var tvCard = new Card("Go to");
        var tl = NewGrid(4);
        tl.Controls.Add(_travel, 0, 0); tl.SetColumnSpan(_travel, 4);
        tl.Controls.Add(Theme.Cap("Move by:"), 0, 1);
        tl.Controls.Add(_moveMode, 1, 1); tl.SetColumnSpan(_moveMode, 3);
        tl.Controls.Add(Theme.Cap("Next area:"), 0, 2); tl.Controls.Add(_kNext, 1, 2);
        tl.Controls.Add(Theme.Cap("Previous area:"), 2, 2); tl.Controls.Add(_kPrev, 3, 2);
        tl.Controls.Add(Theme.Cap("Waypoint:"), 0, 3); tl.Controls.Add(_kWaypoint, 1, 3);
        tl.Controls.Add(Theme.Cap("Cycle targets:"), 2, 3); tl.Controls.Add(_kCycle, 3, 3);
        var tvNote = Theme.Note("Press a travel key again to stop.");
        tl.Controls.Add(tvNote, 0, 4);
        tl.SetColumnSpan(tvNote, 4);
        tvCard.Controls.Add(tl);
        tvStack.Controls.Add(tvCard, 0, 0);

        var seeCard = new Card("On screen");
        var sl = NewGrid(2);
        sl.Controls.Add(_mark, 0, 0); sl.SetColumnSpan(_mark, 2);
        sl.Controls.Add(_overlay, 0, 1); sl.SetColumnSpan(_overlay, 2);
        sl.Controls.Add(_monsters, 0, 2); sl.SetColumnSpan(_monsters, 2);
        sl.Controls.Add(_list, 0, 3); sl.SetColumnSpan(_list, 4);
        var seeNote = Theme.Note(
            "Lines and labels are drawn over the automap, so open the map to see them. The list above\n" +
            "the control bar also shows life, mana and how many potions of each kind you are carrying,\n" +
            "and it is there whether the map is open or not.");
        sl.Controls.Add(seeNote, 0, 4);
        sl.SetColumnSpan(seeNote, 2);
        seeCard.Controls.Add(sl);
        tvStack.Controls.Add(seeCard, 0, 1);

        // ---- Potions
        var potStack = NewPage("Potions", 1);
        var potCard = new Card("Drink at");
        var pl = NewGrid(3);
        pl.Controls.Add(_autoPot, 0, 0); pl.SetColumnSpan(_autoPot, 3);
        pl.Controls.Add(Theme.Cap("Drink healing below:"), 0, 1);
        pl.Controls.Add(_lifePct, 1, 1);
        pl.Controls.Add(Theme.Unit("of life"), 2, 1);
        pl.Controls.Add(Theme.Cap("Drink mana below:"), 0, 2);
        pl.Controls.Add(_manaPct, 1, 2);
        pl.Controls.Add(Theme.Unit("of mana   (0 to never drink mana)"), 2, 2);
        pl.Controls.Add(Theme.Cap("Save rejuvenations for below:"), 0, 3);
        pl.Controls.Add(_rejuvPct, 1, 3);
        pl.Controls.Add(Theme.Unit("of life"), 2, 3);
        var potNote = Theme.Note(
            "It works out what each potion is by itself, by watching whether your life or mana went up.\n" +
            "A rejuvenation raises both, which is how it tells them apart, and it keeps those for\n" +
            "emergencies. It learns from the belt, then uses that kind of potion from the belt or the\n" +
            "inventory, and remembers what it learned for every game after.");
        pl.Controls.Add(potNote, 0, 4);
        pl.SetColumnSpan(potNote, 3);
        potCard.Controls.Add(pl);
        potStack.Controls.Add(potCard, 0, 0);

        // ---- Log
        var logPage = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Ink, Padding = Theme.Pad(16, 14, 16, 14), Visible = false };
        var openData = Theme.Btn("Open data folder");
        openData.Margin = Theme.Pad(0, 10, 0, 0);
        var logCard = new Card("This session") { AutoSize = false, Dock = DockStyle.Fill };
        logCard.Controls.Add(_log);
        var logBottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, AutoSize = true, BackColor = Theme.Ink };
        logBottom.Controls.Add(openData);
        logPage.Controls.Add(logCard);
        logPage.Controls.Add(logBottom);
        AddSection("Log", logPage);

        // ---- behaviour
        openData.Click += (_, _) => OpenDataFolder();
        _launch.Click += (_, _) => LaunchPlatforms();
        _launch.Enabled = _platforms is not null;
        _travel.CheckedChanged += (_, _) => UpdateEnabled();
        _autoPot.CheckedChanged += (_, _) => UpdateEnabled();
        FormClosing += (_, _) => Persist();

        // Two actions on one key would leave the second unreachable, so binding a key that is already
        // taken swaps the two rather than silently shadowing one.
        foreach (var box in _keyBoxes) box.SelectedIndexChanged += (s, _) => NoDuplicateKeys((ComboBox)s!);

        _moveMode.Items.AddRange(new object[]
        {
            "Run  (the game paths around obstacles)",
            "Walk",
            "Teleport  (only if Teleport is the right-click skill)",
        });

        _windowed.Checked = _s.Windowed; _skipBnet.Checked = _s.SkipToBnet; _noSound.Checked = _s.NoSound;
        _extra.Text = _s.ExtraArgs; _count.Value = Math.Clamp(_s.Clients, 1, 8);
        _maphack.Checked = _s.Maphack; _travel.Checked = _s.Travel; _mark.Checked = _s.MarkTargets;
        _overlay.Checked = _s.Overlay; _monsters.Checked = _s.ShowMonsters; _list.Checked = _s.ShowList;
        _autoPot.Checked = _s.AutoPotion;
        _lifePct.Value = Math.Clamp(_s.PotionLifePct, 1, 99);
        _manaPct.Value = Math.Clamp(_s.PotionManaPct, 0, 99);
        _rejuvPct.Value = Math.Clamp(_s.PotionRejuvPct, 0, 99);
        _moveMode.SelectedIndex = Math.Clamp(_s.MoveMode, 0, 2);
        _swapping = true;       // loading is not a user edit; do not try to resolve conflicts yet
        SetKey(_kToggle, _s.KeyToggle); SetKey(_kReveal, _s.KeyReveal);
        SetKey(_kNext, _s.KeyNext); SetKey(_kPrev, _s.KeyPrev);
        SetKey(_kWaypoint, _s.KeyWaypoint); SetKey(_kCycle, _s.KeyCycle);
        _swapping = false;
        foreach (var box in _keyBoxes) if (box.SelectedItem is HotKey k) _lastKey[box] = k.Vk;
        FixLoadedDuplicates();
        UpdateEnabled();

        Show(_sections[0].Page);
        Log($"Settings and log live in {Settings.Dir}");
    }

    /// <summary>A section page: a scrolling column of cards, plus its entry in the list on the left.</summary>
    TableLayoutPanel NewPage(string name, int cards)
    {
        var page = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.Ink, Padding = Theme.Pad(16, 14, 16, 14), Visible = false };
        var stack = new TableLayoutPanel
        {
            Dock = DockStyle.Top, ColumnCount = 1, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Theme.Ink,
        };
        for (int i = 0; i < cards; i++) stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        page.Controls.Add(stack);
        AddSection(name, page);
        return stack;
    }

    /// <summary>The grid of captions and fields inside a card.</summary>
    static TableLayoutPanel NewGrid(int columns) => new()
    {
        Dock = DockStyle.Top, AutoSize = true, ColumnCount = columns, BackColor = Theme.Card,
    };

    void AddSection(string name, Control page)
    {
        var tab = new NavItem(name) { Width = Theme.Px(167) };
        tab.Click += (_, _) => Show(page);
        _nav.Controls.Add(tab);
        _pages.Controls.Add(page);
        _sections.Add((tab, page));
    }

    void Show(Control page)
    {
        foreach (var (tab, p) in _sections)
        {
            bool on = ReferenceEquals(p, page);
            p.Visible = on;
            tab.Selected = on;
            tab.Invalidate();
        }
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        Theme.DarkCaption(Handle);      // a black window with a white caption bar looks broken
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        // Not in OnHandleCreated: the form gets its handle before its children have theirs, and a
        // control with no handle cannot be told which theme to draw its scroll bars from.
        foreach (var c in Scrollables(this)) Theme.DarkScrollBars(c);
    }

    static IEnumerable<Control> Scrollables(Control parent)
    {
        foreach (Control c in parent.Controls)
        {
            if (c is TextBox { Multiline: true } or ScrollableControl { AutoScroll: true }) yield return c;
            foreach (var inner in Scrollables(c)) yield return inner;
        }
    }

    ComboBox[] _keyBoxes => new ComboBox[] { _kToggle, _kReveal, _kNext, _kPrev, _kWaypoint, _kCycle };
    bool _swapping;

    void NoDuplicateKeys(ComboBox changed)
    {
        if (_swapping || changed.SelectedItem is not HotKey now) return;
        foreach (var other in _keyBoxes)
        {
            if (ReferenceEquals(other, changed) || other.SelectedItem is not HotKey k || k.Vk != now.Vk) continue;
            _swapping = true;
            try { SetKey(other, _lastKey.TryGetValue(changed, out int prev) ? prev : k.Vk); }
            finally { _swapping = false; }
            Log($"{now.Name} was already in use, so the two actions swapped keys.");
            break;
        }
        _lastKey[changed] = now.Vk;
    }

    readonly Dictionary<ComboBox, int> _lastKey = new();

    static void SetKey(ComboBox box, int vk)
    {
        var k = HotKey.For(vk);
        int i = box.Items.IndexOf(k);
        if (i < 0) { box.Items.Add(k); i = box.Items.Count - 1; }
        box.SelectedIndex = i;
    }
    static int GetKey(ComboBox box, int fallback) => box.SelectedItem is HotKey k ? k.Vk : fallback;

    /// <summary>A settings file can already contain two actions on one key; give the later ones a free
    /// key so every action stays reachable.</summary>
    void FixLoadedDuplicates()
    {
        var used = new HashSet<int>();
        foreach (var box in _keyBoxes)
        {
            if (box.SelectedItem is not HotKey k) continue;
            if (used.Add(k.Vk)) continue;
            var free = Array.Find(HotKey.All, c => !used.Contains(c.Vk));
            if (free is null) break;
            _swapping = true;
            try { SetKey(box, free.Vk); } finally { _swapping = false; }
            used.Add(free.Vk);
            _lastKey[box] = free.Vk;
            Log($"{k.Name} was bound to two actions; one moved to {free.Name}.");
        }
    }

    void UpdateEnabled()
    {
        foreach (Control c in new Control[] { _moveMode, _kNext, _kPrev, _kWaypoint, _kCycle, _mark, _overlay, _monsters, _list })
            c.Enabled = _travel.Checked;
        foreach (Control c in new Control[] { _lifePct, _manaPct, _rejuvPct })
            c.Enabled = _autoPot.Checked;
    }

    void OpenDataFolder()
    {
        try
        {
            Directory.CreateDirectory(Settings.Dir);
            Process.Start(new ProcessStartInfo(Settings.Dir) { UseShellExecute = true });
        }
        catch (Exception ex) { Log("could not open the data folder: " + ex.Message); }
    }

    string BuildArgs()
    {
        var a = new List<string>();
        if (_windowed.Checked) a.Add("-w");
        if (_skipBnet.Checked) a.Add("-skiptobnet");
        if (_noSound.Checked) a.Add("-ns");
        if (_extra.Text.Trim().Length > 0) a.Add(_extra.Text.Trim());
        return string.Join(' ', a);
    }

    /// <summary>The patch checklist, one row per platform found, each with its own count. Checked rows
    /// launch together on the one Launch button, each maphacked.</summary>
    Control BuildPatchesCard()
    {
        var card = new Card("Patches");
        var baseMissing = _platforms!.BaseMissing();

        // One grid of patch rows. The supported patches and the unsupported ones live in SEPARATE grids
        // so the whole unsupported block can be shown or hidden by one panel's Visible flag - toggling
        // dozens of controls inside a shared grid relaid the whole thing out and lagged badly.
        TableLayoutPanel NewRowGrid()
        {
            var g = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 3, BackColor = Theme.Card };
            g.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            g.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            g.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            return g;
        }
        void AddRow(TableLayoutPanel g, GameInstall p, bool supported)
        {
            string ver = System.IO.Path.GetFileName(p.Dir);     // the folder name is the version
            var chk = new DarkCheck(supported ? $"Diablo II {ver}" : $"Diablo II {ver}  (no maphack)");
            var cnt = new Stepper(1, 8) { Value = 1 };
            bool ok = baseMissing.Count == 0;
            chk.Enabled = ok; cnt.Enabled = ok;
            int r = g.RowCount++;
            g.Controls.Add(chk, 0, r);
            g.Controls.Add(Theme.Cap("clients:"), 1, r);
            g.Controls.Add(cnt, 2, r);
            _patchRows.Add((p, chk, cnt));
        }

        var patches = _platforms.Patches();
        var supportedGrid = NewRowGrid();
        var hiddenGrid = NewRowGrid();
        hiddenGrid.Visible = false;
        int nHidden = 0;
        foreach (var p in patches) if (p.MaphackSupported) AddRow(supportedGrid, p, true);
        foreach (var p in patches) if (!p.MaphackSupported) { AddRow(hiddenGrid, p, false); nHidden++; }

        // The card stacks: supported rows, a Show/Hide button, the (collapsed) unsupported rows, a note.
        var stack = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, BackColor = Theme.Card };
        int sr = 0;
        stack.Controls.Add(supportedGrid, 0, sr++);
        if (_patchRows.Count == 0)
            stack.Controls.Add(Theme.Note("No patches were found under the Platforms folder."), 0, sr++);
        if (nHidden > 0)
        {
            var show = Theme.Btn($"Show {nHidden} more (no maphack)");
            show.Margin = Theme.Pad(0, 8, 0, 4);
            show.Click += (_, _) =>
            {
                hiddenGrid.Visible = !hiddenGrid.Visible;
                show.Text = hiddenGrid.Visible ? "Hide unsupported patches" : $"Show {nHidden} more (no maphack)";
            };
            stack.Controls.Add(show, 0, sr++);
            stack.Controls.Add(hiddenGrid, 0, sr++);
        }
        var note = baseMissing.Count > 0
            ? Theme.Note($"The base Diablo II folder is missing {string.Join(", ", baseMissing)} — patches\ncannot run until the shared archives are there.")
            : Theme.Note("Tick the patches to run and set how many of each. One Launch starts them all, side by\n" +
                         "side, each with the maphack. A patch different from the base folder runs from its own\n" +
                         "folder under 'run', hard-linked to the base archives so it uses no real disk.");
        stack.Controls.Add(note, 0, sr++);
        card.Controls.Add(stack);
        return card;
    }

    /// <summary>Launches every ticked patch, each from its assembled runtime folder, each maphacked.</summary>
    void LaunchPlatforms()
    {
        if (_platforms is null) return;
        var chosen = _patchRows.Where(r => r.Check.Checked && r.Check.Enabled).ToList();
        if (chosen.Count == 0) { Log("tick at least one patch to launch"); return; }

        Persist();
        string dll;
        try { dll = _s.PrepareDll(); }
        catch (Exception ex) { Log("could not prepare the in-game module: " + ex.Message); return; }
        string args = BuildArgs();
        _launch.Enabled = false;
        try
        {
            foreach (var (patch, _, count) in chosen)
            {
                string exe;
                try { exe = _platforms.RuntimeExe(patch, Log); }
                catch (Exception ex) { Log($"could not assemble the {patch.Patch} runtime: {ex.Message}"); continue; }
                int n = count.Value;
                for (int i = 1; i <= n; i++)
                {
                    try
                    {
                        var p = Injector.Launch(exe, args, dll);
                        Log($"launched {patch.Patch} client {i}/{n}: pid {p.Id}{(_maphack.Checked && patch.MaphackSupported ? ", maphack on" : "")}");
                        WatchForEarlyExit(p);
                    }
                    catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 740)
                    {
                        Log($"{patch.Patch} needs administrator rights");
                        if (MessageBox.Show(this,
                                "A Game.exe here is set to run as administrator, so D2F Utilities must run as administrator to start it.\n\n" +
                                "Restart as administrator now?", "D2F Utilities", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                            RestartElevated();
                        return;
                    }
                    catch (Exception ex) { Log($"{patch.Patch} client {i}/{n} failed: {ex.Message}"); break; }
                    if (i < n) { Application.DoEvents(); Thread.Sleep(1500); }
                }
            }
        }
        finally { _launch.Enabled = true; }
    }

    /// <summary>
    /// A client that quits a second after it starts is the hardest kind of report to act on, because
    /// nothing appears on screen and the launcher has long since said "launched". So it is watched for
    /// a short while and the exit is written down, with the code the game returned: that number is
    /// usually the whole answer (a second copy refusing to run, a missing file, a failed video mode).
    /// </summary>
    void WatchForEarlyExit(Process p)
    {
        var started = DateTime.Now;
        var watch = new System.Windows.Forms.Timer { Interval = 1000 };
        int ticks = 0;
        watch.Tick += (_, _) =>
        {
            ticks++;
            bool gone;
            int code = 0;
            try { gone = p.HasExited; if (gone) code = p.ExitCode; }
            catch { gone = true; }
            if (gone)
            {
                watch.Stop(); watch.Dispose();
                double seconds = (DateTime.Now - started).TotalSeconds;
                Log($"client pid {p.Id} exited after {seconds:F1}s with code {code} (0x{code:X8}) — " +
                    "it closed itself, so the maphack never got a chance to run");
            }
            else if (ticks >= 15) { watch.Stop(); watch.Dispose(); }   // it is up and running
        };
        watch.Start();
    }

    void RestartElevated()
    {
        Persist();
        try
        {
            Process.Start(new ProcessStartInfo(Application.ExecutablePath) { UseShellExecute = true, Verb = "runas" });
            Close();
        }
        catch (System.ComponentModel.Win32Exception) { Log("elevation was declined"); }   // UAC cancelled
    }

    void Persist()
    {
        _s.Windowed = _windowed.Checked; _s.SkipToBnet = _skipBnet.Checked; _s.NoSound = _noSound.Checked;
        _s.ExtraArgs = _extra.Text; _s.Clients = _count.Value;
        _s.Maphack = _maphack.Checked; _s.Travel = _travel.Checked; _s.MarkTargets = _mark.Checked;
        _s.Overlay = _overlay.Checked; _s.ShowMonsters = _monsters.Checked; _s.ShowList = _list.Checked;
        _s.AutoPotion = _autoPot.Checked;
        _s.PotionLifePct = _lifePct.Value; _s.PotionManaPct = _manaPct.Value;
        _s.PotionRejuvPct = _rejuvPct.Value;
        _s.MoveMode = Math.Max(0, _moveMode.SelectedIndex);
        _s.KeyToggle = GetKey(_kToggle, _s.KeyToggle); _s.KeyReveal = GetKey(_kReveal, _s.KeyReveal);
        _s.KeyNext = GetKey(_kNext, _s.KeyNext); _s.KeyPrev = GetKey(_kPrev, _s.KeyPrev);
        _s.KeyWaypoint = GetKey(_kWaypoint, _s.KeyWaypoint); _s.KeyCycle = GetKey(_kCycle, _s.KeyCycle);
        try { _s.Save(); } catch (Exception ex) { Log("could not save settings: " + ex.Message); }
    }

    /// <summary>Goes to the log, and the latest line also sits beside the Launch button, where it is
    /// seen without opening the log at all.</summary>
    void Log(string line)
    {
        _log.AppendText($"[{DateTime.Now:HH:mm:ss}] {line}{Environment.NewLine}");
        _status.Text = line;
        // Also to launch.log in the data folder. The log window is gone the moment the launcher closes,
        // and when someone else's client misbehaves the two halves of the story - what the launcher did
        // and what the module saw - need to be sendable as a pair of files.
        try
        {
            Directory.CreateDirectory(Settings.Dir);
            File.AppendAllText(Path.Combine(Settings.Dir, "launch.log"),
                $"[{DateTime.Now:HH:mm:ss}] {line}{Environment.NewLine}");
        }
        catch { /* a log that cannot be written must not stop a launch */ }
    }
}
