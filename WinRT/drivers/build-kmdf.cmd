@echo off
rem Builds one Goya KMDF driver (ARM32) from the command line with the Windows 10 19041 WDK.
rem   build-kmdf.cmd goyagpio
rem Run from an ARM-targeting VS developer prompt (vcvarsall x86_arm) with DDK_INC_PATH set to
rem ...\Windows Kits\10\Include\10.0.19041.0, as in .github\workflows\goya-drivers.yml.
rem
rem KMDF version: Windows 8.0 ships KMDF 1.11; the 19041 WDK has the 1.11 headers/libs, so the
rem driver is built against 1.11 and can load on 8.0 RT (if nothing else it uses is newer).
rem
rem NOTE: "%ProgramFiles(x86)%" contains a ")" which breaks cmd's parenthesised blocks, so every
rem path is first converted to its 8.3 short form (no parentheses, no spaces).
setlocal EnableExtensions
set N=%1
if "%N%"=="" goto usage
if not defined DDK_INC_PATH goto nowdk
for %%I in ("%DDK_INC_PATH%") do set "INCV=%%~sI"
for %%I in ("%DDK_INC_PATH%\..\..") do set "KITS=%%~sI"
for %%I in ("%~dp0%N%") do set "SRC=%%~sI"
cd /d %SRC% || exit /b 1

set KMINOR=11
set KM=%KITS%\Include\wdf\kmdf\1.%KMINOR%
set KMLIB=%KITS%\Lib\wdf\kmdf\arm\1.%KMINOR%
if not exist %KM%\wdf.h (echo KMDF 1.%KMINOR% headers missing: %KM% & exit /b 1)
if not exist %KMLIB% (echo KMDF 1.%KMINOR% libs missing: %KMLIB% & exit /b 1)

echo ---- libraries mentioning gpio / spb / wdf for ARM ----
dir /s /b %KITS%\Lib\10.0.19041.0\km\arm\*gpio*.lib %KITS%\Lib\10.0.19041.0\km\arm\*spb*.lib 2>nul
dir /b %KMLIB%
echo ---- how gpioclx.h binds ----
findstr /n /i /c:"pragma comment" /c:"Stubs" /c:"GpioClxFunctions" %INCV%\km\gpioclx.h 2>nul
echo -------------------------------------------------------

set DEFS=/DARM /D_ARM_ /DNDEBUG /D_WIN32_WINNT=0x0602 /DNTDDI_VERSION=0x06020000 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=%KMINOR% /DWINNT=1
echo ---- spbcx.h locations ----
dir /s /b %KITS%\Include\10.0.19041.0\spbcx.h %KITS%\Include\10.0.19041.0\spb.h 2>nul
set SPBINC=
for /d %%D in (%INCV%\km\spb\*) do set SPBINC=/I%%D
if exist %INCV%\km\spb set SPBINC=%SPBINC% /I%INCV%\km\spb
echo SPBINC=%SPBINC%
set INC=/I%INCV% /I%INCV%\km /I%INCV%\km\crt /I%INCV%\shared /I%KM% %SPBINC%
cl /nologo /c /kernel /GS- /Oi /Zi /W3 /wd4201 /wd4214 /wd4100 %DEFS% %INC% %N%.c ..\common\goyamem.c || exit /b 1

set EXTRA=
if exist %KITS%\Lib\10.0.19041.0\km\arm\msgpioclxstub.lib set EXTRA=%KITS%\Lib\10.0.19041.0\km\arm\msgpioclxstub.lib
if exist %KITS%\Lib\10.0.19041.0\km\arm\gpio\1.0\gpioclxstubs.lib set EXTRA=%KITS%\Lib\10.0.19041.0\km\arm\gpio\1.0\gpioclxstubs.lib
if exist %KITS%\Lib\10.0.19041.0\km\arm\spb\1.0\spbcxstubs.lib set EXTRA=%EXTRA% %KITS%\Lib\10.0.19041.0\km\arm\spb\1.0\spbcxstubs.lib
rem the battery miniclass driver links against the inbox battery class driver
if /i "%N%"=="goyabatt" set EXTRA=%EXTRA% %KITS%\Lib\10.0.19041.0\km\arm\battc.lib
echo extra libs: %EXTRA%
link /nologo /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE /ENTRY:FxDriverEntry /INTEGRITYCHECK /LIBPATH:%KMLIB% /LIBPATH:%KITS%\Lib\10.0.19041.0\km\arm ^
     /OUT:%N%.sys %N%.obj goyamem.obj WdfDriverEntry.lib WdfLdr.lib ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib libcntpr.lib %EXTRA% || exit /b 1
rem No NEON/VFP: the Windows 8 ARM kernel does not enable it for kernel-mode code
dumpbin /nologo /disasm %N%.sys > %N%.dis
findstr /r /c:" vld" /c:" vst" /c:" vmov" /c:" vdup" /c:" vmrs" /c:" vmsr" %N%.dis && (echo ERROR: %N%.sys contains VFP/NEON instructions & exit /b 1)
rem No hardware divide: the PXA988 Cortex-A9 has no SDIV/UDIV (undefined instruction, C000001D)
findstr /r /c:" sdiv" /c:" udiv" %N%.dis && (echo ERROR: %N%.sys contains SDIV/UDIV & exit /b 1)
echo built %N%.sys ^(KMDF 1.%KMINOR%^)
exit /b 0

:usage
echo usage: build-kmdf.cmd name
exit /b 1
:nowdk
echo run from a WDK/VS ARM prompt with DDK_INC_PATH set
exit /b 1
