param(
  [string]$DistDir              = (Join-Path $PSScriptRoot "dist"),
  [string]$OutDir               = (Join-Path $PSScriptRoot "installer"),
  # MSIX requires a 4-part version (Major.Minor.Build.Revision).
  [string]$Version              = "1.0.0.0",
  [string]$DisplayName          = "Ultralight IP CAM Viewer Recorder",
  # Identity.Name — Store-reserved name once you reserve one in Partner Center;
  # any valid reverse-domain-style string for sideload testing.
  [string]$PackageName          = "Slava.UltralightIPCAMViewerRecorder",
  # Identity.Publisher — must be a literal X.509 Subject (CN=...) and must match
  # exactly whatever your signing certificate uses, otherwise Add-AppxPackage
  # will refuse to install. For Store submission, use the CN value that Partner
  # Center assigned you (it's a GUID).
  [string]$Publisher            = "CN=Slava",
  [string]$PublisherDisplayName = "Slava",
  # Skip signing entirely (use this when producing an MSIX for Store upload —
  # the Store re-signs your package on ingest).
  [switch]$NoSign,
  # Optional: use your own .pfx instead of the auto-generated self-signed cert.
  [string]$CertPfx              = $null,
  [string]$CertPassword         = $null
)

$ErrorActionPreference = "Stop"

# --- 0. Validate manifest-sensitive inputs --------------------------------
# If the user pasted a Package Family Name by mistake (Identity.Name + an
# underscore + 13-char base32 publisher hash), strip the hash suffix
# automatically. Partner Center's "App identity" page shows both names side by
# side and the family name is the visually-longer one, so confusion is common.
if ($PackageName -match '^(.+)_[a-z0-9]{13}$') {
  $orig = $PackageName
  $PackageName = $matches[1]
  Write-Host "Note: -PackageName '$orig' looks like a Package Family Name."        -ForegroundColor Yellow
  Write-Host "       The trailing '_<hash>' is derived from the publisher CN and"  -ForegroundColor Yellow
  Write-Host "       must not be in Identity.Name. Using Identity.Name = '$PackageName'." -ForegroundColor Yellow
}

# Identity.Name allowed chars: letters / digits / dots / hyphens. 3..50 chars.
# (Reverse-domain style; no spaces, no underscores.) -DisplayName is the
# user-visible string and the place where spaces / Cyrillic / etc. belong.
if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9.\-]{2,49}$') {
  throw @"
-PackageName '$PackageName' is not a valid MSIX Identity.Name.
Must be 3-50 chars: letters/digits/dots/hyphens only — no spaces, no underscores.
Partner Center → your app → App identity → Package/Identity/Name is the value to
copy (NOT the longer Package Family Name, which ends in '_<13-char-hash>').
The visible product name with spaces goes in -DisplayName, not here.
"@
}

# Identity.Publisher must be a real X.500 subject (CN=...) — no angle brackets
# or other XML-illegal characters; it has to byte-for-byte match the subject of
# the signing cert (or the CN your Partner Center reservation assigned you).
if ($Publisher -match '[<>]') {
  throw @"
-Publisher '$Publisher' looks like the placeholder text from the help, not a real
X.500 subject. Replace it with the actual CN (Partner Center → your reservation
→ App identity → Publisher copy-paste). For local sideload testing 'CN=Slava' or
any literal CN=<your-name> works.
"@
}
if ($Publisher -notmatch '^CN=') {
  throw "-Publisher must start with 'CN=' (full X.500 subject). Got: '$Publisher'"
}

# Version must be 4-part numeric.
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
  throw "-Version must be Major.Minor.Build.Revision (e.g. 1.0.0.0). Got: '$Version'"
}

# --- 1. Verify the three runtime files are present ------------------------
$required = @("Ultralight_IP_CAM_Viewer_Recorder.exe", "SDL2.dll", "libwinpthread-1.dll")
foreach ($f in $required) {
  $p = Join-Path $DistDir $f
  if (-not (Test-Path $p)) {
    throw "Missing: $p`nRun .\build_min_portable.ps1 first to produce the runtime files."
  }
}

# --- 2. Locate makeappx.exe and signtool.exe in the Windows SDK -----------
function Find-SdkTool {
  param([string]$Tool)
  $roots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
    "${env:ProgramFiles}\Windows Kits\10\bin"
  ) | Where-Object { Test-Path $_ }
  foreach ($root in $roots) {
    $versions = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
      Sort-Object { [Version]$_.Name } -Descending
    foreach ($v in $versions) {
      foreach ($arch in @("x64", "x86")) {
        $candidate = Join-Path $v.FullName "$arch\$Tool"
        if (Test-Path $candidate) { return $candidate }
      }
    }
    # Some installs put the tools directly in bin\ (no version subdir).
    foreach ($arch in @("x64", "x86")) {
      $candidate = Join-Path $root "$arch\$Tool"
      if (Test-Path $candidate) { return $candidate }
    }
  }
  $cmd = Get-Command $Tool -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

