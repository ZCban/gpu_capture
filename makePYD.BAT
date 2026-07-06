@echo off
title Build gpu_capture

echo.
echo ============================
echo Pulizia...
echo ============================

if exist build (
    rmdir /s /q build
)

del /q *.pyd 2>nul

del /q *.exp 2>nul
del /q *.lib 2>nul

echo.
echo ============================
echo Compilazione...
echo ============================

python setup.py build_ext --inplace

echo.
if errorlevel 1 (
    echo *************************************
    echo BUILD FALLITA!
    echo *************************************
) else (
    echo *************************************
    echo BUILD COMPLETATA CON SUCCESSO!
    echo *************************************
)

echo.
pause