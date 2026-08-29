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
    $zipName = "FFXIV_MOD_Options_Chinese_AI-Translated_v$ver.zip"
    $zipFile = Join-Path $zipDir $zipName
    if (Test-Path $zipFile) { Remove-Item $zipFile -Force }
    Compress-Archive -Path (Join-Path $pub '*') -DestinationPath $zipFile -CompressionLevel Optimal
    Write-Host "==> Zip created: $zipFile  ($((Get-Item $zipFile).Length) bytes)"
}
Write-Host "[DONE] All done."
exit 0
