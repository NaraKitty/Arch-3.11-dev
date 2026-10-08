<#
Runs one scripted session of real Windows for Workgroups 3.11 in DOSBox-X and records every
distinct, stable screen as a PNG (frames that stay unchanged for -Stable polls).

  ref-run.ps1 -Name notepad -Shell notepad.exe -Keys (Get-Content tests\notepad.keys)

-Keys are DOSBox-X AUTOTYPE buttons (letters, enter, esc, lalt, tab, ...; "," = extra pause).
The app under test is made the Windows shell, so it is alone on the desktop and Windows exits
when it closes. Each run starts from a fresh copy of the pristine install (setup.ps1).
#>
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Shell,
    [string[]]$Keys = @(),
    [double]$Wait = 15,
    [double]$Pace = 0.15,
    [int]$Timeout = 240,
    [int]$PollMs = 250,
    [int]$Stable = 4,
    [string]$RefDir = "$env:USERPROFILE\arch311-ref",
    [string]$DosboxX = 'C:\DOSBox-X\dosbox-x.exe',
    [string]$Cwd = '\WINDOWS',
    [string]$Run = '',         # WIN.INI [windows] run= (programs started after the shell)
    [string]$Scenario = ''     # a .scn file (scenario.py); replaces -Keys
)
$ErrorActionPreference = 'Stop'
if ($Scenario) { $Keys = @(python -I (Join-Path $PSScriptRoot 'scenario.py') autotype $Scenario) }
if (-not ('WinCap' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'WinCap.cs') }

$drive = Join-Path $RefDir 'run'
if (Test-Path $drive) { Remove-Item -Recurse -Force $drive }
Copy-Item -Recurse (Join-Path $RefDir 'c-pristine') $drive
$ini = Join-Path $drive 'WINDOWS\SYSTEM.INI'
$text = [IO.File]::ReadAllText($ini) -replace '(?m)^shell=.*$', "shell=$Shell"
[IO.File]::WriteAllText($ini, $text, [Text.Encoding]::ASCII)
if ($Run) {
    $wini = Join-Path $drive 'WINDOWS\WIN.INI'
    $text = [IO.File]::ReadAllText($wini) -replace '(?m)^run=.*$', "run=$Run"
    [IO.File]::WriteAllText($wini, $text, [Text.Encoding]::ASCII)
}

$conf = Join-Path $RefDir 'run.conf'
$auto = if ($Keys.Count) { "autotype -w $Wait -p $Pace " + ($Keys -join ' ') } else { 'rem no keys' }
@"
[autoexec]
mount c "$drive"
c:
cd $Cwd
$auto
win
exit
"@ | Set-Content -Encoding ascii $conf

$out = Join-Path $RefDir "shots\$Name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

$p = Start-Process $DosboxX -ArgumentList '-conf', (Join-Path $PSScriptRoot 'common.conf'), '-conf', $conf -WorkingDirectory $RefDir -PassThru
$t0 = Get-Date
# "unchanged" tolerates up to $Tolerance pixels so the blinking caret does not count as a change
$Tolerance = 80
$prev = $null; $saved = $null; $same = 0; $n = 0
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
    Start-Sleep -Milliseconds $PollMs
    $p.Refresh()
    if ($p.MainWindowHandle -eq 0) { continue }
    $w = 0; $h = 0
    $rgb = [WinCap]::Grab($p.MainWindowHandle, [ref]$w, [ref]$h)
    if (-not $rgb -or $w -ne 640) { continue }   # only the 640x480 Windows screen
    if ([WinCap]::DiffCount($prev, $rgb) -le $Tolerance) { $same++ } else { $same = 0 }
    $prev = $rgb
    if ($same -eq $Stable -and [WinCap]::DiffCount($saved, $rgb) -gt $Tolerance) {
        $n++
        $f = Join-Path $out ('{0:D2}.png' -f $n)
        [WinCap]::SavePng($f, $rgb, $w, $h)
        $saved = $rgb
        '{0,6:F1}s  {1}' -f ((Get-Date) - $t0).TotalSeconds, (Split-Path -Leaf $f)
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; 'timeout: killed DOSBox-X' }
"$n frames in $out"
