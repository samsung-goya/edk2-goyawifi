@echo off
rem autorun.cmd - runs at every WinPE boot from the SD card. Edit it on the PC.
rem Writes a diagnostics file next to it so the card can be sent back for analysis.
set OUT=%~dp0diag.txt
echo. >> "%OUT%"
echo === %DATE% %TIME% >> "%OUT%"
echo --- cpu / os >> "%OUT%"
echo %NUMBER_OF_PROCESSORS% processors, %PROCESSOR_ARCHITECTURE% >> "%OUT%"
ver >> "%OUT%"
echo --- drivers >> "%OUT%"
driverquery /v /fo list >> "%OUT%" 2>&1
echo --- devices ^(problem codes^) >> "%OUT%"
wmic path Win32_PnPEntity get Name,ConfigManagerErrorCode,DeviceID /format:list >> "%OUT%" 2>&1
echo --- timers ^(HAL extension^) >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase" /s >> "%OUT%" 2>&1
echo --- boot log >> "%OUT%"
if exist X:\Windows\ntbtlog.txt type X:\Windows\ntbtlog.txt >> "%OUT%"
rem ---- your own commands below ----

rem ---- verify.flag (left by the speed test): compare its copy again after a restart, so the NTFS copy
rem is read from the card and not from the file cache ----
if not exist "%~dp0verify.flag" goto noverify
move /y "%~dp0verify.flag" "%~dp0verify.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- compare after restart (cold cache) >> "%OUT%"
call :compare cold
:noverify

rem ---- goyasdhc speed test (bench.flag next to this file turns it on) -------------------------------
rem Reads \sources\boot.wim (the WinPE image, ~190 MB) from this FAT32 partition, then copies it to the
rem NTFS partition (write) and reads that copy back. Start/end times go to diag.txt.
if not exist "%~dp0bench.flag" goto nobench
rem one run per flag: a restart that comes back to WinPE does not repeat the test
move /y "%~dp0bench.flag" "%~dp0bench.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- goyasdhc speed test >> "%OUT%"
for %%F in ("%~d0\sources\boot.wim") do echo file %%~fF  %%~zF bytes >> "%OUT%"
echo read  FAT32 start %TIME% >> "%OUT%"
copy /b "%~d0\sources\boot.wim" nul >nul
echo read  FAT32 end   %TIME% >> "%OUT%"
if not defined WIN (echo no Windows partition found, write test skipped >> "%OUT%" & goto benchdone)
if not exist %WIN%\goya mkdir %WIN%\goya
echo write NTFS %WIN% start %TIME% >> "%OUT%"
rem a fresh file each time: stale bytes of an older copy must not pass for a correct write
if exist %WIN%\goya\bench.tmp del %WIN%\goya\bench.tmp
copy /b /y "%~d0\sources\boot.wim" %WIN%\goya\bench.tmp >nul
echo write NTFS %WIN% end   %TIME% >> "%OUT%"
echo read  NTFS %WIN% start %TIME% >> "%OUT%"
copy /b %WIN%\goya\bench.tmp nul >nul
echo read  NTFS %WIN% end   %TIME% >> "%OUT%"
call :compare warm
echo compare again after the restart > "%~dp0verify.flag"
rem bench.tmp stays on the NTFS partition for the compare after the restart
:benchdone
echo --- end %TIME% >> "%OUT%"
:nobench

rem ---- install.flag: put every driver package in .\install\ (NAME.sys, with NAME.inf/.cat if
rem present) into the Windows RT install on the NTFS partition: System32\drivers\NAME.sys, which the
rem boot loads (the first replaced copy is kept as NAME.sys.old), and the copy staged for Windows
rem setup in \goya\drivers so setup cannot bring the old one back. Each copy is byte-compared. ----
if not exist "%~dp0install.flag" goto noinstall
move /y "%~dp0install.flag" "%~dp0install.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- install drivers from .\install into %WIN% >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto noinstall)
set DRV=%WIN%\Windows\System32\drivers
for %%f in ("%~dp0install\*.sys") do call :instdrv %%~nf
:noinstall

rem ---- halext.flag: replace the timer HAL extension, System32\HalExtMmpTimer.dll on the Windows RT
rem partition, with .\install\HalExtMmpTimer.dll (the first replaced copy is kept as .dll.old) and
rem byte-compare it. ----
if not exist "%~dp0halext.flag" goto nohalext
move /y "%~dp0halext.flag" "%~dp0halext.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- HAL extension into %WIN% >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto nohalext)
if not exist "%~dp0install\HalExtMmpTimer.dll" (echo no install\HalExtMmpTimer.dll >> "%OUT%" & goto nohalext)
set HX=%WIN%\Windows\System32\HalExtMmpTimer.dll
if exist %HX% if not exist %HX%.old copy /b /y %HX% %HX%.old >nul
copy /b /y "%~dp0install\HalExtMmpTimer.dll" %HX% >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~dp0install\HalExtMmpTimer.dll" %HX% >> "%OUT%" 2>&1
echo System32\HalExtMmpTimer.dll compare exit code %ERRORLEVEL% >> "%OUT%"
:nohalext

