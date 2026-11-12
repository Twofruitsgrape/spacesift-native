# 一键安装 SpaceSift（当前用户，不需要管理员权限）
#
# 做三件事：
#   1. 把程序拷到 %LOCALAPPDATA%\Programs\SpaceSift
#   2. 建桌面快捷方式
#   3. 建开始菜单快捷方式
#
# 卸载：删掉上面那个目录和两个快捷方式即可（本脚本不写注册表）。

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$src  = Split-Path -Parent $MyInvocation.MyCommand.Path
$dest = Join-Path $env:LOCALAPPDATA 'Programs\SpaceSift'
$exe  = Join-Path $dest 'SpaceSift.exe'

Write-Host ''
Write-Host '  SpaceSift 内容检索版 · 安装' -ForegroundColor Cyan
Write-Host '  --------------------------------'
Write-Host "  源目录：$src"
Write-Host "  安装到：$dest"
Write-Host ''

if (-not (Test-Path (Join-Path $src 'SpaceSift.exe'))) {
    Write-Host '  错误：当前目录下找不到 SpaceSift.exe' -ForegroundColor Red
    Write-Host '  请把本脚本和程序放在同一个文件夹里再运行。'
    exit 1
}

# 如果程序正在运行，先提示退出
$running = Get-Process -Name 'SpaceSift' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host '  检测到 SpaceSift 正在运行，正在关闭…' -ForegroundColor Yellow
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null

# 拷文件（排除脚本自己）
$files = Get-ChildItem -Path $src -Recurse -File | Where-Object {
    $_.Extension -ne '.ps1' -and $_.Name -ne 'install.bat' -and $_.Name -ne 'README.txt'
}
foreach ($f in $files) {
    $rel = $f.FullName.Substring($src.Length).TrimStart('\')
    $dst = Join-Path $dest $rel
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Copy-Item -Path $f.FullName -Destination $dst -Force
}
Write-Host '  文件已复制。' -ForegroundColor Green

# 快捷方式
$shell = New-Object -ComObject WScript.Shell
$targets = @(
    (Join-Path ([Environment]::GetFolderPath('Desktop')) 'SpaceSift.lnk'),
    (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SpaceSift.lnk')
)
foreach ($lnkPath in $targets) {
    $lnk = $shell.CreateShortcut($lnkPath)
    $lnk.TargetPath       = $exe
    $lnk.WorkingDirectory = $dest
    $lnk.IconLocation     = "$exe,0"
    $lnk.Description      = 'SpaceSift 磁盘空间分析与内容检索'
    $lnk.Save()
}
Write-Host '  桌面 / 开始菜单快捷方式已创建。' -ForegroundColor Green

Write-Host ''
Write-Host '  安装完成。' -ForegroundColor Cyan
Write-Host '  从桌面或开始菜单打开 SpaceSift 即可。'
Write-Host ''
$open = Read-Host '  现在打开吗？(Y/n)'
if ($open -eq '' -or $open -eq 'Y' -or $open -eq 'y') {
    Start-Process -FilePath $exe -WorkingDirectory $dest
}
