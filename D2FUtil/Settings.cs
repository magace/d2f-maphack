using System.Text.Json;

namespace D2Chain.D2FUtil;

/// <summary>Persisted UI state, in %LOCALAPPDATA%\D2FUtil\settings.json next to the extracted DLL.</summary>
internal sealed class Settings
{
    public List<string> Folders { get; set; } = new();
    public string? Selected { get; set; }
    public bool Windowed { get; set; } = true;
    public bool SkipToBnet { get; set; }
    public bool NoSound { get; set; }
    public string ExtraArgs { get; set; } = "";
    public int Clients { get; set; } = 1;
    public bool Maphack { get; set; } = true;
    public int KeyToggle { get; set; } = 0x74;   // VK_F5
    public int KeyReveal { get; set; } = 0x75;   // VK_F6

    /// <summary>Travel hotkeys: walk or teleport the character to an exit, the waypoint or a place.</summary>
    public bool Travel { get; set; } = true;
    /// <summary>Mark the targets on the game's own automap.</summary>
    public bool MarkTargets { get; set; } = true;
    /// <summary>Draw lines and labels to each target over the automap.</summary>
    public bool Overlay { get; set; } = true;
    /// <summary>Mark nearby monsters on the overlay.</summary>
    public bool ShowMonsters { get; set; } = true;
    /// <summary>List the places you can travel to, above the control bar, with their keys.</summary>
    public bool ShowList { get; set; } = true;
    /// <summary>Where that list sits: pixels from the left, and from the bottom of the game window.</summary>
    public int ListX { get; set; } = 12;
    public int ListBottom { get; set; } = 92;

    /// <summary>Drink a potion, from the belt or the inventory, when life or mana falls below this.</summary>
    public bool AutoPotion { get; set; }
    public int PotionLifePct { get; set; } = 50;
    public int PotionManaPct { get; set; } = 30;
    /// <summary>Keep rejuvenations for below this much life, rather than spending them topping up.</summary>
    public int PotionRejuvPct { get; set; } = 25;

    /// <summary>
    /// Overlay line colours, as indices into the game's palette, so the exact shade depends on the
    /// game's own colours. Editable here rather than in the UI because picking one is trial and error.
    /// </summary>
    public int ColourExit { get; set; } = 0x84;
    public int ColourWaypoint { get; set; } = 0x97;
    public int ColourPlace { get; set; } = 0x0C;
    public int ColourMonster { get; set; } = 0x5B;
    public int ColourActive { get; set; } = 0x9B;
    /// <summary>0 run, 1 walk, 2 teleport (only teleports if Teleport is the character's right skill).</summary>
    public int MoveMode { get; set; }
    public int KeyNext { get; set; } = 0x22;     // VK_NEXT   (Page Down)
    public int KeyPrev { get; set; } = 0x21;     // VK_PRIOR  (Page Up)
    public int KeyWaypoint { get; set; } = 0x24; // VK_HOME
    public int KeyCycle { get; set; } = 0x23;    // VK_END

    /// <summary>
    /// Diagnostic: makes the in-game module dump the game's room structures to its log the first time
    /// it reveals each level. Only useful when adding support for a patch, or when asking someone to
    /// send a log; there is no UI for it, set it in settings.json by hand.
    /// </summary>
    public bool Dump { get; set; }

    public static string Dir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "D2FUtil");
    static string FilePath => Path.Combine(Dir, "settings.json");
    public static string LogPath => Path.Combine(Dir, "d2fmh.log");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch { /* corrupt file: start fresh */ }
        return new Settings();
    }

    public void Save()
    {
        Directory.CreateDirectory(Dir);
        File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>
    /// Writes the embedded in-game DLL out and the ini it reads at load. Returns the DLL path to inject.
    ///
    /// <para>The file is named after its content hash (<c>d2fmh-1a2b3c4d.dll</c>). A client that is
    /// still running has its copy mapped and Windows will not let that file be replaced, so a new build
    /// simply gets a new name; copies no client holds any more are deleted opportunistically. The DLL
    /// finds its ini and log by directory, not by its own file name.</para>
    /// </summary>
    public string PrepareDll()
    {
        Directory.CreateDirectory(Dir);

        // Development override: D2FUTIL_DLL=<path> injects that DLL instead of the embedded one, so the
        // in-game module can be rebuilt and retried without republishing the launcher. Deliberately an
        // environment variable rather than "a DLL next to the exe", which would be picked up by accident.
        string? dev = Environment.GetEnvironmentVariable("D2FUTIL_DLL");
        if (!string.IsNullOrWhiteSpace(dev) && File.Exists(dev))
        {
            WriteIni();
            return Path.GetFullPath(dev);
        }

        using var res = typeof(Settings).Assembly.GetManifestResourceStream("d2fmh.dll")
            ?? throw new InvalidOperationException("d2fmh.dll is not embedded in this build");
        byte[] bytes = new byte[res.Length]; res.ReadExactly(bytes);
        string hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes))[..8].ToLowerInvariant();
        string dll = Path.Combine(Dir, $"d2fmh-{hash}.dll");
        if (!File.Exists(dll)) File.WriteAllBytes(dll, bytes);
        foreach (var old in Directory.GetFiles(Dir, "d2fmh*.dll"))
            if (!string.Equals(old, dll, StringComparison.OrdinalIgnoreCase))
                try { File.Delete(old); } catch { /* still mapped by a running client */ }
        WriteIni();
        return dll;
    }

    /// <summary>The settings the in-game module reads when it loads. It looks for this in this same
    /// folder (via %LOCALAPPDATA%), never beside itself, so a dev-override DLL still reads it.</summary>
    void WriteIni() => File.WriteAllText(Path.Combine(Dir, "d2fmh.ini"),
        "[d2fmh]\r\n" +
        $"maphack={(Maphack ? 1 : 0)}\r\n" +
        $"key_toggle={KeyToggle}\r\nkey_reveal={KeyReveal}\r\n" +
        $"travel={(Travel ? 1 : 0)}\r\nmark_targets={(MarkTargets ? 1 : 0)}\r\nmove_mode={MoveMode}\r\n" +
        $"overlay={(Overlay ? 1 : 0)}\r\nshow_monsters={(ShowMonsters ? 1 : 0)}\r\n" +
        $"show_list={(ShowList ? 1 : 0)}\r\nlist_x={ListX}\r\nlist_bottom={ListBottom}\r\n" +
        $"auto_potion={(AutoPotion ? 1 : 0)}\r\npotion_life_pct={PotionLifePct}\r\npotion_mana_pct={PotionManaPct}\r\n" +
        $"potion_rejuv_pct={PotionRejuvPct}\r\n" +
        $"colour_exit={ColourExit}\r\ncolour_waypoint={ColourWaypoint}\r\ncolour_place={ColourPlace}\r\n" +
        $"colour_monster={ColourMonster}\r\ncolour_active={ColourActive}\r\n" +
        $"key_next={KeyNext}\r\nkey_prev={KeyPrev}\r\nkey_waypoint={KeyWaypoint}\r\nkey_cycle={KeyCycle}\r\n" +
        $"log=1\r\ndump={(Dump ? 1 : 0)}\r\n");
}
