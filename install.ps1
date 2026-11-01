<#
    brightness-control installer.

    Copies the files next to this script into %LOCALAPPDATA%\BrightnessControl,
    registers a logon launcher, starts the resident agent, and waits until it has
    actually attached to the Windows shell so it can tell you whether it worked.

    No administrator rights are needed: nothing here writes outside the current
    user's profile.
#>

$ErrorActionPreference = 'Stop'

$AppName     = 'brightness-control'
$InstallDir  = Join-Path $env:LOCALAPPDATA 'BrightnessControl'
$AgentExe    = Join-Path $InstallDir 'bcsync.exe'
$DllName     = 'bcdll.dll'
$StartupDir  = [Environment]::GetFolderPath('Startup')
$StartupFile = Join-Path $StartupDir 'brightness-control.vbs'

function Say($text, $colour = 'Gray') { Write-Host $text -ForegroundColor $colour }
function Ok($text)   { Write-Host "  [ok]   $text" -ForegroundColor Green }
function Warn($text) { Write-Host "  [note] $text" -ForegroundColor Yellow }
function Bad($text)  { Write-Host "  [fail] $text" -ForegroundColor Red }

Say ''
Say '  brightness-control' 'White'
Say '  Per-display brightness inside Windows Quick Settings' 'DarkGray'
Say ''

# --- 1. the files this script needs, sitting beside it -----------------------
$sourceDir = $PSScriptRoot
if (-not $sourceDir) { $sourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path }

$needed = @('bcdll.dll', 'bcsync.exe')
$missing = @()
foreach ($f in $needed) {
    if (-not (Test-Path (Join-Path $sourceDir $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Bad "these files are missing next to the installer: $($missing -join ', ')"
    Say ''
    Say '  Extract the whole release zip first, then run install.cmd from it.' 'DarkGray'
    Say ''
    exit 1
}
Ok 'found the program files'

# --- 2. stop anything already running ---------------------------------------
$stopped = @()
foreach ($name in @('bcsync', 'BrightnessControl')) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($procs) {
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        $stopped += $name
        Start-Sleep -Milliseconds 400
    }
}
if ($stopped.Count -gt 0) { Ok "stopped the previous version ($($stopped -join ', '))" }

# --- 3. copy the files into place -------------------------------------------
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

# A leftover from a previous uninstall can sit here because the shell still has
# the old DLL mapped. Clear it before we start.
$aside = "$InstallDir.old"
if (Test-Path $aside) { Remove-Item $aside -Recurse -Force -ErrorAction SilentlyContinue }

$stillLoaded = $false
foreach ($f in @('bcdll.dll', 'bcsync.exe', 'bcinject.exe')) {
    $src = Join-Path $sourceDir $f
    if (-not (Test-Path $src)) { continue }

    $dest = Join-Path $InstallDir $f
    $copied = $false
    try {
        Copy-Item $src $dest -Force -ErrorAction Stop
        $copied = $true
    } catch {
        # The shell has this DLL mapped, so it cannot be overwritten. Renaming it
        # out of the way works even while it is loaded.
        if (Test-Path $dest) {
            $stale = "$dest.old"
            Remove-Item $stale -Force -ErrorAction SilentlyContinue
            try {
                Rename-Item $dest $stale -ErrorAction Stop
                Copy-Item $src $dest -Force -ErrorAction Stop
                $copied = $true
                if ($f -eq 'bcdll.dll') { $stillLoaded = $true }
            } catch {
                $copied = $false
            }
        }
    }

    if (-not $copied) {
        Bad "could not copy $f - it is in use"
        Say '  Sign out and back in, then run install.cmd again.' 'DarkGray'
        Say ''
        exit 1
    }
}
# an old C# install may have left these behind
foreach ($old in @('BrightnessControl.exe', 'config.ini')) {
    $p = Join-Path $InstallDir $old
    if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
}
Ok "installed to $InstallDir"

# --- 4. start it with Windows ------------------------------------------------
$vbs = @"
' Starts the brightness-control agent with no window.
Set sh = CreateObject("WScript.Shell")
sh.Run """$AgentExe""", 0, False
"@
Set-Content -Path $StartupFile -Value $vbs -Encoding ASCII
Ok 'registered to start with Windows'

# --- 5. start it now ---------------------------------------------------------
Start-Process -FilePath $AgentExe -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$agent = Get-Process -Name 'bcsync' -ErrorAction SilentlyContinue
if (-not $agent) {
    Bad 'the agent did not start'
    Say ''
    Say '  Try running it by hand to see the error:' 'DarkGray'
    Say "    $AgentExe" 'DarkGray'
    Say ''
    exit 1
}
Ok "agent running (pid $($agent.Id))"

# --- 6. prove it actually attached to the shell ------------------------------
Say ''
Say '  Waiting for the Windows shell...' 'DarkGray'

$attached = $false
$sawShell = $false
for ($i = 0; $i -lt 20; $i++) {
    $hosts = Get-Process -Name 'ShellHost' -ErrorAction SilentlyContinue
    if ($hosts) {
        $sawShell = $true
        foreach ($h in $hosts) {
            try {
                $modules = $h.Modules | Where-Object { $_.ModuleName -eq $DllName }
                if ($modules) { $attached = $true; break }
            } catch {
                # a protected process can refuse to list modules; ignore it
            }
        }
    }
    if ($attached) { break }
    Start-Sleep -Milliseconds 500
}

Say ''
if ($attached) {
    Ok 'attached to the Windows shell'
    Say ''
    Say '  Press Win+A to open Quick Settings.' 'White'
    Say '  The chevron on the brightness row opens the per-display sliders.' 'DarkGray'
} elseif ($sawShell) {
    Warn 'the shell is running but the DLL has not attached yet'
    Say '  It retries on its own; open Quick Settings with Win+A to nudge it.' 'DarkGray'
} else {
    Warn 'no ShellHost process yet'
    Say '  The agent attaches as soon as the shell starts. Sign out and back in,' 'DarkGray'
    Say '  or just open Quick Settings with Win+A.' 'DarkGray'
}

if ($stillLoaded) {
    Say ''
    Warn 'the previous version is still loaded in the running shell'
    Say '  Sign out and back in to switch to this build. Until then the old one' 'DarkGray'
    Say '  keeps running, even though the new files are installed.' 'DarkGray'
}

Say ''
Say '  To remove it later, run uninstall.cmd from the same folder.' 'DarkGray'
Say ''
exit 0
