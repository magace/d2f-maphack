using System.IO.Compression;
using System.Net.Http;

namespace D2Chain.D2FUtil;

/// <summary>
/// First-run setup: fetch the per-patch DLL sets from the Cactus project and build the base game folder
/// from the user's own Diablo II install. Neither ships with the launcher — the platform DLLs are
/// Blizzard's, redistributed by Cactus, and the archives are the user's own game data — so both are
/// pulled in on the machine rather than bundled.
/// </summary>
internal static class Setup
{
    // The Cactus repository ships one folder of DLLs per patch under "1. Files/Platforms". The whole
    // repo is downloaded once as a zip and only that subtree is extracted.
    const string CactusZip = "https://github.com/CDVyhlidal/Cactus/archive/refs/heads/master.zip";
    const string PlatformsPrefix = "1. Files/Platforms/";

    /// <summary>
    /// Downloads the Cactus repo and extracts its Platforms subtree into <paramref name="platformsDir"/>.
    /// Reports progress as short log lines. Runs on a background thread; do not call on the UI thread.
    /// </summary>
    public static void DownloadPlatforms(string platformsDir, Action<string> log)
    {
        Directory.CreateDirectory(platformsDir);
        string tmp = Path.Combine(Path.GetTempPath(), "cactus-" + Guid.NewGuid().ToString("N") + ".zip");
        try
        {
            log("downloading the Cactus platforms (this is large, give it a minute)…");
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("D2FUtil");
                using var resp = http.GetAsync(CactusZip, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
                resp.EnsureSuccessStatusCode();
                using var src = resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
                using var dst = File.Create(tmp);
                src.CopyTo(dst, 1 << 20);
            }

            log("extracting platform files…");
            using var zip = ZipFile.OpenRead(tmp);
            int files = 0; var versions = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in zip.Entries)
            {
                // Entry paths look like "Cactus-master/1. Files/Platforms/1.10/D2Client.dll".
                int at = entry.FullName.IndexOf(PlatformsPrefix, StringComparison.OrdinalIgnoreCase);
                if (at < 0) continue;
                string rel = entry.FullName.Substring(at + PlatformsPrefix.Length);   // "1.10/D2Client.dll"
                if (rel.Length == 0 || rel.EndsWith("/")) continue;                    // a directory entry
                rel = rel.Replace('/', Path.DirectorySeparatorChar);
                string outPath = Path.GetFullPath(Path.Combine(platformsDir, rel));
                if (!outPath.StartsWith(Path.GetFullPath(platformsDir), StringComparison.OrdinalIgnoreCase)) continue; // zip-slip guard
                Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
                entry.ExtractToFile(outPath, overwrite: true);
                files++;
                versions.Add(rel.Split(Path.DirectorySeparatorChar)[0]);
            }
            log($"platforms ready: {files} files across {versions.Count} patch folders.");
        }
        finally { try { File.Delete(tmp); } catch { } }
    }

    /// <summary>
    /// Builds the base game folder from an existing Diablo II install: the archives and DLLs the game
    /// loads, hard-linked where possible so it costs no disk, copied otherwise. Skips the junk (bats,
    /// updaters, logs) the same way the runtime folders do.
    /// </summary>
    public static void BuildBase(string sourceDir, string baseDir, Action<string> log)
    {
        if (!Directory.Exists(sourceDir)) { log("that folder does not exist."); return; }
        if (!File.Exists(Path.Combine(sourceDir, "d2data.mpq")))
        {
            log("that folder has no d2data.mpq — pick the folder that holds your Diablo II archives.");
            return;
        }
        Directory.CreateDirectory(baseDir);
        int n = 0;
        foreach (var src in Directory.EnumerateFiles(sourceDir))
        {
            string name = Path.GetFileName(src);
            if (!PlatformSet.IsGameFilePublic(name)) continue;
            string dst = Path.Combine(baseDir, name);
            if (File.Exists(dst)) continue;
            if (!PlatformSet.HardLinkOrCopy(src, dst)) log($"could not bring in {name}");
            else n++;
        }
        log($"base folder ready: {n} game files from {Path.GetFileName(sourceDir)}.");
    }
}
