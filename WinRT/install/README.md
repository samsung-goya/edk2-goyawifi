# Windows 8.0 RT on the microSD card (goyawifi)

`install-winrt-sd.cmd <disk#> <install.wim> [index]` on a Windows 10/11 PC (elevated). You provide your own
Windows RT 8.0 ARM `install.wim`; the script never downloads Windows files. It erases the card, applies the image,
injects the Goya drivers from `..\drivers` (unsigned, `/ForceUnsigned`), makes `goyasdhc` boot start, registers
`HalExtMmpTimer.dll`, writes the boot files + BCD (testsigning, nointegritychecks, onecpu, no recovery, `sos`),
and adds `unattend.xml` (skips OOBE: local admin `Goya`, no password, auto logon) and `SetupComplete.cmd`
(first-boot diagnostics to `C:\goya\diag.txt`).

Boot it with the EDK2 image `goyawifi-uefi-winrt-release.img` (RTC fixed to 2012-08-01 so 2012-era certificates are
still valid). Use `WinRT\winpe\make-goya-sd.cmd` first if you want to test the drivers in WinPE (runs from RAM).

Known limits: first boot is slow (polled PIO SD driver); only CPU0 is used; Windows RT code integrity may still refuse
unsigned drivers depending on the build; no display, Wi-Fi or audio drivers yet (the screen is the UEFI
framebuffer through BasicDisplay).
