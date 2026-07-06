param(
    [Parameter(Mandatory = $true)]
    [string]$Payload
)

$ErrorActionPreference = 'Stop'

$appName = 'Anvil Player'
$publisher = 'Anvil'
$version = '0.1.0'
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\Anvil Player'
$startMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Anvil Player'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Anvil Player.lnk'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Anvil Player'
$tempDir = Join-Path $env:TEMP ('anvil-player-install-' + [guid]::NewGuid().ToString('N'))

function New-Shortcut {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Target,
        [string]$WorkingDirectory = '',
        [string]$IconLocation = ''
    )

    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    if ($WorkingDirectory) {
        $shortcut.WorkingDirectory = $WorkingDirectory
    }
    if ($IconLocation) {
        $shortcut.IconLocation = $IconLocation
    }
    $shortcut.Save()
}

try {
    if (!(Test-Path -LiteralPath $Payload)) {
        throw "Payload not found: $Payload"
    }

    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    Expand-Archive -LiteralPath $Payload -DestinationPath $tempDir -Force

    $payloadRoot = Join-Path $tempDir 'AnvilPlayer'
    if (!(Test-Path -LiteralPath (Join-Path $payloadRoot 'AnvilPlayer.App.exe'))) {
        throw 'Invalid installer payload.'
    }

    if (Test-Path -LiteralPath $installDir) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    Get-ChildItem -LiteralPath $payloadRoot -Force | Copy-Item -Destination $installDir -Recurse -Force

    $exePath = Join-Path $installDir 'AnvilPlayer.App.exe'
    $iconPath = Join-Path $installDir 'assets\app.ico'

    $uninstallScript = @"
`$ErrorActionPreference = 'Stop'
`$installDir = '$($installDir.Replace("'", "''"))'
`$startMenuDir = '$($startMenuDir.Replace("'", "''"))'
`$desktopShortcut = '$($desktopShortcut.Replace("'", "''"))'
`$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Anvil Player'
if (Test-Path -LiteralPath `$desktopShortcut) { Remove-Item -LiteralPath `$desktopShortcut -Force }
if (Test-Path -LiteralPath `$startMenuDir) { Remove-Item -LiteralPath `$startMenuDir -Recurse -Force }
if (Test-Path -LiteralPath `$uninstallKey) { Remove-Item -LiteralPath `$uninstallKey -Recurse -Force }
Start-Sleep -Milliseconds 300
if (Test-Path -LiteralPath `$installDir) { Remove-Item -LiteralPath `$installDir -Recurse -Force }
"@
    Set-Content -LiteralPath (Join-Path $installDir 'uninstall.ps1') -Value $uninstallScript -Encoding UTF8

    New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null
    New-Shortcut -Path (Join-Path $startMenuDir 'Anvil Player.lnk') -Target $exePath -WorkingDirectory $installDir -IconLocation $iconPath
    New-Shortcut -Path $desktopShortcut -Target $exePath -WorkingDirectory $installDir -IconLocation $iconPath

    $uninstallCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$installDir\uninstall.ps1`""
    New-Shortcut -Path (Join-Path $startMenuDir 'Uninstall Anvil Player.lnk') `
        -Target "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
        -WorkingDirectory $installDir `
        -IconLocation "$env:SystemRoot\System32\shell32.dll,31"

    $uninstallShortcut = Join-Path $startMenuDir 'Uninstall Anvil Player.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($uninstallShortcut)
    $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$installDir\uninstall.ps1`""
    $shortcut.Save()

    New-Item -Path $uninstallKey -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayName -Value $appName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value $version -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name Publisher -Value $publisher -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $installDir -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayIcon -Value $exePath -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name UninstallString -Value $uninstallCmd -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null

    Write-Host "$appName installed to $installDir"
    Start-Process -FilePath $exePath -WorkingDirectory $installDir
} finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
