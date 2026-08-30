# One-click release build for FFXIV MOD Hanhua Tool
# Default: build Release x64 -> copy exe into release\
# Add -Zip to also compress the release folder into a zip.
param(
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$msbuild = 'E:\Program Files\Microsoft Visual Studio\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) { $msbuild = $cmd.Source }
}
if (-not (Test-Path $msbuild)) {
    Write-Host "[ERROR] MSBuild not found."
    exit 1
}

$proj = Get-ChildItem $root -Filter '*.vcxproj' | Select-Object -First 1
if (-not $proj) {
    Write-Host "[ERROR] No .vcxproj found in $root"
    exit 1
}

# Step 1: build Release x64
Write-Host "==> Building Release x64 ..."
& $msbuild $proj.FullName /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /nologo
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed, exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

# Step 2: copy exe into release folder
$relDir = Join-Path $root 'x64\Release'
$relExe = Get-ChildItem $relDir -Filter '*.exe' | Select-Object -First 1
if (-not $relExe) {
    Write-Host "[ERROR] Release exe not found."
    exit 1
}
$pub = Join-Path $root 'release'
if (-not (Test-Path $pub)) { New-Item -ItemType Directory -Path $pub | Out-Null }
Copy-Item $relExe.FullName (Join-Path $pub $relExe.Name) -Force
Write-Host "==> Copied exe to release folder."

# Step 2b: copy default config into release (zip ships with config.default.json)
$cfgDefault = Join-Path $root 'config.default.json'
if (Test-Path $cfgDefault) {
    Copy-Item $cfgDefault (Join-Path $pub 'config.default.json') -Force
    Write-Host "==> Copied config.default.json to release folder."
} else {
    Write-Host "[WARN] config.default.json not found in project root; zip will not ship default config."
}

# Step 2c: copy built-in wiki dictionary into release (first-run seed for dictionary folder)
$wikiName = -join [char[]](0x5185, 0x7F6E, 0x77, 0x69, 0x6B, 0x69, 0x5F, 0x672F, 0x8BED, 0x5BF9, 0x7167, 0x2E, 0x6A, 0x73, 0x6F, 0x6E)  # 内置wiki_术语对照.json
$builtinWiki = Join-Path $root $wikiName
if (Test-Path $builtinWiki) {
    Copy-Item $builtinWiki (Join-Path $pub $wikiName) -Force
    Write-Host "==> Copied $wikiName to release folder."
} else {
    Write-Host "[WARN] Built-in wiki seed not found in project root; first-run wiki seed will be missing."
}

# Step 2d: ensure an empty 日志.json template ships in the zip (the app auto-creates/overwrites it on every run)
$logName = -join [char[]](0x65E5, 0x5FD7, 0x2E, 0x6A, 0x73, 0x6F, 0x6E)  # 日志.json
$logTpl = Join-Path $pub $logName
if (-not (Test-Path $logTpl)) {
    Set-Content -Path $logTpl -Value '[]' -Encoding UTF8
    Write-Host "==> Created empty $logName template in release folder."
}

# Step 3 (optional): zip the release folder content, only with -Zip
if ($Zip) {
    # Resolve current version from README changelog (first "### vX.Y.Z" heading)
    $readme = Join-Path $root 'README.md'
    $ver = $null
    if (Test-Path $readme) {
        $m = Select-String -Path $readme -Pattern '###\s+v(\d+\.\d+(\.\d+)*)' | Select-Object -First 1
        if ($m -and $m.Matches.Count -gt 0 -and $m.Matches[0].Groups[1]) { $ver = $m.Matches[0].Groups[1].Value }
    }
    if (-not $ver) {
        Write-Host "[ERROR] Cannot resolve version from README.md changelog (expect '### vX.Y.Z')."
        exit 1
    }
    # Output dir: D:\Fast folder\Downloads\压缩文件 (built via code points to avoid script-encoding issues)
    $zipDirName = -join [char[]](0x538B, 0x7F29, 0x6587, 0x4EF6)
    $zipDir = Join-Path 'D:\Fast folder\Downloads' $zipDirName
    if (-not (Test-Path $zipDir)) { New-Item -ItemType Directory -Path $zipDir | Out-Null }

    # Zip contains a top-level Chinese folder without version, so extracting to any
    # location yields a clean "最终幻想14_mod选项汉化工具_AI翻译版\" directory.
    # The name is built from code points because this script may be decoded as ANSI.
    $innerName = -join [char[]](0x6700,0x7EC8,0x5E7B,0x60F3,0x31,0x34,0x5F,0x6D,0x6F,0x64,0x9009,0x9879,0x6C49,0x5316,0x5DE5,0x5177,0x5F,0x41,0x49,0x7FFB,0x8BD1,0x7248)
    $stage = Join-Path $env:TEMP ("ffxiv_mod_pkg_" + [guid]::NewGuid().ToString('N'))
    $inner = Join-Path $stage $innerName
    New-Item -ItemType Directory -Path $inner -Force | Out-Null
    Copy-Item (Join-Path $pub '*') $inner -Recurse -Force

    # Pick a zip file name; if it already exists, append _0/_1/... instead of overwriting
    $zipFile = Join-Path $zipDir "FFXIV_MOD_Options_Chinese_AI-Translated_v$ver.zip"
    $cand = $zipFile
    $n = 0
    while (Test-Path $cand) {
        $cand = Join-Path $zipDir "FFXIV_MOD_Options_Chinese_AI-Translated_v${ver}_${n}.zip"
        $n++
    }
    $zipFile = $cand

    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipFile -CompressionLevel Optimal
    Remove-Item $stage -Recurse -Force
    Write-Host "==> Zip created: $zipFile  ($((Get-Item $zipFile).Length) bytes)"
}
Write-Host "[DONE] All done."
exit 0