$makeappx = Find-SdkTool "makeappx.exe"
$signtool = Find-SdkTool "signtool.exe"
if (-not $makeappx) {
  throw @"
makeappx.exe not found. Install the Windows 10/11 SDK:
  - Stand-alone: https://developer.microsoft.com/windows/downloads/windows-sdk/
  - Via Visual Studio Installer: select 'Windows 10/11 SDK' under Individual Components.
"@
}
if (-not $signtool -and -not $NoSign) {
  throw "signtool.exe not found. Same install as makeappx — it ships in the same SDK."
}
Write-Host "makeappx: $makeappx" -ForegroundColor Cyan
if ($signtool) { Write-Host "signtool: $signtool" -ForegroundColor Cyan }

# --- 3. Stage a clean working tree ---------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage     = Join-Path $OutDir "msix-stage"
$assetsDir = Join-Path $stage  "Assets"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage     | Out-Null
New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null

foreach ($f in $required) {
  Copy-Item -Force (Join-Path $DistDir $f) (Join-Path $stage $f)
}

# --- 4. Generate (or copy) the required PNG assets ------------------------
# If you've authored real artwork, drop matching files into assets\msix\ in
# the project root and they'll be used instead of the placeholders.
function Make-PlaceholderPng {
  param([string]$OutPath, [int]$W, [int]$H)
  Add-Type -AssemblyName System.Drawing
  $bmp  = New-Object System.Drawing.Bitmap($W, $H)
  $g    = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode    = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias

  $bg     = [System.Drawing.Color]::FromArgb(14, 61, 47)        # matches BackgroundColor
  $accent = [System.Drawing.Color]::FromArgb(80, 220, 100)
  $g.Clear($bg)

  $pen = New-Object System.Drawing.Pen($accent, 3)
  $g.DrawRectangle($pen, 2, 2, $W - 5, $H - 5)

  $fontSize = [int]([Math]::Max(10, [Math]::Min($W, $H) / 4))
  $font     = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold)
  $sf       = New-Object System.Drawing.StringFormat
  $sf.Alignment     = [System.Drawing.StringAlignment]::Center
  $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
  $g.DrawString("CAM", $font, [System.Drawing.Brushes]::White,
                (New-Object System.Drawing.RectangleF(0, 0, $W, $H)), $sf)

  $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $font.Dispose(); $pen.Dispose(); $g.Dispose(); $bmp.Dispose()
}

$pngSpecs = @(
  @{ Name = "Square44x44Logo.png";   W = 44;  H = 44  },
  @{ Name = "Square150x150Logo.png"; W = 150; H = 150 },
  @{ Name = "Wide310x150Logo.png";   W = 310; H = 150 },
  @{ Name = "StoreLogo.png";         W = 50;  H = 50  },
  @{ Name = "SplashScreen.png";      W = 620; H = 300 }
)
$userAssetsDir = Join-Path $PSScriptRoot "assets\msix"
foreach ($p in $pngSpecs) {
  $dst     = Join-Path $assetsDir $p.Name
  $userPng = Join-Path $userAssetsDir $p.Name
  if (Test-Path $userPng) {
    Copy-Item -Force $userPng $dst
    Write-Host "  asset (user) $($p.Name)" -ForegroundColor DarkGray
  } else {
    Make-PlaceholderPng -OutPath $dst -W $p.W -H $p.H
    Write-Host "  asset (gen)  $($p.Name) $($p.W)x$($p.H)" -ForegroundColor DarkGray
  }
}

# --- 5. AppxManifest.xml --------------------------------------------------
$xmlEscape = {
  param([string]$s)
  return $s.Replace('&','&amp;').Replace('<','&lt;').Replace('>','&gt;').Replace('"','&quot;')
}
$xDisp    = & $xmlEscape $DisplayName
$xPub     = & $xmlEscape $PublisherDisplayName

$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package
    xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
    xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
    xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
    IgnorableNamespaces="uap rescap">

  <Identity Name="$PackageName"
            Publisher="$Publisher"
            Version="$Version"
            ProcessorArchitecture="x64" />

  <Properties>
    <DisplayName>$xDisp</DisplayName>
    <PublisherDisplayName>$xPub</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>

  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.22621.0" />
  </Dependencies>

  <Resources>
    <Resource Language="en-US" />
    <Resource Language="ru-RU" />
  </Resources>

  <Applications>
    <Application Id="App"
                 Executable="Ultralight_IP_CAM_Viewer_Recorder.exe"
                 EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="$xDisp"
                          Description="$xDisp"
                          BackgroundColor="#0E3D2F"
                          Square150x150Logo="Assets\Square150x150Logo.png"
                          Square44x44Logo="Assets\Square44x44Logo.png">
        <uap:DefaultTile Wide310x150Logo="Assets\Wide310x150Logo.png" />
        <uap:SplashScreen Image="Assets\SplashScreen.png" BackgroundColor="#0E3D2F" />
      </uap:VisualElements>
    </Application>
  </Applications>

  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
    <Capability Name="internetClient" />
    <Capability Name="privateNetworkClientServer" />
  </Capabilities>
