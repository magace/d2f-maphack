using System.Runtime.InteropServices;

namespace D2Chain.D2FUtil;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // Headless mode for scripts and tests:  D2FUtil.exe --launch "<Diablo II folder>" [-w -ns ...]
        // Uses the saved settings for the maphack switch; prints the pid, or the error, and exits.
        if (args.Length >= 2 && args[0] == "--launch")
        {
            AttachConsole(-1);
            // A WinExe may have no console to attach to, so the result also goes to launch.log.
            void Say(string line)
            {
                Console.WriteLine(line);
                try { Directory.CreateDirectory(Settings.Dir); File.AppendAllText(Path.Combine(Settings.Dir, "launch.log"), $"[{DateTime.Now:HH:mm:ss}] {line}{Environment.NewLine}"); } catch { }
            }
            var g = GameInstall.Inspect(args[1]);
            if (!g.IsGame) { Say($"no Game.exe in {args[1]}"); return 2; }
            var s = Settings.Load();
            try
            {
                string dll = s.PrepareDll();
                var p = Injector.Launch(g.Exe, string.Join(' ', args.Skip(2)), dll);
                Say($"launched pid {p.Id} ({g.Patch}{(g.MaphackSupported ? "" : ", no maphack table")}) - log: {Settings.LogPath}");
                return 0;
            }
            catch (Exception ex) { Say("launch failed: " + ex); return 1; }
        }

        ApplicationConfiguration.Initialize();
        // Everything in the window is sized in pixels and drawn by hand, so it has to be told what a
        // pixel is worth here: on a display at 150% this process is told the screen is 144 dots per
        // inch, and without that factor the window comes out two thirds of the size it should be with
        // the text, which is in points and scales on its own, overflowing every box around it.
        Theme.Init();
        Application.Run(new MainForm());
        return 0;
    }

    [DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);
}
