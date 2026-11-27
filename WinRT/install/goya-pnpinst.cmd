@echo off
rem ===========================================================================
rem  goya-pnpinst.cmd - runs once inside Windows RT (HKLM RunOnce, set by the card's WinPE pnpinst.flag
rem  step) from C:\goya\pnpinst: trusts the kit's test certificate, then adds and installs every driver
rem  package listed in pnpinst.list with pnputil. The log is copied back to the card's WinPE partition
rem  (goyape\pnpinst-result.log), which TWRP can read; the NTFS partition it cannot.
rem ===========================================================================
setlocal
set D=%~dp0
set L=%D%pnpinst.log
echo ==== %date% %time% >> "%L%"
whoami /groups | find "S-1-16-12288" >nul && (echo elevated: yes >> "%L%") || (echo elevated: NO >> "%L%")
certutil -f -addstore Root "%D%GoyaTestCert.cer" >> "%L%" 2>&1
certutil -f -addstore TrustedPublisher "%D%GoyaTestCert.cer" >> "%L%" 2>&1
for /f "usebackq eol=; delims=" %%n in ("%D%pnpinst.list") do call :one %%n
echo ==== done %time% >> "%L%"
for %%d in (D E F G H I J K L M N O P Q R S T U V W Y Z) do if exist %%d:\goyape\autorun.cmd copy /y "%L%" %%d:\goyape\pnpinst-result.log >nul
goto :eof

:one
echo -- %1 >> "%L%"
pnputil -i -a "%D%%1.inf" >> "%L%" 2>&1
echo %1 pnputil exit code %ERRORLEVEL% >> "%L%"
goto :eof
