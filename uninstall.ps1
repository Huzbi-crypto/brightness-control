<#
    brightness-control uninstaller.

    Stops the agent, removes the logon launcher and deletes the install folder.
    The DLL stays loaded in the Windows shell until that process restarts, so the
    chevron can linger until you sign out - which this says plainly.
#>

$ErrorActionPreference = 'Continue'

$InstallDir  = Join-Path $env:LOCALAPPDATA 'BrightnessControl'
$StartupDir  = [Environment]::GetFolderPath('Startup')
$StartupFile = Join-Path $StartupDir 'brightness-control.vbs'

function Say($text, $colour = 'Gray') { Write-Host $text -ForegroundColor $colour }
function Ok($text)   { Write-Host "  [ok]   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "  [note] $text" -ForegroundColor Yellow }

Say ''
Say '  brightness-control - uninstall' 'White'
Say ''

$stopped = @()
foreach ($name in @('bcsync', 'BrightnessControl')) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($procs) {
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        $stopped += $name
        Start-Sleep -Milliseconds 400
    }
}
if ($stopped.Count -gt 0) { Ok "stopped $($stopped -join ', ')" } else { Ok 'nothing was running' }

if (Test-Path $StartupFile) {
    Remove-Item $StartupFile -Force -ErrorAction SilentlyContinue
    Ok 'removed the logon launcher'
} else {
    Ok 'no logon launcher to remove'
}

if (Test-Path $InstallDir) {
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $InstallDir) {
        # bcdll.dll is still mapped into the running shell, so it cannot be
        # deleted. Moving the folder aside is enough to free the path for a
        # future install; the file itself goes when the shell restarts.
        $aside = "$InstallDir.old"
        Remove-Item $aside -Recurse -Force -ErrorAction SilentlyContinue
        $moved = $false
        try {
            Rename-Item $InstallDir $aside -ErrorAction Stop
            $moved = $true
        } catch {
            $moved = $false
        }
        if ($moved) {
            Remove-Item $aside -Recurse -Force -ErrorAction SilentlyContinue
            Ok 'removed the install folder (one file is still loaded by the shell)'
            if (Test-Path $aside) {
                Say "  $aside is left behind until you sign out, then it can be deleted." 'DarkGray'
            }
        } else {
            Warn "could not remove $InstallDir"
            Say '  Sign out and delete it by hand.' 'DarkGray'
        }
    } else {
        Ok "deleted $InstallDir"
    }
} else {
    Ok 'install folder already gone'
}

Say ''
Warn 'The Quick Settings chevron stays until the Windows shell restarts.'
Say '  Sign out and back in (or reboot) to clear it completely.' 'DarkGray'
Say ''
exit 0
