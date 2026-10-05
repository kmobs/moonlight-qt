@echo off
rem Alignment adds driver queries around Present and may perturb timing.
rem Default composition reports display events; raster alignment probes require MOONLIGHT_VRR_COMPOSITION=0.
rem Main trace schema 5 and passive GPU sidecars retain exact historical replay.
call "%~dp0Moonlight VRR Diagnostic.cmd" --align
exit /b %errorlevel%