rem ---- adddrv.flag: add every driver package in .\adddrv\ (NAME.inf + .sys + .cat) to the Windows RT
rem install's driver store with DISM, for a device that has no driver yet (install.flag only replaces
rem the files of drivers that are installed already). Windows installs it when it finds the device. ----
if not exist "%~dp0adddrv.flag" goto noadddrv
move /y "%~dp0adddrv.flag" "%~dp0adddrv.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- add driver packages from .\adddrv to %WIN% >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto noadddrv)
for %%f in ("%~dp0adddrv\*.inf") do call :adddrv "%%f"
:noadddrv

rem ---- redrv.flag: one hardware ID per line (e.g. ACPI\MRVL0009). Every device instance of it in the
rem offline install loses its Service value and gets ConfigFlags 0x20 (CONFIGFLAG_REINSTALL), so Windows
rem picks the best driver from its driver store again at the next boot (after packages were added with
rem DISM on a PC, add-drivers-sd.cmd). Undoes a :regdrv binding. The Enum keys are only writable as
rem SYSTEM, which WinPE is. The SYSTEM hive is copied to .\redrv-SYSTEM.before first. ----
if not exist "%~dp0redrv.flag" goto noredrv
move /y "%~dp0redrv.flag" "%~dp0redrv.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- redrv: reinstall drivers for the IDs in redrv.done on %WIN% >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto noredrv)
copy /b /y %WIN%\Windows\System32\config\SYSTEM "%~dp0redrv-SYSTEM.before" >> "%OUT%" 2>&1
if errorlevel 1 (echo hive backup failed, registry not changed >> "%OUT%" & goto noredrv)
reg load HKLM\GOYAREG %WIN%\Windows\System32\config\SYSTEM >> "%OUT%" 2>&1
if errorlevel 1 (echo hive load failed >> "%OUT%" & goto noredrv)
set CS=1
for /f "tokens=3" %%v in ('reg query HKLM\GOYAREG\Select /v Current ^| find "Current"') do set /a CS=%%v
set CS=000%CS%
set RK=HKLM\GOYAREG\ControlSet%CS:~-3%
for /f "usebackq eol=; delims=" %%h in ("%~dp0redrv.done") do call :redrvid "%%h"
reg unload HKLM\GOYAREG >> "%OUT%" 2>&1
:noredrv

rem ---- pnpinst.flag: install driver packages from inside Windows RT, where pnputil works (DISM does not
rem in this WinPE, and a PC may not load this image's hives either). .\pnpinst\ (goya-pnpinst.cmd,
rem pnpinst.list, the packages and GoyaTestCert.cer) is copied to C:\goya\pnpinst and an HKLM RunOnce
rem entry runs goya-pnpinst.cmd at the next logon; it leaves goyape\pnpinst-result.log on this card. ----
if not exist "%~dp0pnpinst.flag" goto nopnpinst
move /y "%~dp0pnpinst.flag" "%~dp0pnpinst.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SOFTWARE if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- pnpinst: stage .\pnpinst on %WIN% and set RunOnce >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto nopnpinst)
if not exist %WIN%\goya\pnpinst mkdir %WIN%\goya\pnpinst
xcopy /y /q "%~dp0pnpinst\*" %WIN%\goya\pnpinst\ >> "%OUT%" 2>&1
reg load HKLM\GOYASW %WIN%\Windows\System32\config\SOFTWARE >> "%OUT%" 2>&1
if errorlevel 1 (echo SOFTWARE hive load failed >> "%OUT%" & goto nopnpinst)
reg add "HKLM\GOYASW\Microsoft\Windows\CurrentVersion\RunOnce" /v GoyaPnpInst /t REG_SZ /d "cmd.exe /c C:\goya\pnpinst\goya-pnpinst.cmd" /f >> "%OUT%" 2>&1
reg query "HKLM\GOYASW\Microsoft\Windows\CurrentVersion\RunOnce" >> "%OUT%" 2>&1
reg unload HKLM\GOYASW >> "%OUT%" 2>&1
:nopnpinst

