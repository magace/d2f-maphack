using Microsoft.Win32;

namespace D2Chain.D2FUtil;

/// <summary>A Diablo II folder and what patch it turned out to be.</summary>
internal sealed record GameInstall(string Dir, string Exe, string Patch, bool MaphackSupported)
{
    /// <summary>
    /// Patches the in-game DLL has a table for, keyed by D2Client.dll's PE timestamp. Must match
    /// native\d2fmh_versions.h — the DLL decides support on its own; this is only for the UI.
    /// </summary>
    static readonly Dictionary<uint, string> Known = new()
    {
        [0x392ECD34] = "1.00", [0x3B007197] = "1.07", [0x3B2EB437] = "1.08", [0x3B7C5076] = "1.09", [0x3C0700B0] = "1.09d", [0x3F7CB8BE] = "1.10", [0x42E6C43F] = "1.11", [0x43028CA5] = "1.11b",
        [0x483CB8DF] = "1.12a", [0x4B95CA3E] = "1.13c", [0x4E9DE60A] = "1.13d",
    };

    /// <summary>
    /// Total conversions and mod loaders, by a file only they leave behind. A modded copy looks like an
    /// ordinary install from the outside — right patch, right DLLs — but it loads its own code first,
    /// usually quits the moment it sees anything else injected, and could not join the realm anyway.
    /// Worth naming in the window, because what it looks like from the outside is a client that starts
    /// and vanishes a second later with no message at all.
    /// </summary>
    static readonly (string File, string Name)[] Mods =
    {
        ("D2Sigma.dll", "Median XL Sigma"),
        ("MXL.mpq", "Median XL"),
        ("ProjectDiablo.mpq", "Project Diablo 2"),
        ("PlugY.dll", "PlugY"),
        ("D2SE.mpq", "D2SE"),
        ("BH.dll", "Slashdiablo maphack"),
        ("FogOriginal.dll", "a mod that has replaced Fog.dll"),
    };

    /// <summary>
    /// The files a stock Diablo II folder needs to start and play on the realm. Required ones missing
    /// means the game will not launch, or launches and dies at once with no message — which is exactly
    /// what a half-copied install looks like from the outside. Optional ones (music, speech, movies)
    /// only cost you sound and cinematics if absent.
    ///
    /// <para>Names are matched case-insensitively, as Windows stores them. The renderer DLLs
    /// (D2Direct3D, D2Glide, D2DDraw, D2Gdi) are not each required — the game picks the one for the
    /// video mode — so they are not listed; a folder with none of them is a deeper problem than a file
    /// check should diagnose.</para>
    /// </summary>
    static readonly string[] RequiredFiles =
    {
        // the DLLs the game loads on startup, before any renderer
        "Fog.dll", "Storm.dll", "Bnclient.dll",
        "D2Client.dll", "D2Common.dll", "D2Game.dll", "D2Net.dll", "D2Win.dll",
        "D2gfx.dll", "D2Lang.dll", "D2Launch.dll", "D2sound.dll", "D2CMP.dll",
        "D2MCPClient.dll", "D2Multi.dll", "ijl11.dll",
        // the data archives without which the game cannot run
        "d2data.mpq", "d2exp.mpq", "d2char.mpq", "d2sfx.mpq", "patch_d2.mpq",
    };

    static readonly string[] OptionalFiles =
    {
        "d2music.mpq", "d2xmusic.mpq", "d2speech.mpq", "d2xtalk.mpq", "d2video.mpq", "d2xvideo.mpq",
        "SmackW32.dll", "binkw32.dll",
    };

    /// <summary>Required files that are not in the folder. Empty when the install is complete.</summary>
    public IReadOnlyList<string> MissingRequired { get; private init; } = Array.Empty<string>();
    /// <summary>Optional files that are absent — sound, music and movies.</summary>
    public IReadOnlyList<string> MissingOptional { get; private init; } = Array.Empty<string>();

    static (List<string> required, List<string> optional) FindMissing(string dir)
    {
        // One directory listing, compared case-insensitively, rather than a File.Exists per name.
        HashSet<string> present;
        try { present = new HashSet<string>(
            Directory.EnumerateFiles(dir).Select(Path.GetFileName)!, StringComparer.OrdinalIgnoreCase); }
        catch { return (new List<string>(), new List<string>()); }
        var req = RequiredFiles.Where(f => !present.Contains(f)).ToList();
        var opt = OptionalFiles.Where(f => !present.Contains(f)).ToList();
        return (req, opt);
    }

    /// <summary>The mod found in the folder, if any.</summary>
    public string? Mod { get; private init; }

    public bool IsGame => Patch != "";

    public override string ToString() => Display;

    public string Display => !IsGame ? $"{Dir}  (no Game.exe here)"
        : $"{Dir}  [{Patch}{(MaphackSupported ? "" : ", no maphack")}{(Mod is null ? "" : ", " + Mod)}]";

    public static GameInstall Inspect(string dir)
    {
        dir = dir.TrimEnd('\\');
        string exe = Path.Combine(dir, "Game.exe");
        string client = Path.Combine(dir, "D2Client.dll");
        string? mod = null;
        foreach (var (file, name) in Mods)
            if (File.Exists(Path.Combine(dir, file))) { mod = name; break; }

        if (!File.Exists(exe)) return new GameInstall(dir, exe, "", false) { Mod = mod };
        var (req, opt) = FindMissing(dir);
        if (!File.Exists(client)) return new GameInstall(dir, exe, "1.14 (no D2Client.dll)", false) { Mod = mod, MissingRequired = req, MissingOptional = opt };
        uint stamp;
        try { stamp = PeTimeStamp(client); }
        catch { return new GameInstall(dir, exe, "unreadable D2Client.dll", false) { Mod = mod, MissingRequired = req, MissingOptional = opt }; }
        if (Known.TryGetValue(stamp, out var patch)) return new GameInstall(dir, exe, patch, true) { Mod = mod, MissingRequired = req, MissingOptional = opt };
        return new GameInstall(dir, exe, $"unknown patch ({stamp:X8})", false) { Mod = mod, MissingRequired = req, MissingOptional = opt };
    }

    static uint PeTimeStamp(string file)
    {
        using var f = File.OpenRead(file);
        using var r = new BinaryReader(f);
        f.Position = 0x3C; int lfanew = r.ReadInt32();
        f.Position = lfanew + 8;            // Signature(4) + Machine(2) + NumberOfSections(2)
        return r.ReadUInt32();
    }

    /// <summary>Where the game says it is installed, if anywhere.</summary>
    public static string? RegistryInstallPath()
    {
        foreach (var (hive, key) in new[] {
            (Registry.CurrentUser, @"Software\Blizzard Entertainment\Diablo II"),
            (Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Blizzard Entertainment\Diablo II"),
            (Registry.LocalMachine, @"SOFTWARE\Blizzard Entertainment\Diablo II") })
        {
            try
            {
                using var k = hive.OpenSubKey(key);
                if (k?.GetValue("InstallPath") is string p && Directory.Exists(p)) return p.TrimEnd('\\');
            }
            catch { /* no access */ }
        }
        return null;
    }
}
