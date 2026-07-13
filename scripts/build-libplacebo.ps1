param(
    [string]$Msys2Root = $(if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }),
    [string]$Commit = 'a7a18af88ff0a17c04840dcb3246047bb6b46df3',
    [switch]$SkipPackageInstall
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$thirdPartyRoot = Join-Path $root 'third_party\libplacebo'
$buildRoot = Join-Path $thirdPartyRoot 'build'
$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
$ucrt64 = Join-Path $Msys2Root 'ucrt64'

function Convert-ToMsysPath([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($fullPath -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "MSYS2 path conversion only supports local drive paths: $fullPath"
    }
    $drive = $Matches[1].ToLowerInvariant()
    $tail = $Matches[2].Replace('\', '/')
    return "/$drive/$tail"
}

if (!(Test-Path -LiteralPath $bash)) {
    throw "MSYS2 UCRT64 was not found at $Msys2Root. Install MSYS2 or set MSYS2_ROOT."
}

$expectedParent = [System.IO.Path]::GetFullPath($thirdPartyRoot) +
    [System.IO.Path]::DirectorySeparatorChar
$resolvedBuildRoot = [System.IO.Path]::GetFullPath($buildRoot)
if (!$resolvedBuildRoot.StartsWith(
        $expectedParent, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean an unexpected build path: $resolvedBuildRoot"
}

$previousMsystem = $env:MSYSTEM
$previousChereInvoking = $env:CHERE_INVOKING
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
try {
    if (!$SkipPackageInstall) {
        Write-Host 'Installing the MSYS2 UCRT64 build dependencies...'
        $packages = @(
            'mingw-w64-ucrt-x86_64-gcc',
            'mingw-w64-ucrt-x86_64-meson',
            'mingw-w64-ucrt-x86_64-ninja',
            'mingw-w64-ucrt-x86_64-pkgconf',
            'mingw-w64-ucrt-x86_64-python-jinja',
            'mingw-w64-ucrt-x86_64-shaderc',
            'mingw-w64-ucrt-x86_64-spirv-cross',
            'mingw-w64-ucrt-x86_64-vulkan-headers'
        ) -join ' '
        & $bash -lc "pacman -S --needed --noconfirm $packages"
        if ($LASTEXITCODE -ne 0) {
            throw "MSYS2 dependency installation failed with exit code $LASTEXITCODE"
        }
    }

    if (Test-Path -LiteralPath $buildRoot) {
        Remove-Item -LiteralPath $buildRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

    $archive = Join-Path $buildRoot "libplacebo-$Commit.zip"
    $sourceParent = Join-Path $buildRoot 'source'
    Write-Host "Downloading libplacebo commit $Commit..."
    Invoke-WebRequest -Uri "https://github.com/haasn/libplacebo/archive/$Commit.zip" -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $sourceParent
    $source = Get-ChildItem -LiteralPath $sourceParent -Directory |
        Select-Object -ExpandProperty FullName -First 1
    if (!$source -or !(Test-Path -LiteralPath (Join-Path $source 'meson.build'))) {
        throw 'The downloaded libplacebo archive does not contain a Meson project.'
    }

    $pkgConfig = Join-Path $buildRoot 'pkgconfig'
    $mesonBuild = Join-Path $buildRoot 'meson'
    $install = Join-Path $buildRoot 'install'
    New-Item -ItemType Directory -Force -Path $pkgConfig | Out-Null

    $ucrt64Pc = $ucrt64.Replace('\', '/')
    $shadercPc = @"
Name: shaderc
Description: Statically linked shaderc for Anvil Player
Version: 2026
Libs: -Wl,--start-group $ucrt64Pc/lib/libshaderc_combined.a $ucrt64Pc/lib/libglslang.a $ucrt64Pc/lib/libSPIRV-Tools-opt.a $ucrt64Pc/lib/libSPIRV-Tools.a $ucrt64Pc/lib/libSPIRV-Tools-link.a -Wl,--end-group
Cflags: -I$ucrt64Pc/include
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $pkgConfig 'shaderc.pc'), $shadercPc,
        [System.Text.UTF8Encoding]::new($false))

    $spirvCrossPc = @"
Name: spirv-cross-c-shared
Description: Statically linked SPIRV-Cross for Anvil Player
Version: 1.4
Libs: -L$ucrt64Pc/lib -Wl,--start-group -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-core -Wl,--end-group
Cflags: -I$ucrt64Pc/include/spirv_cross
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $pkgConfig 'spirv-cross-c-shared.pc'), $spirvCrossPc,
        [System.Text.UTF8Encoding]::new($false))

    $sourceMsys = Convert-ToMsysPath $source
    $buildMsys = Convert-ToMsysPath $mesonBuild
    $installMsys = Convert-ToMsysPath $install
    $pkgConfigMsys = Convert-ToMsysPath $pkgConfig
    $buildCommands = @"
export PKG_CONFIG_PATH='${pkgConfigMsys}:/ucrt64/lib/pkgconfig'
export LDFLAGS='-static-libgcc -static-libstdc++ -static'
meson setup '$buildMsys' '$sourceMsys' --prefix='$installMsys' --buildtype=release -Dstrip=true -Dtests=false -Ddemos=false -Dbench=false -Dvulkan=disabled -Dopengl=disabled -Dd3d11=enabled -Ddovi=enabled -Dlibdovi=disabled -Dshaderc=enabled -Dglslang=disabled -Dlcms=disabled -Dxxhash=disabled -Dunwind=disabled
meson compile -C '$buildMsys'
meson install -C '$buildMsys'
"@
    Write-Host 'Building the D3D11 libplacebo backend and static shader toolchain...'
    & $bash -lc $buildCommands
    if ($LASTEXITCODE -ne 0) {
        throw "libplacebo build failed with exit code $LASTEXITCODE"
    }

    $dll = Join-Path $install 'bin\libplacebo-371.dll'
    $configHeader = Join-Path $install 'include\libplacebo\config.h'
    if (!(Test-Path -LiteralPath $dll) -or
        !(Select-String -LiteralPath $configHeader -Quiet -Pattern '#define PL_API_VER 371')) {
        throw 'The build did not produce the expected libplacebo API 371 artifacts.'
    }

    $objdump = Join-Path $ucrt64 'bin\objdump.exe'
    $imports = & $objdump -p $dll | Select-String 'DLL Name:' | ForEach-Object Line
    $unexpectedImports = $imports | Where-Object {
        $_ -match 'lib(gcc|stdc\+\+|winpthread|shaderc|spirv|glslang)'
    }
    if ($unexpectedImports) {
        throw "libplacebo retained conflicting MinGW dependencies:`n$($unexpectedImports -join "`n")"
    }

    $includeDestination = Join-Path $thirdPartyRoot 'include\libplacebo'
    if (Test-Path -LiteralPath $includeDestination) {
        Remove-Item -LiteralPath $includeDestination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $includeDestination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $install 'include\libplacebo') -Destination $includeDestination -Recurse -Force
    New-Item -ItemType Directory -Force -Path (Join-Path $thirdPartyRoot 'bin') | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $thirdPartyRoot 'bin\libplacebo-371.dll') -Force
    Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $thirdPartyRoot 'LICENSE.txt') -Force

    $hash = Get-FileHash (Join-Path $thirdPartyRoot 'bin\libplacebo-371.dll') -Algorithm SHA256
    Write-Host "libplacebo API 371 ready: $($hash.Path)"
    Write-Host "SHA256: $($hash.Hash)"
} finally {
    $env:MSYSTEM = $previousMsystem
    $env:CHERE_INVOKING = $previousChereInvoking
}