rem ---- wifireg.flag: one "name value" pair per line (DWORD, decimal or 0x hex), set in the installed
rem Marvell 88W8777 Wi-Fi driver's key (Net class instance whose MatchingDeviceId has PID_9131) in the
rem offline SYSTEM hive: the driver reads its settings from there, and re-running pnputil with an unchanged
rem package would not rewrite them. The hive is copied to .\wifireg-SYSTEM.before first. ----
if not exist "%~dp0wifireg.flag" goto nowifireg
move /y "%~dp0wifireg.flag" "%~dp0wifireg.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- wifireg: Wi-Fi driver settings from wifireg.done on %WIN% >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto nowifireg)
copy /b /y %WIN%\Windows\System32\config\SYSTEM "%~dp0wifireg-SYSTEM.before" >> "%OUT%" 2>&1
if errorlevel 1 (echo hive backup failed, registry not changed >> "%OUT%" & goto nowifireg)
reg load HKLM\GOYAREG %WIN%\Windows\System32\config\SYSTEM >> "%OUT%" 2>&1
if errorlevel 1 (echo hive load failed >> "%OUT%" & goto nowifireg)
set CS=1
for /f "tokens=3" %%v in ('reg query HKLM\GOYAREG\Select /v Current ^| find "Current"') do set /a CS=%%v
set CS=000%CS%
set RK=HKLM\GOYAREG\ControlSet%CS:~-3%
set WRN=0
for /f "delims=" %%k in ('reg query "%RK%\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" /s /f "PID_9131" /d ^| find "HKEY_"') do call :wifiregkey "%%k"
if "%WRN%"=="0" echo no Net class key with PID_9131 found >> "%OUT%"
reg unload HKLM\GOYAREG >> "%OUT%" 2>&1
:nowifireg

rem ---- desktop.flag: copy .\desktop\*.lnk (made by WinRT\install\make-desktop-links.ps1) to the Public
rem Desktop of the Windows RT partition, so every user gets them. ----
if not exist "%~dp0desktop.flag" goto nodesktop
move /y "%~dp0desktop.flag" "%~dp0desktop.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- desktop: shortcuts to %WIN%\Users\Public\Desktop >> "%OUT%"
if not defined WIN (echo no Windows partition found >> "%OUT%" & goto nodesktop)
if not exist "%WIN%\Users\Public\Desktop" mkdir "%WIN%\Users\Public\Desktop"
xcopy /y "%~dp0desktop\*.lnk" "%WIN%\Users\Public\Desktop\" >> "%OUT%" 2>&1
dir /b "%WIN%\Users\Public\Desktop" >> "%OUT%" 2>&1
:nodesktop

rem ---- setuplogs.flag: copy Windows Setup's logs from the Windows RT partition to .\setuplogs\ ----
if not exist "%~dp0setuplogs.flag" goto nosetuplogs
move /y "%~dp0setuplogs.flag" "%~dp0setuplogs.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- Windows Setup logs from %WIN% >> "%OUT%"
if not defined WIN goto nosetuplogs
if not exist "%~dp0setuplogs" mkdir "%~dp0setuplogs"
xcopy /y /q /s "%WIN%\Windows\Panther\*.log" "%~dp0setuplogs\panther\" >> "%OUT%" 2>&1
xcopy /y /q /s "%WIN%\Windows\Panther\*.xml" "%~dp0setuplogs\panther\" >> "%OUT%" 2>&1
xcopy /y /q "%WIN%\Windows\INF\setupapi*.log" "%~dp0setuplogs\inf\" >> "%OUT%" 2>&1
xcopy /y /q /s "%WIN%\Windows\System32\Sysprep\Panther\*.log" "%~dp0setuplogs\sysprep\" >> "%OUT%" 2>&1
xcopy /y /q /s "%WIN%\goya\*.log" "%~dp0setuplogs\goya\" >> "%OUT%" 2>&1
xcopy /y /q /s "%WIN%\goya\*.txt" "%~dp0setuplogs\goya\" >> "%OUT%" 2>&1
dir /s /b "%~dp0setuplogs" >> "%OUT%" 2>&1
:nosetuplogs

rem ---- probe.flag: read-only facts for a failed Windows Setup (system BCD store, partition types,
rem Setup state in the Windows RT SYSTEM hive, the specialize action list) ----
if not exist "%~dp0probe.flag" goto noprobe
move /y "%~dp0probe.flag" "%~dp0probe.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
if not exist "%~dp0setuplogs" mkdir "%~dp0setuplogs"
set P=%~dp0setuplogs\probe.txt
echo === probe %DATE% %TIME%, card %~d0, Windows %WIN% > "%P%"
echo --- bcdedit system store >> "%P%"
bcdedit /enum all >> "%P%" 2>&1
echo bcdedit exit code %ERRORLEVEL% >> "%P%"
echo --- bcdedit sysstore >> "%P%"
bcdedit /sysstore >> "%P%" 2>&1
echo --- diskpart >> "%P%"
> "%TEMP%\dp.txt" echo list disk
>> "%TEMP%\dp.txt" echo select disk 0
>> "%TEMP%\dp.txt" echo list partition
>> "%TEMP%\dp.txt" echo select partition 1
>> "%TEMP%\dp.txt" echo detail partition
>> "%TEMP%\dp.txt" echo select partition 2
>> "%TEMP%\dp.txt" echo detail partition
>> "%TEMP%\dp.txt" echo list volume
diskpart /s "%TEMP%\dp.txt" >> "%P%" 2>&1
echo --- firmware boot info >> "%P%"
reg query HKLM\SYSTEM\CurrentControlSet\Control /v FirmwareBootDevice >> "%P%" 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Control /v SystemBootDevice >> "%P%" 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Control /v PEFirmwareType >> "%P%" 2>&1
if not defined WIN goto noprobe
echo --- Windows RT Setup state >> "%P%"
reg load HKLM\GOYAPROBE %WIN%\Windows\System32\config\SYSTEM >> "%P%" 2>&1
reg query HKLM\GOYAPROBE\Setup /s >> "%P%" 2>&1
reg query HKLM\GOYAPROBE\Select >> "%P%" 2>&1
reg unload HKLM\GOYAPROBE >> "%P%" 2>&1
copy /y "%WIN%\Windows\System32\Sysprep\ActionFiles\Specialize.xml" "%~dp0setuplogs\Specialize.xml" >> "%P%" 2>&1
dir "%WIN%\Windows\System32\Sysprep\ActionFiles" >> "%P%" 2>&1
:noprobe

