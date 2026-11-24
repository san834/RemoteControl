# 生成 docs/screenshot-client.png 的可复现脚本。
#
# 做法：本机起一个服务端（捕获当前桌面）+ 一个客户端（显示远端画面），
# 4 秒后截全屏，然后关掉两个进程。
#
# 注意：两个子进程都用 -RedirectStandardOutput 把输出重定向到文件，
# 否则它们会继承调用方的 stdout 管道，导致调用方等不到 EOF 而挂起。
#
# 用法（在仓库根目录）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture-screenshot.ps1

param(
    [string]$BuildDir = "build\Release",
    [int]$Port = 19999,
    [string]$OutFile = "docs\screenshot-client.png",
    [int]$WarmupSeconds = 4,
    [int]$Scale = 1,
    [string]$Codec = "auto"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$serverExe = Join-Path $BuildDir "rc_server.exe"
$clientExe = Join-Path $BuildDir "rc_client.exe"
foreach ($exe in @($serverExe, $clientExe)) {
    if (-not (Test-Path $exe)) { throw "找不到 $exe，请先构建 Release 配置" }
}

$logDir = Join-Path $BuildDir "capture-logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$outDir = Split-Path -Parent $OutFile
if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$server = $null
$client = $null
try {
    $server = Start-Process -FilePath $serverExe `
        -ArgumentList '--port', $Port, '--verbose', '--scale', $Scale, '--codec', $Codec `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $logDir "server.log") `
        -RedirectStandardError (Join-Path $logDir "server.err.log")

    Start-Sleep -Milliseconds 1500

    $client = Start-Process -FilePath $clientExe `
        -ArgumentList '--host', '127.0.0.1', '--port', $Port `
        -PassThru `
        -RedirectStandardOutput (Join-Path $logDir "client.out.log") `
        -RedirectStandardError (Join-Path $logDir "client.err.log")

    Start-Sleep -Seconds $WarmupSeconds

    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bitmap.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()

    Write-Output "已保存 $OutFile ($($bounds.Width)x$($bounds.Height))"
}
finally {
    foreach ($proc in @($client, $server)) {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
