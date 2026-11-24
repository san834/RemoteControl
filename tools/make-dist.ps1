# Build the folder that gets copied into the VM / another machine.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-dist.ps1
#
# Output: dist\ -- two exes, two double-click launchers, one readme.
# The exes are statically linked against the MSVC CRT (/MT), so the target
# machine needs no Visual Studio and no VC++ redistributable.
#
# NOTE: this script is deliberately ASCII-only. Windows PowerShell 5.1 reads
# .ps1 files in the ANSI code page unless they have a UTF-8 BOM, and a stray
# multi-byte sequence would break parsing. Chinese text lives in
# tools\dist-readme.txt instead.

param(
    [string]$BuildDir = "build\Release",
    [string]$OutDir = "dist",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$server = Join-Path $BuildDir "rc_server.exe"
$client = Join-Path $BuildDir "rc_client.exe"
foreach ($exe in @($server, $client)) {
    if (-not (Test-Path $exe)) {
        throw "Missing $exe -- run: cmake --build build --config $Config"
    }
}

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Copy-Item $server, $client -Destination $OutDir

$launcherServer = @(
    '@echo off',
    'cd /d "%~dp0"',
    'rc_server.exe %*',
    'pause'
) -join "`r`n"
Set-Content -Path (Join-Path $OutDir "run-server.bat") -Value $launcherServer -Encoding ASCII

$launcherClient = @(
    '@echo off',
    'cd /d "%~dp0"',
    'set HOST=%~1',
    'if "%HOST%"=="" set /p HOST=Server IP [127.0.0.1]: ',
    'if "%HOST%"=="" set HOST=127.0.0.1',
    'rc_client.exe --host %HOST%'
) -join "`r`n"
Set-Content -Path (Join-Path $OutDir "run-client.bat") -Value $launcherClient -Encoding ASCII

# Readme: UTF-8 with BOM so Notepad on any Windows version renders it correctly.
$readmePath = Join-Path $PSScriptRoot "dist-readme.txt"
if (Test-Path $readmePath) {
    $text = Get-Content -Path $readmePath -Raw -Encoding UTF8
    $utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path $OutDir "readme-cn.txt"), $text, $utf8Bom)
}

Write-Output "dist ready:"
Get-ChildItem $OutDir | ForEach-Object {
    "  {0,-22} {1,8:N0} KB" -f $_.Name, ($_.Length / 1KB)
}