rem ---- fixspec.flag: Windows Setup's specialize pass cannot open the system BCD store on this card
rem (Sysprep_Specialize_Bcd, 0xC0000098: Windows does not take the FAT32 type 0x0C partition for the
rem system partition), and Setup stops with "could not configure Windows to run on this computer's
rem hardware". .\Specialize.xml is the image's action list without that one module (the boot entries
rem were written by the installer). The original is kept in .\setuplogs\Specialize.xml. ----
if not exist "%~dp0fixspec.flag" goto nofixspec
move /y "%~dp0fixspec.flag" "%~dp0fixspec.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- Specialize.xml without Sysprep_Specialize_Bcd into %WIN% >> "%OUT%"
if not defined WIN goto nofixspec
set AF=%WIN%\Windows\System32\Sysprep\ActionFiles
takeown /f "%AF%\Specialize.xml" >> "%OUT%" 2>&1
icacls "%AF%\Specialize.xml" /grant *S-1-5-32-544:F >> "%OUT%" 2>&1
copy /b /y "%~dp0Specialize.xml" "%AF%\Specialize.xml" >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~dp0Specialize.xml" "%AF%\Specialize.xml" >> "%OUT%" 2>&1
echo Specialize.xml compare exit code %ERRORLEVEL% >> "%OUT%"
:nofixspec

rem ---- fixbcd.flag: Windows takes a BCD store as the system store only if Description\System and
rem Description\TreatAsSystem are non-zero DWORDs (bcdedit's system store check; without them every
rem open fails with STATUS_FILE_INVALID, "the volume for a file has been externally altered", and
rem Setup stops with "could not update the computer's boot configuration"). bootmgr does not check
rem them. A store made with bcdedit /createstore has neither. The store is copied to
rem .\setuplogs\BCD.bak first. ----
if not exist "%~dp0fixbcd.flag" goto nofixbcd
move /y "%~dp0fixbcd.flag" "%~dp0fixbcd.done" >nul
set B=%~d0\EFI\Microsoft\Boot\BCD
echo --- system store flags in %B% >> "%OUT%"
if not exist "%~dp0setuplogs" mkdir "%~dp0setuplogs"
copy /b /y "%B%" "%~dp0setuplogs\BCD.bak" >> "%OUT%" 2>&1
if errorlevel 1 (echo BCD backup failed, nothing changed >> "%OUT%" & goto nofixbcd)
reg load HKLM\GOYABCD "%B%" >> "%OUT%" 2>&1
reg add HKLM\GOYABCD\Description /v System /t REG_DWORD /d 1 /f >> "%OUT%" 2>&1
reg add HKLM\GOYABCD\Description /v TreatAsSystem /t REG_DWORD /d 1 /f >> "%OUT%" 2>&1
reg query HKLM\GOYABCD\Description >> "%OUT%" 2>&1
reg unload HKLM\GOYABCD >> "%OUT%" 2>&1
bcdedit /store "%B%" /enum {bootmgr} >> "%OUT%" 2>&1
echo bcdedit /store exit code %ERRORLEVEL% >> "%OUT%"
bcdedit /enum {bootmgr} >> "%OUT%" 2>&1
echo bcdedit system store exit code %ERRORLEVEL% >> "%OUT%"
:nofixbcd

