param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Find-MSBuild {
    $candidates = @()

    $pathMsbuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
    if ($pathMsbuild) {
        $candidates += $pathMsbuild
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installPaths = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPaths) {
            foreach ($installPath in $installPaths) {
                $candidates += Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
                $candidates += Join-Path $installPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
            }
        }
    }

    $candidates += @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe')
    )

    return $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
}

$msbuild = Find-MSBuild
$isccCandidates = @(
    @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
)

if (!$msbuild) {
    throw 'MSBuild was not found. Install Visual Studio Build Tools or add MSBuild.exe to PATH.'
}
if ($isccCandidates.Count -eq 0) {
    throw 'Inno Setup compiler ISCC.exe was not found.'
}
$iscc = $isccCandidates[0]

$webViewVersion = '150.0.4078.48'
$webViewCabUrl = 'https://msedge.sf.dl.delivery.mp.microsoft.com/filestreamingservice/files/60926d99-f201-46bb-91a0-d868dc06b275/Microsoft.WebView2.FixedVersionRuntime.150.0.4078.48.x64.cab'
$webViewCache = Join-Path $root 'third_party\webview2-runtime'
$webViewCab = Join-Path $webViewCache "Microsoft.WebView2.FixedVersionRuntime.$webViewVersion.x64.cab"
$webViewExtract = Join-Path $webViewCache "Microsoft.WebView2.FixedVersionRuntime.$webViewVersion.x64"

New-Item -ItemType Directory -Force -Path $webViewCache | Out-Null
if (!(Test-Path -LiteralPath $webViewCab)) {
    Write-Host "Downloading WebView2 Fixed Runtime $webViewVersion x64..."
    Invoke-WebRequest -Uri $webViewCabUrl -OutFile $webViewCab
}

if (!(Test-Path -LiteralPath (Join-Path $webViewExtract 'msedgewebview2.exe'))) {
    if (Test-Path -LiteralPath $webViewExtract) {
        Remove-Item -LiteralPath $webViewExtract -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $webViewExtract | Out-Null
    Write-Host "Extracting WebView2 Fixed Runtime..."
    & "$env:SystemRoot\System32\expand.exe" -F:* $webViewCab $webViewExtract | Out-Host
    if (!(Test-Path -LiteralPath (Join-Path $webViewExtract 'msedgewebview2.exe'))) {
        $nested = Get-ChildItem -LiteralPath $webViewExtract -Recurse -Filter msedgewebview2.exe |
            Select-Object -ExpandProperty DirectoryName -First 1
        if (!$nested) {
            throw 'Extracted WebView2 runtime does not contain msedgewebview2.exe.'
        }
        if ($nested -ne $webViewExtract) {
            $flattened = Join-Path $webViewCache "flat-$webViewVersion"
            if (Test-Path -LiteralPath $flattened) {
                Remove-Item -LiteralPath $flattened -Recurse -Force
            }
            New-Item -ItemType Directory -Force -Path $flattened | Out-Null
            Copy-Item -Path (Join-Path $nested '*') -Destination $flattened -Recurse -Force
            Remove-Item -LiteralPath $webViewExtract -Recurse -Force
            Move-Item -LiteralPath $flattened -Destination $webViewExtract
        }
    }
}

Write-Host "Building web UI..."
$webUiRoot = Join-Path $root 'src\AnvilPlayer.App\webui'
Push-Location $webUiRoot
try {
    npm run build
    if ($LASTEXITCODE -ne 0) {
        throw "npm run build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "Building $Configuration|$Platform..."
& $msbuild (Join-Path $root 'AnvilPlayer.sln') /p:Configuration=$Configuration /p:Platform=$Platform /m
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$outDir = Join-Path $root "x64\$Configuration"
$distDir = Join-Path $root 'dist\release'
$stagingRoot = Join-Path $distDir 'staging'
$appStage = Join-Path $stagingRoot 'AnvilPlayer'

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $appStage | Out-Null

Copy-Item -LiteralPath (Join-Path $outDir 'AnvilPlayer.App.exe') -Destination $appStage -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $appStage -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $appStage -Force

Copy-Item -LiteralPath (Join-Path $root 'src\AnvilPlayer.App\assets') -Destination $appStage -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'src\AnvilPlayer.App\webui\dist') -Destination (Join-Path $appStage 'webui') -Recurse -Force

Copy-Item -Path (Join-Path $root 'third_party\ffmpeg\bin\*.dll') -Destination $appStage -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\ffmpeg\bin\ffmpeg.exe') -Destination $appStage -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\ffmpeg\bin\ffprobe.exe') -Destination $appStage -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\ffmpeg\bin\ffplay.exe') -Destination $appStage -Force

Copy-Item -LiteralPath $webViewExtract -Destination (Join-Path $appStage 'WebView2Runtime') -Recurse -Force

$notice = @"
Anvil Player third-party notices

FFmpeg
- Runtime DLLs and helper executables are bundled from third_party/ffmpeg.
- License: see third_party/ffmpeg/LICENSE.txt in this repository.

Microsoft Edge WebView2
- Fixed Version Runtime $webViewVersion x64 is bundled in WebView2Runtime.
- Microsoft WebView2 SDK files are under third_party/webview2.
- License and notices: see third_party/webview2/LICENSE.txt and NOTICE.txt.
"@
Set-Content -LiteralPath (Join-Path $appStage 'THIRD-PARTY-NOTICES.txt') -Value $notice -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $root 'third_party\ffmpeg\LICENSE.txt') -Destination (Join-Path $appStage 'FFMPEG-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\webview2\LICENSE.txt') -Destination (Join-Path $appStage 'WEBVIEW2-SDK-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\webview2\NOTICE.txt') -Destination (Join-Path $appStage 'WEBVIEW2-SDK-NOTICE.txt') -Force

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Write-Host "Compiling Inno Setup installer..."
& $iscc (Join-Path $root 'packaging\anvil-player.iss')
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE"
}

$installer = Join-Path $distDir 'AnvilPlayer-0.1.0-x64-Setup.exe'
if (!(Test-Path -LiteralPath $installer)) {
    throw "Installer was not produced: $installer"
}

Write-Host "Installer ready: $installer"
