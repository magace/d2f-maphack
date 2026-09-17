# Builds D2F Utilities: the 32-bit in-game DLL first (MSVC), then the self-contained x86 launcher
# that embeds it. Output: d2futil\D2FUtil\publish\D2FUtil.exe — one file, nothing to install.
#
#   .\build.ps1            build everything
#   .\build.ps1 -DllOnly   just recompile native\d2fmh.dll
param([switch]$DllOnly)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

Write-Host "== native: d2fmh.dll (x86)"
& cmd /c "`"$root\native\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw "native build failed" }
if ($DllOnly) { return }

Write-Host "== launcher: D2FUtil.exe (win-x86, self-contained, single file)"
$out = Join-Path $root 'D2FUtil\publish'
& dotnet publish "$root\D2FUtil\D2FUtil.csproj" -c Release -o $out --nologo -v minimal
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed" }

$exe = Join-Path $out 'D2FUtil.exe'
Write-Host ("== done: {0}  ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))
