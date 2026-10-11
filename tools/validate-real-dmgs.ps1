<#
  Real-world validation of ORVYN against official open-source release DMGs.

  - Downloads the DMGs from the projects' GitHub releases into .validation\dmg (git-ignored), verifying the
    pinned SHA-256 (the release assets are re-checked, not trusted by name).
  - NEVER executes anything from the images: they are only parsed / extracted.
  - For every image: `orvyn dmg-info`, `orvyn extract`, then an independent comparison:
      1. file tree + SHA-256 of every file vs a direct `7z x` extraction (same backend, different path:
         ORVYN's staged extraction vs 7-Zip's own nested extraction);
      2. Info.plist metadata (CFBundleIdentifier/Executable/Name/Version) vs Python's plistlib;
      3. every Mach-O slice vs live llvm-objdump (test_real_macho `real_world_*`).
  Usage:  pwsh tools\validate-real-dmgs.ps1 [-Build build\bin\Release] [-LlvmBin <dir>]
#>
param(
  [string]$Build = "build\bin\Release",
  [string]$LlvmBin = "",
  [string]$Work = ".validation"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
$orvyn = Join-Path $Build "orvyn.exe"
if (-not (Test-Path $orvyn)) { throw "build orvyn first: $orvyn not found" }
if (-not $LlvmBin) { $LlvmBin = (Get-ChildItem .toolchain\clang+llvm-*\bin -Directory -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }

# name, url, sha256 (as downloaded 2026-10-10)
$images = @(
  @{ name = "Rectangle2.0.3";       url = "https://github.com/rxhanson/Rectangle/releases/download/v2.0.3/Rectangle2.0.3.dmg";                         sha = "a48883b56fbe3fbbd699a05bc8ddbfce65c0c73c66f71eaebbe9f1fdc5a3dcb5" },
  @{ name = "Stats";                url = "https://github.com/exelban/stats/releases/download/v3.0.20/Stats.dmg";                                       sha = "ad48e0cd2dd0c9d8873831fa0285be09e19017885a54b57509c4768573f9dfbe" },
  @{ name = "MonitorControl.4.4.0"; url = "https://github.com/MonitorControl/MonitorControl/releases/download/v4.4.0/MonitorControl.4.4.0.dmg";      sha = "e058da598b92f6ca674f4398c0b6fa49b3b5567ccfddccf6c6a5b23c5c625612" }
)
New-Item -ItemType Directory -Force "$Work\dmg", "$Work\extracted", "$Work\direct" | Out-Null
$report = @()

foreach ($i in $images) {
  $dmg = "$Work\dmg\$($i.name).dmg"
  if (-not (Test-Path $dmg)) { Write-Host "downloading $($i.name)"; Invoke-WebRequest -Uri $i.url -OutFile $dmg }
  $sha = (Get-FileHash $dmg -Algorithm SHA256).Hash.ToLower()
  if ($sha -ne $i.sha) { Write-Warning "$($i.name): SHA-256 differs from the pinned value ($sha); the upstream asset changed"; }

  Write-Host "=== $($i.name)"
  & $orvyn dmg-info $dmg --json | Out-File "$Work\$($i.name).dmg-info.json" -Encoding utf8
  $out = "$Work\extracted\$($i.name)"
  if (Test-Path $out) { Remove-Item -Recurse -Force $out }
  & $orvyn extract $dmg -o $out --json | Out-File "$Work\$($i.name).extract.json" -Encoding utf8
  if ($LASTEXITCODE -ne 0) { $report += "$($i.name): orvyn extract FAILED (exit $LASTEXITCODE)"; continue }
  $ext = Get-Content "$Work\$($i.name).extract.json" -Raw | ConvertFrom-Json

  # 1. independent extraction with 7-Zip's own nested extraction
  $direct = "$Work\direct\$($i.name)"
  if (Test-Path $direct) { Remove-Item -Recurse -Force $direct }
  & 7z x -y -bd "-o$direct" $dmg | Out-Null
  $volume = (Get-ChildItem $direct -Directory | Select-Object -First 1).FullName
  $cm = Join-Path $out ($ext.partitions[0].directory)
  $cmVol = (Get-ChildItem $cm -Directory | Select-Object -First 1).FullName
  function Hashes($dir) {
    $prefix = "\\?\" + (Resolve-Path $dir).Path
    Get-ChildItem -LiteralPath $prefix -Recurse -File -Force | Where-Object { -not $_.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) } |
      ForEach-Object { [pscustomobject]@{ rel = $_.FullName.Substring($prefix.Length).ToLower(); sha = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } }
  }
  $a = Hashes $cmVol; $b = Hashes $volume
  $ha = @{}; $a | ForEach-Object { $ha[$_.rel] = $_.sha }
  $hb = @{}; $b | ForEach-Object { $hb[$_.rel] = $_.sha }
  $onlyOrvyn = @($ha.Keys | Where-Object { -not $hb.ContainsKey($_) })
  $onlyDirect = @($hb.Keys | Where-Object { -not $ha.ContainsKey($_) })
  $diff = @($ha.Keys | Where-Object { $hb.ContainsKey($_) -and $ha[$_] -ne $hb[$_] })
  $report += ("{0}: files orvyn={1} direct-7z={2} | only-in-orvyn={3} only-in-direct={4} content-differs={5}" -f $i.name, $ha.Count, $hb.Count, $onlyOrvyn.Count, $onlyDirect.Count, $diff.Count)
  if ($onlyDirect.Count) { $report += "   only in direct 7z (first 5): " + (($onlyDirect | Select-Object -First 5) -join "; ") }

  # 2. Info.plist metadata vs Python plistlib
  foreach ($app in $ext.apps) {
    $plist = Join-Path (Join-Path $out $app) "Contents\Info.plist"
    $py = & python -I -c "import plistlib,json,sys; d=plistlib.load(open(sys.argv[1],'rb')); print(json.dumps({k:str(d.get(k,'')) for k in ['CFBundleIdentifier','CFBundleExecutable','CFBundleName','CFBundleVersion','CFBundleShortVersionString']}))" ("\\?\" + (Resolve-Path $plist).Path)
    $insp = & $orvyn inspect (Join-Path $out $app) --json | ConvertFrom-Json
    $p = $py | ConvertFrom-Json
    $ok = ($p.CFBundleIdentifier -eq $insp.bundle.identifier) -and ($p.CFBundleExecutable -eq $insp.bundle.executable) -and
          ($p.CFBundleVersion -eq $insp.bundle.version) -and ($p.CFBundleShortVersionString -eq $insp.bundle.short_version)
    $report += ("{0}: Info.plist orvyn vs plistlib {1} (id={2}, exe={3}, v={4})" -f $app, $(if ($ok) { "MATCH" } else { "MISMATCH" }), $p.CFBundleIdentifier, $p.CFBundleExecutable, $p.CFBundleShortVersionString)
  }

  # 3. Mach-O slices vs live llvm-objdump
  if ($LlvmBin) {
    $env:ORVYN_LLVM_BIN = $LlvmBin
    $env:ORVYN_REAL_APPS_DIR = (Resolve-Path $out).Path
    $t = & (Join-Path $Build "test_real_macho.exe") real_world 2>&1
    $report += "objdump: " + (($t | Select-String "real-world|FAILED|\[ ok|\[FAIL") -join " | ")
  } else { $report += "objdump comparison skipped (no LLVM)" }
}

"`n==== SUMMARY ===="
$report
$report | Out-File "$Work\validation-summary.txt" -Encoding utf8
