using System.Runtime.InteropServices;

namespace D2Chain.D2FUtil;

/// <summary>
/// The Cactus-style split install: one base <c>Diablo II</c> folder holding the big shared archives
/// (d2data, d2exp, d2char, music, video …) and a <c>Platforms</c> folder with a small DLL-and-exe set
/// for each patch. A patch is run from a folder that has the patch's own DLLs AND the base archives, so
/// this assembles that folder per patch: the platform's files copied in (they are small and must win),
/// and the base's files it does not provide brought in as hard links, which cost no extra disk. Each
/// patch gets its own runtime folder, so several patches run side by side.
/// </summary>
internal sealed class PlatformSet
{
    public string BaseDir { get; }
    public string PlatformsDir { get; }
    public string RunRoot { get; }

    /// <summary>The patch the base folder itself is, so that patch can launch from the base folder
    /// directly with no runtime folder at all.</summary>
    public string BasePatch { get; }

    PlatformSet(string baseDir, string platformsDir, string runRoot)
    {
        BaseDir = baseDir; PlatformsDir = platformsDir; RunRoot = runRoot;
        BasePatch = GameInstall.Inspect(baseDir).Patch;
    }

    /// <summary>
    /// Finds the base folder and the Platforms folder next to the launcher's exe, the layout the
    /// published folder ships in. Returns null when either is absent, so the launcher falls back to the
    /// plain single-folder mode.
    /// </summary>
    public static PlatformSet? Detect()
    {
        string? exeDir = Path.GetDirectoryName(Application.ExecutablePath);
        if (exeDir is null) return null;
        return DetectIn(exeDir);
    }

    public static PlatformSet? DetectIn(string dir)
    {
        string platforms = Path.Combine(dir, "Platforms");
        if (!Directory.Exists(platforms)) return null;
        // The base folder is the sibling that has the big data archives. It is normally named
        // "Diablo II", but any sibling folder with d2data.mpq will do.
        string? baseDir = null;
        string named = Path.Combine(dir, "Diablo II");
        if (File.Exists(Path.Combine(named, "d2data.mpq"))) baseDir = named;
        else
            foreach (var sub in SafeDirs(dir))
                if (!sub.Equals(platforms, StringComparison.OrdinalIgnoreCase) &&
                    File.Exists(Path.Combine(sub, "d2data.mpq"))) { baseDir = sub; break; }
        if (baseDir is null) return null;
        return new PlatformSet(baseDir, platforms, Path.Combine(dir, "run"));
    }

    static IEnumerable<string> SafeDirs(string dir)
    {
        try { return Directory.EnumerateDirectories(dir); } catch { return Array.Empty<string>(); }
    }

    /// <summary>Does the base folder hold the shared archives a platform relies on?</summary>
    public IReadOnlyList<string> BaseMissing()
    {
        string[] need = { "d2data.mpq", "d2exp.mpq", "d2char.mpq", "d2sfx.mpq" };
        var have = SafePresent(BaseDir);
        return need.Where(f => !have.Contains(f)).ToList();
    }

