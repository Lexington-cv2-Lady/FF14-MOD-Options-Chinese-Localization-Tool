# One-click release packaging for FFXIV MOD Hanhua Tool
# Steps: build Release x64 -> copy exe into release\ -> compress zip
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

# Step 3: zip the release folder content
$zip = Join-Path $root 'FFXIV_Mod_Hanhua_v1.0_Green.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $pub '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "==> Zip created: $zip  ($((Get-Item $zip).Length) bytes)"
Write-Host "[DONE] All done."
exit 0