</Package>
"@
$manifestPath = Join-Path $stage "AppxManifest.xml"
Set-Content -Path $manifestPath -Value $manifest -Encoding UTF8
Write-Host "Wrote: $manifestPath" -ForegroundColor Cyan

# --- 6. Pack the MSIX -----------------------------------------------------
$msixPath = Join-Path $OutDir ("Ultralight_IP_CAM_Viewer_Recorder_" + $Version + ".msix")
if (Test-Path $msixPath) { Remove-Item -Force $msixPath }

Write-Host "Packing MSIX..." -ForegroundColor Cyan
& $makeappx pack /h SHA256 /d $stage /p $msixPath /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed (exit $LASTEXITCODE)" }

# --- 7. Sign (sideload testing only; Store re-signs on ingest) -----------
if (-not $NoSign) {
  $pfx        = $CertPfx
  $pfxPassword = $CertPassword

  if (-not $pfx) {
    # Auto-generate a self-signed cert. Subject *must* match Identity.Publisher
    # exactly, otherwise Add-AppxPackage rejects the signed package.
    $pfx          = Join-Path $OutDir "msix-test-cert.pfx"
    $pfxPassword  = "test123"

    if (-not (Test-Path $pfx)) {
      Write-Host "Generating self-signed code-signing cert ($Publisher)..." -ForegroundColor Yellow
      $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Publisher `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA -KeyLength 2048 `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -NotAfter (Get-Date).AddYears(2)
      $secure = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
      Export-PfxCertificate `
        -Cert ("Cert:\CurrentUser\My\" + $cert.Thumbprint) `
        -FilePath $pfx `
        -Password $secure | Out-Null
      Write-Host "Cert saved to: $pfx (password: $pfxPassword)" -ForegroundColor DarkGray
    }
  }

  Write-Host "Signing MSIX with $pfx ..." -ForegroundColor Cyan
  if ($pfxPassword) {
    & $signtool sign /fd SHA256 /a /f $pfx /p $pfxPassword $msixPath
  } else {
    & $signtool sign /fd SHA256 /a /f $pfx $msixPath
  }
  if ($LASTEXITCODE -ne 0) { throw "signtool failed (exit $LASTEXITCODE)" }
}

$sizeMB = [math]::Round((Get-Item $msixPath).Length / 1MB, 2)

Write-Host ""
Write-Host "MSIX ready:" -ForegroundColor Green
Write-Host "  $msixPath ($sizeMB MB)" -ForegroundColor Green
Write-Host ""
Write-Host "Sideload test on this machine:" -ForegroundColor DarkGray
if (-not $NoSign) {
  $pfxAbs = (Join-Path $OutDir 'msix-test-cert.pfx')
  Write-Host "  1. Trust the test cert ONCE (run from elevated PowerShell):" -ForegroundColor DarkGray
  Write-Host "       Import-PfxCertificate -FilePath '$pfxAbs' -CertStoreLocation Cert:\LocalMachine\TrustedPeople -Password (ConvertTo-SecureString 'test123' -AsPlainText -Force)" -ForegroundColor DarkGray
}
Write-Host "  2. Install: Add-AppxPackage -Path '$msixPath'" -ForegroundColor DarkGray
Write-Host "  3. Launch via Start Menu (search '$DisplayName')." -ForegroundColor DarkGray
Write-Host ""
Write-Host "Store submission:" -ForegroundColor DarkGray
Write-Host "  1. Reserve a name in Partner Center; from 'App identity' copy these" -ForegroundColor DarkGray
Write-Host "     three values: Package/Identity/Name, Package/Identity/Publisher," -ForegroundColor DarkGray
Write-Host "     and Package/Properties/PublisherDisplayName." -ForegroundColor DarkGray
Write-Host "  2. Re-run, substituting your real values (the literals shown here are" -ForegroundColor DarkGray
Write-Host "     illustration only and will be rejected as invalid):" -ForegroundColor DarkGray
Write-Host "       .\make_msix.ps1 -NoSign -PackageName 'YourCompany.YourApp' -Publisher 'CN=11111111-2222-3333-4444-555555555555' -PublisherDisplayName 'Your Company'" -ForegroundColor DarkGray
Write-Host "  3. Upload the produced .msix in Partner Center -> Packages." -ForegroundColor DarkGray