    /// <summary>Every patch under Platforms that can actually be launched, with its detected version.</summary>
    public IReadOnlyList<GameInstall> Patches()
    {
        var list = new List<GameInstall>();
        foreach (var sub in SafeDirs(PlatformsDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
        {
            var g = GameInstall.Inspect(sub);
            if (g.IsGame) list.Add(g);          // has a Game.exe and a readable D2Client.dll
        }
        return list;
    }

    static HashSet<string> SafePresent(string dir)
    {
        try { return new HashSet<string>(Directory.EnumerateFiles(dir).Select(Path.GetFileName)!, StringComparer.OrdinalIgnoreCase); }
        catch { return new HashSet<string>(StringComparer.OrdinalIgnoreCase); }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLinkW(string link, string target, IntPtr reserved);

    /// <summary>
    /// Assembles (or refreshes) the runtime folder for a platform and returns its Game.exe. The platform
    /// files are copied so they always win; the base's remaining files are hard-linked, so the gigabyte
    /// of shared archives is not duplicated. Falls back to a copy where a hard link cannot be made (a
    /// different volume), so it still works, just using disk.
    /// </summary>
    /// <summary>
    /// The Game.exe to launch for a patch. The base folder's own patch launches straight from the base
    /// folder, so no runtime folder is made for it; every other patch is assembled under 'run' and
    /// launched from there.
    /// </summary>
    public string RuntimeExe(GameInstall platform, Action<string>? log = null)
    {
        if (platform.Patch == BasePatch) return Path.Combine(BaseDir, "Game.exe");
        return BuildRuntime(platform, log);
    }

    /// <summary>
    /// Assembles (or refreshes) the runtime folder for a platform and returns its Game.exe. Every file,
    /// the platform's DLLs and the base's archives alike, is brought in as a HARD LINK, so the folder
    /// points at bytes that already exist and uses essentially no disk of its own. Where a hard link
    /// cannot be made (a different drive) the file is copied instead, so it still works.
    /// </summary>
    public string BuildRuntime(GameInstall platform, Action<string>? log = null)
    {
        string runDir = Path.Combine(RunRoot, Path.GetFileName(platform.Dir));
        Directory.CreateDirectory(runDir);

        // The platform's own files first - they define the patch and must win over the base's. Only the
        // files the game actually loads are brought in; the batch files, updaters and survey tools that
        // clutter an old install are left out, so a run folder holds nothing but DLLs and archives.
        var provided = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var src in Directory.EnumerateFiles(platform.Dir))
        {
            string name = Path.GetFileName(src);
            if (!IsGameFile(name)) continue;
            provided.Add(name);
            Link(src, Path.Combine(runDir, name), log);
        }
        // Then everything the base has that the platform did not provide: the big archives and extras.
        foreach (var src in Directory.EnumerateFiles(BaseDir))
        {
            string name = Path.GetFileName(src);
            if (IsGameFile(name) && !provided.Contains(name)) Link(src, Path.Combine(runDir, name), log);
        }
        return Path.Combine(runDir, "Game.exe");
    }

    // Everything the game needs is brought in; only clear junk is skipped. An allow-list by extension was
    // a mistake - it left out D2.LNG and the game could not start its language manager. So this is a
    // block-list instead: batch files, install logs and the Blizzard updater/diagnostic tools are
    // dropped, and anything else (DLLs, archives, D2.LNG, keys, the game exes) comes along.
    static readonly string[] SkipNames =
    {
        "BNUpdate.exe", "BlizzardError.exe", "SystemSurvey.exe", "D2VidTst.exe",
        "glide-init.exe", "d2launch.exe", "d2multi.exe", "bncache.dat",
    };
    static bool IsGameFile(string name)
    {
        string ext = Path.GetExtension(name).ToLowerInvariant();
        if (ext is ".bat" or ".log" or ".html" or ".htm") return false;
        foreach (var s in SkipNames) if (name.Equals(s, StringComparison.OrdinalIgnoreCase)) return false;
        return true;
    }

    /// <summary>Same rule as the runtime assembler, for the first-run base-folder setup.</summary>
    public static bool IsGameFilePublic(string name) => IsGameFile(name);

    /// <summary>Hard-links src to dst (no extra disk), falling back to a copy. Returns success.</summary>
    public static bool HardLinkOrCopy(string src, string dst)
    {
        try
        {
            if (CreateHardLinkW(dst, src, IntPtr.Zero)) return true;
            File.Copy(src, dst, true);
            return true;
        }
        catch { return false; }
    }

    // Point dst at src by a hard link, keeping it current: a link whose target is a different size (the
    // platform was updated) is remade, one that already matches is left alone. Falls back to a copy.
    static void Link(string src, string dst, Action<string>? log)
    {
        try
        {
            var s = new FileInfo(src);
            if (File.Exists(dst))
            {
                if (new FileInfo(dst).Length == s.Length) return;   // already the same bytes
                File.Delete(dst);
            }
            if (CreateHardLinkW(dst, src, IntPtr.Zero)) return;
            File.Copy(src, dst, true);
        }
        catch (Exception ex) { log?.Invoke($"could not bring in {Path.GetFileName(src)}: {ex.Message}"); }
    }

    /// <summary>Deletes the assembled runtime folders. The links are cheap to remake on the next launch.</summary>
    public void CleanRun()
    {
        try { if (Directory.Exists(RunRoot)) Directory.Delete(RunRoot, true); } catch { }
    }
}
