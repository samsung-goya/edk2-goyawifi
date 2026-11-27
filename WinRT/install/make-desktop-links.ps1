<#
  make-desktop-links.ps1 -Out <folder>
  Writes the Windows RT desktop shortcuts (.lnk) for the tablet into <folder>. WinPE's desktop.flag step
  (WinRT\winpe\goyape\autorun.cmd) copies goyape\desktop\*.lnk to the Public Desktop of the Windows RT
  partition. Run it on any Windows PC: the targets are the same System32 paths there, and only the paths
  are stored in the links.

  On-Screen Keyboard is osk.exe: a UIAccess program, so it stays above the full-screen Windows 8 UI (the
  Charms network list, Start). goyatouch reports touches as an absolute mouse, so Windows does not open
  its touch keyboard by itself when a text box is tapped.
#>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$sys = 'C:\Windows\System32'
$links = @(
    @{ Name = 'Device Manager';      Target = "$sys\mmc.exe";      Args = "$sys\devmgmt.msc"; Icon = "$sys\devmgr.dll,4" },
    @{ Name = 'Task Manager';        Target = "$sys\Taskmgr.exe";  Args = '';                 Icon = "$sys\Taskmgr.exe,0" },
    @{ Name = 'On-Screen Keyboard';  Target = "$sys\osk.exe";      Args = '';                 Icon = "$sys\osk.exe,0" },
    @{ Name = 'Command Prompt';      Target = "$sys\cmd.exe";      Args = '';                 Icon = "$sys\cmd.exe,0" },
    @{ Name = 'Network Connections'; Target = "$sys\control.exe";  Args = 'ncpa.cpl';         Icon = "$sys\netshell.dll,0" },
    @{ Name = 'Control Panel';       Target = "$sys\control.exe";  Args = '';                 Icon = "$sys\control.exe,0" },
    @{ Name = 'Restart';             Target = "$sys\shutdown.exe"; Args = '/r /t 0';          Icon = "$sys\shell32.dll,238" },
    @{ Name = 'Power Off';           Target = "$sys\shutdown.exe"; Args = '/s /t 0';          Icon = "$sys\shell32.dll,27" }
)
$sh = New-Object -ComObject WScript.Shell
foreach ($l in $links) {
    $p = Join-Path (Resolve-Path $Out) ($l.Name + '.lnk')
    $s = $sh.CreateShortcut($p)
    $s.TargetPath = $l.Target
    $s.Arguments = $l.Args
    $s.WorkingDirectory = '%HOMEDRIVE%%HOMEPATH%'
    $s.IconLocation = $l.Icon
    $s.Save()
    Write-Host "  $($l.Name).lnk -> $($l.Target) $($l.Args)"
}