rem ---- specdiag.flag: Setup's own BCD update fails (ModifyBootEntries 0x3EE). .\specdiag\unattend.xml
rem is the installed unattend plus a specialize RunSynchronous of C:\goya\specdiag.cmd, which writes
rem what Windows sees of the system partition and BCD to C:\goya\specdiag.txt. The installed
rem unattend.xml is kept in .\setuplogs\unattend.orig.xml. ----
if not exist "%~dp0specdiag.flag" goto nospecdiag
move /y "%~dp0specdiag.flag" "%~dp0specdiag.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- specialize diagnostics into %WIN% >> "%OUT%"
if not defined WIN goto nospecdiag
if not exist "%~dp0setuplogs" mkdir "%~dp0setuplogs"
if not exist "%~dp0setuplogs\unattend.orig.xml" copy /b /y %WIN%\Windows\Panther\unattend.xml "%~dp0setuplogs\unattend.orig.xml" >> "%OUT%" 2>&1
if not exist %WIN%\goya mkdir %WIN%\goya
copy /b /y "%~dp0specdiag\specdiag.cmd" %WIN%\goya\specdiag.cmd >> "%OUT%" 2>&1
copy /b /y "%~dp0specdiag\unattend.xml" %WIN%\Windows\Panther\unattend.xml >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~dp0specdiag\unattend.xml" %WIN%\Windows\Panther\unattend.xml >> "%OUT%" 2>&1
echo unattend compare exit code %ERRORLEVEL% >> "%OUT%"
del %WIN%\goya\specdiag.txt 2>nul
:nospecdiag

