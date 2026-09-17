using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace D2Chain.D2FUtil;

/// <summary>
/// Starts a Diablo II client with d2fmh.dll loaded before the game runs a single instruction.
///
/// <para>CreateProcess(CREATE_SUSPENDED) → write the DLL path into the child → CreateRemoteThread on
/// kernel32!LoadLibraryA → wait → resume. LoadLibraryA's address is taken from OUR kernel32, which is
/// valid in the child because every 32-bit process maps kernel32 at the same base — the reason this
/// tool is built x86 (see D2FUtil.csproj).</para>
/// </summary>
internal static class Injector
{
    public static Process Launch(string exe, string args, string dllPath)
    {
        string cmd = $"\"{exe}\" {args}".TrimEnd();
        var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
        PROCESS_INFORMATION pi;
        if (!CreateProcess(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, CREATE_SUSPENDED,
                IntPtr.Zero, Path.GetDirectoryName(exe), ref si, out pi))
        {
            int err = Marshal.GetLastWin32Error();
            if (err != ERROR_ELEVATION_REQUIRED)
                throw new Win32Exception(err, "CreateProcess failed");
            // Game.exe is marked "run as administrator" (a compatibility flag many old installs carry).
            // CreateProcess cannot elevate, and we do not need it to: RunAsInvoker makes Windows ignore
            // the flag for this launch. The child inherits our environment, so set it on ourselves.
            Environment.SetEnvironmentVariable("__COMPAT_LAYER", "RunAsInvoker");
            if (!CreateProcess(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, CREATE_SUSPENDED,
                    IntPtr.Zero, Path.GetDirectoryName(exe), ref si, out pi))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Game.exe insists on administrator rights; run D2F Utilities as administrator");
        }

        try
        {
            byte[] path = Encoding.Default.GetBytes(dllPath + "\0");
            IntPtr remote = VirtualAllocEx(pi.hProcess, IntPtr.Zero, (uint)path.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "VirtualAllocEx failed");
            if (!WriteProcessMemory(pi.hProcess, remote, path, (uint)path.Length, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory failed");

            IntPtr loadLibrary = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryA");
            IntPtr thread = CreateRemoteThread(pi.hProcess, IntPtr.Zero, 0, loadLibrary, remote, 0, out _);
            if (thread == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateRemoteThread failed");
            WaitForSingleObject(thread, 10000);
            GetExitCodeThread(thread, out uint hmod);
            CloseHandle(thread);
            VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
            if (hmod == 0) throw new InvalidOperationException("the game refused to load d2fmh.dll (LoadLibrary returned 0)");

            ResumeThread(pi.hThread);
            return Process.GetProcessById((int)pi.dwProcessId);
        }
        catch
        {
            TerminateProcess(pi.hProcess, 1);
            throw;
        }
        finally
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    const uint CREATE_SUSPENDED = 0x4;
    const int ERROR_ELEVATION_REQUIRED = 740;
    const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000;
    const uint PAGE_READWRITE = 0x04;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    struct STARTUPINFO
    {
        public int cb; public string? lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public uint dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    static extern bool CreateProcess(string? app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
        IntPtr env, string? cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, uint size, uint type, uint prot);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, uint size, uint type);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, uint size, out uint written);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, uint stack, IntPtr start, IntPtr param, uint flags, out uint tid);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)] static extern IntPtr GetProcAddress(IntPtr mod, string name);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
}