rem ---- resetsetup.flag: after a failed specialize, Setup only shows "the computer restarted
rem unexpectedly" (ChildCompletion\setup.exe stays 1, SetupShutdownRequired 1) and never retries.
rem Back to "not started" so the next boot runs specialize again. The SYSTEM hive is copied to
rem .\setuplogs\SYSTEM.bak first. ----
if not exist "%~dp0resetsetup.flag" goto noresetsetup
move /y "%~dp0resetsetup.flag" "%~dp0resetsetup.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- reset Windows Setup state in %WIN% >> "%OUT%"
if not defined WIN goto noresetsetup
if not exist "%~dp0setuplogs" mkdir "%~dp0setuplogs"
copy /b /y %WIN%\Windows\System32\config\SYSTEM "%~dp0setuplogs\SYSTEM.bak" >> "%OUT%" 2>&1
if errorlevel 1 (echo hive backup failed, nothing changed >> "%OUT%" & goto noresetsetup)
reg load HKLM\GOYARESET %WIN%\Windows\System32\config\SYSTEM >> "%OUT%" 2>&1
reg add HKLM\GOYARESET\Setup\Status\ChildCompletion /v setup.exe /t REG_DWORD /d 0 /f >> "%OUT%" 2>&1
reg delete HKLM\GOYARESET\Setup /v SetupShutdownRequired /f >> "%OUT%" 2>nul
rem windeploy refuses with "failed generalization state [6]" after a failed specialize; 7 = generalized
reg add HKLM\GOYARESET\Setup\Status\SysprepStatus /v GeneralizationState /t REG_DWORD /d 7 /f >> "%OUT%" 2>&1
rem run the specialize unattend pass (RunSynchronous) again
reg add HKLM\GOYARESET\Setup\Status\UnattendPasses /v specialize /t REG_DWORD /d 0 /f >> "%OUT%" 2>&1
reg query HKLM\GOYARESET\Setup\Status\UnattendPasses >> "%OUT%" 2>&1
reg query HKLM\GOYARESET\Setup\Status\SysprepStatus >> "%OUT%" 2>&1
reg query HKLM\GOYARESET\Setup >> "%OUT%" 2>&1
reg query HKLM\GOYARESET\Setup\Status\ChildCompletion >> "%OUT%" 2>&1
reg unload HKLM\GOYARESET >> "%OUT%" 2>&1
rem a blackboard left by a failed run cannot be reopened (CreateSetupBlackboard GLE 8, "Windows setup
rem experienced an unexpected error"); phase 4 creates a new one when it is missing
if not exist "%~dp0setuplogs\blackboard" mkdir "%~dp0setuplogs\blackboard"
dir /a %WIN%\Windows\Panther\SetupInfo* >> "%OUT%" 2>&1
move /y %WIN%\Windows\Panther\SetupInfo* "%~dp0setuplogs\blackboard\" >> "%OUT%" 2>&1
dir /a %WIN%\Windows\Panther >> "%OUT%" 2>&1
:noresetsetup

rem ---- cpudiag.flag: what the last Windows RT boot made of the processors: its processor devices
rem (Enum\ACPI\ACPI0007) and processor settings, plus copies of its SYSTEM hive and System event
rem log, into .\setuplogs\cpudiag\ (read-only for Windows). ----
if not exist "%~dp0cpudiag.flag" goto nocpudiag
move /y "%~dp0cpudiag.flag" "%~dp0cpudiag.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
if not exist "%~dp0setuplogs\cpudiag" mkdir "%~dp0setuplogs\cpudiag"
set P=%~dp0setuplogs\cpudiag\cpudiag.txt
echo === cpudiag %DATE% %TIME%, Windows %WIN% > "%P%"
if not defined WIN goto nocpudiag
copy /b /y %WIN%\Windows\System32\config\SYSTEM "%~dp0setuplogs\cpudiag\SYSTEM" >> "%P%" 2>&1
copy /b /y %WIN%\Windows\System32\winevt\Logs\System.evtx "%~dp0setuplogs\cpudiag\System.evtx" >> "%P%" 2>&1
reg load HKLM\GOYACPU %WIN%\Windows\System32\config\SYSTEM >> "%P%" 2>&1
reg query HKLM\GOYACPU\ControlSet001\Enum\ACPI\ACPI0007 /s >> "%P%" 2>&1
reg query "HKLM\GOYACPU\ControlSet001\Control\Session Manager\Kernel" >>"%P%" 2>&1
reg query HKLM\GOYACPU\ControlSet001\Control\Processor >> "%P%" 2>&1
reg unload HKLM\GOYACPU >> "%P%" 2>&1
echo --- System log, errors and warnings, newest first >> "%P%"
wevtutil qe "%~dp0setuplogs\cpudiag\System.evtx" /lf:true /q:"*[System[(Level<=3)]]" /f:text /c:80 /rd:true >> "%P%" 2>&1
:nocpudiag

rem ---- noautoreboot.flag (0 or 1): Windows RT's restart after a stop (CrashControl\AutoReboot).
rem 0 keeps the stop screen up, with its parameters (DisplayParameters), for stops that happen
rem before goyabug loads. ----
if not exist "%~dp0noautoreboot.flag" goto nonoautoreboot
set NAR=
set /p NAR=<"%~dp0noautoreboot.flag"
move /y "%~dp0noautoreboot.flag" "%~dp0noautoreboot.done" >nul
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do if not defined WIN if exist %%d:\Windows\System32\config\SYSTEM if /i not "%%d:"=="%~d0" set WIN=%%d:
echo --- Windows RT AutoReboot %NAR% in %WIN% >> "%OUT%"
if not defined WIN goto nonoautoreboot
reg load HKLM\GOYACC %WIN%\Windows\System32\config\SYSTEM >> "%OUT%" 2>&1
reg add HKLM\GOYACC\ControlSet001\Control\CrashControl /v AutoReboot /t REG_DWORD /d %NAR:~0,1% /f >> "%OUT%" 2>&1
reg add HKLM\GOYACC\ControlSet001\Control\CrashControl /v DisplayParameters /t REG_DWORD /d 1 /f >> "%OUT%" 2>&1
reg query HKLM\GOYACC\ControlSet001\Control\CrashControl >> "%OUT%" 2>&1
reg unload HKLM\GOYACC >> "%OUT%" 2>&1
:nonoautoreboot

rem ---- cpus.flag (1 or 2): how many CPUs Windows RT may use. 1 = bcdedit onecpu on (what the
rem installer sets), 2 = onecpu removed, for firmware that leaves CPU1 waiting in its ACPI parking
rem page (MADT CPU1 enabled). The entry is the one in winrt.flag / winrt.done, else {default}. ----
if not exist "%~dp0cpus.flag" goto nocpus
set NCPU=
set /p NCPU=<"%~dp0cpus.flag"
move /y "%~dp0cpus.flag" "%~dp0cpus.done" >nul
set CPUID=
if exist "%~dp0winrt.done" set /p CPUID=<"%~dp0winrt.done"
if exist "%~dp0winrt.flag" set /p CPUID=<"%~dp0winrt.flag"
if not defined CPUID set CPUID={default}
echo --- CPUs for Windows RT %CPUID%: %NCPU% >> "%OUT%"
set BCDS="%~d0\EFI\Microsoft\Boot\BCD"
if "%NCPU:~0,1%"=="1" (bcdedit /store %BCDS% /set %CPUID% onecpu on >> "%OUT%" 2>&1) else (bcdedit /store %BCDS% /deletevalue %CPUID% onecpu >> "%OUT%" 2>&1)
echo bcdedit exit code %ERRORLEVEL% >> "%OUT%"
bcdedit /store %BCDS% /enum %CPUID% >> "%OUT%" 2>&1
:nocpus

rem ---- winrt.flag (holds the BCD id of the Windows RT entry): boot Windows RT once. The boot after
rem that comes back to WinPE (bootsequence is one-shot), where twrp.flag, left in place, goes to TWRP;
rem the firmware saves the Windows boot's log to \goya\logs on the way. ----
if not exist "%~dp0winrt.flag" goto nowinrt
set RTID=
set /p RTID=<"%~dp0winrt.flag"
move /y "%~dp0winrt.flag" "%~dp0winrt.done" >nul
echo --- one-time boot of Windows RT %RTID% at %TIME% >> "%OUT%"
rem after a Windows boot that did not finish, bootmgr starts the recovery sequence instead, whose
rem winload.exe this install does not have (0xc000000f): always boot Windows itself
bcdedit /store "%~d0\EFI\Microsoft\Boot\BCD" /set %RTID% recoveryenabled No >> "%OUT%" 2>&1
bcdedit /store "%~d0\EFI\Microsoft\Boot\BCD" /bootsequence %RTID% >> "%OUT%" 2>&1
echo bcdedit exit code %ERRORLEVEL% >> "%OUT%"
if errorlevel 1 goto nowinrt
rem the WinPE boot after Windows copies Setup's logs before it goes to TWRP
echo after the Windows RT boot > "%~dp0setuplogs.flag"
if exist "%~dp0setuplogs.prev" rd /s /q "%~dp0setuplogs.prev"
if exist "%~dp0setuplogs" ren "%~dp0setuplogs" setuplogs.prev
wpeutil reboot
goto :eof
:nowinrt

rem ---- twrp.flag next to this file: restart straight into TWRP (the firmware sees \goya\reboot-recovery) ----
if not exist "%~dp0twrp.flag" goto :eof
rem one-shot as well (the restart may come back to WinPE)
del "%~dp0twrp.flag"
if not exist "%~d0\goya" mkdir "%~d0\goya"
echo WinPE autorun > "%~d0\goya\reboot-recovery"
wpeutil reboot
goto :eof

rem ---- :adddrv INF - DISM /Add-Driver into the offline Windows RT install (see adddrv.flag). This
rem WinPE's DISM cannot start its servicing host for the image (dismhost: 0x80004002), so when DISM
rem fails and NAME.regdrv is next to the INF, :regdrv registers the driver directly. ----
:adddrv
echo -- %~nx1 >> "%OUT%"
dism /English /Image:%WIN%\ /Add-Driver /Driver:%1 /ForceUnsigned /LogPath:"%~dp0adddrv-dism.log" >> "%OUT%" 2>&1
set RC=%ERRORLEVEL%
echo %~nx1 dism exit code %RC% >> "%OUT%"
if "%RC%"=="0" goto :eof
if exist "%~dpn1.regdrv" (call :regdrv "%~dpn1") else (echo no %~n1.regdrv, nothing else to try >> "%OUT%")
goto :eof

rem ---- :regdrv PATH (without extension) - what the INF would do, written straight into the offline
rem install: PATH.sys into System32\drivers, the service key (+ KMDF version), and every existing
rem device instance of the hardware ID pointed at the service with its class and ConfigFlags 0, so the
rem next boot starts the driver without a driver-store install. PATH.regdrv holds HWID=, CLASS=,
rem GUID= (class GUID) and KMDF= lines. The SYSTEM hive is copied to .\adddrv first. ----
:regdrv
set RN=%~n1
set R_HWID=& set R_CLASS=& set R_GUID=& set R_KMDF=
for /f "usebackq eol=; tokens=1,* delims==" %%a in ("%~1.regdrv") do set R_%%a=%%b
echo registry install of %RN% for %R_HWID% (class %R_CLASS% %R_GUID%, KMDF %R_KMDF%) >> "%OUT%"
if not defined R_HWID (echo %RN%.regdrv has no HWID line >> "%OUT%" & goto :eof)
copy /b /y "%~1.sys" %WIN%\Windows\System32\drivers\%RN%.sys >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~1.sys" %WIN%\Windows\System32\drivers\%RN%.sys >> "%OUT%" 2>&1
echo System32\drivers\%RN%.sys compare exit code %ERRORLEVEL% >> "%OUT%"
if exist %WIN%\goya\drivers for %%e in (sys inf cat) do copy /b /y "%~1.%%e" %WIN%\goya\drivers\ >> "%OUT%" 2>&1
copy /b /y %WIN%\Windows\System32\config\SYSTEM "%~dp0adddrv\SYSTEM.before-%RN%" >> "%OUT%" 2>&1
if errorlevel 1 (echo hive backup failed, registry not changed >> "%OUT%" & goto :eof)
reg load HKLM\GOYAREG %WIN%\Windows\System32\config\SYSTEM >> "%OUT%" 2>&1
if errorlevel 1 (echo hive load failed >> "%OUT%" & goto :eof)
rem (this WinPE has find.exe but no findstr.exe)
set CS=1
for /f "tokens=3" %%v in ('reg query HKLM\GOYAREG\Select /v Current ^| find "Current"') do set /a CS=%%v
set CS=000%CS%
set RK=HKLM\GOYAREG\ControlSet%CS:~-3%
echo control set %RK% >> "%OUT%"
reg add %RK%\Services\%RN% /v Type /t REG_DWORD /d 1 /f >> "%OUT%" 2>&1
reg add %RK%\Services\%RN% /v Start /t REG_DWORD /d 3 /f >> "%OUT%" 2>&1
reg add %RK%\Services\%RN% /v ErrorControl /t REG_DWORD /d 1 /f >> "%OUT%" 2>&1
reg add %RK%\Services\%RN% /v ImagePath /t REG_EXPAND_SZ /d \SystemRoot\System32\drivers\%RN%.sys /f >> "%OUT%" 2>&1
reg add %RK%\Services\%RN% /v DisplayName /t REG_SZ /d %RN% /f >> "%OUT%" 2>&1
if defined R_KMDF reg add %RK%\Services\%RN%\Parameters\Wdf /v KmdfLibraryVersion /t REG_SZ /d %R_KMDF% /f >> "%OUT%" 2>&1
reg query "%RK%\Enum\%R_HWID%" >nul 2>&1
if errorlevel 1 (echo no %R_HWID% device in the registry yet ^(Windows has not seen it^) >> "%OUT%" & goto regdone)
set RDEV=0
for /f "delims=" %%i in ('reg query "%RK%\Enum\%R_HWID%" ^| find "HKEY_"') do call :regdev "%%i"
echo %RDEV% device instance(s) pointed at %RN% >> "%OUT%"
:regdone
reg query %RK%\Services\%RN% /s >> "%OUT%" 2>&1
reg unload HKLM\GOYAREG >> "%OUT%" 2>&1
goto :eof

rem ---- :redrvid HWID - every instance of one hardware ID (see redrv.flag) ----
:redrvid
echo %~1 >> "%OUT%"
reg query "%RK%\Enum\%~1" >nul 2>&1
if errorlevel 1 (echo   not in the registry >> "%OUT%" & goto :eof)
for /f "delims=" %%i in ('reg query "%RK%\Enum\%~1" ^| find "HKEY_"') do call :redrvdev "%%i"
goto :eof

:redrvdev
reg query "%~1" /v HardwareID >nul 2>&1
if errorlevel 1 goto :eof
echo   %~1 >> "%OUT%"
reg query "%~1" /v Service >> "%OUT%" 2>&1
reg delete "%~1" /v Service /f >> "%OUT%" 2>&1
reg add "%~1" /v ConfigFlags /t REG_DWORD /d 32 /f >> "%OUT%" 2>&1
goto :eof

rem ---- :regdev KEY - one device instance key (subkeys without a HardwareID value are skipped) ----
:regdev
echo device %~1 >> "%OUT%"
reg query "%~1" /v HardwareID >nul 2>&1
if errorlevel 1 (echo   no HardwareID, skipped >> "%OUT%" & goto :eof)
reg add "%~1" /v Service /t REG_SZ /d %RN% /f >> "%OUT%" 2>&1
if defined R_CLASS reg add "%~1" /v Class /t REG_SZ /d %R_CLASS% /f >> "%OUT%" 2>&1
if defined R_GUID reg add "%~1" /v ClassGUID /t REG_SZ /d %R_GUID% /f >> "%OUT%" 2>&1
reg add "%~1" /v ConfigFlags /t REG_DWORD /d 0 /f >> "%OUT%" 2>&1
set /a RDEV+=1
reg query "%~1" >> "%OUT%" 2>&1
goto :eof

rem ---- :instdrv NAME - install .\install\NAME.sys (see install.flag) ----
:instdrv
echo -- %1 >> "%OUT%"
if exist %DRV%\%1.sys if not exist %DRV%\%1.sys.old copy /b /y %DRV%\%1.sys %DRV%\%1.sys.old >nul
copy /b /y "%~dp0install\%1.sys" %DRV%\%1.sys >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~dp0install\%1.sys" %DRV%\%1.sys >> "%OUT%" 2>&1
echo System32\drivers\%1.sys compare exit code %ERRORLEVEL% >> "%OUT%"
if not exist %WIN%\goya\drivers goto :eof
for %%e in (sys inf cat) do if exist "%~dp0install\%1.%%e" call :stage %1 %%e
goto :eof

rem ---- :stage NAME EXT - copy NAME.EXT into \goya\drivers of the Windows partition and compare ----
:stage
copy /b /y "%~dp0install\%1.%2" %WIN%\goya\drivers\%1.%2 >> "%OUT%" 2>&1
"%~dp0goyacmp.exe" "%~dp0install\%1.%2" %WIN%\goya\drivers\%1.%2 >> "%OUT%" 2>&1
echo goya\drivers\%1.%2 compare exit code %ERRORLEVEL% >> "%OUT%"
goto :eof

rem ---- :compare NAME - byte compare of boot.wim and its NTFS copy with goyacmp.exe (WinPE has no fc.exe or script engine) ----
:compare
echo compare %1 start %TIME% >> "%OUT%"
"%~dp0goyacmp.exe" "%~d0\sources\boot.wim" %WIN%\goya\bench.tmp >> "%OUT%" 2>&1
echo compare %1 end   %TIME%  exit code %ERRORLEVEL% >> "%OUT%"
goto :eof

:wifiregkey
set /a WRN+=1
echo key %~1 >> "%OUT%"
for /f "usebackq eol=; tokens=1,2" %%a in ("%~dp0wifireg.done") do (
    reg add "%~1" /v %%a /t REG_DWORD /d %%b /f >> "%OUT%" 2>&1
    reg query "%~1" /v %%a >> "%OUT%" 2>&1
)
goto :eof
