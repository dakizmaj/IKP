@echo off
echo ================================================
echo  Energy Distribution System - Extended
echo  8 Agregatora + 5 Destinacija
echo ================================================
echo.

REM  PRVO ZATVORI SVE STARE INSTANCE
echo Zatvaram stare procese...
taskkill /F /IM Source.exe >nul 2>&1
taskkill /F /IM Aggregator.exe >nul 2>&1
taskkill /F /IM Destination.exe >nul 2>&1
timeout /t 2 /nobreak >nul
echo OK - Procesi zatvoreni
echo.

set BUILD_DIR=x64\Debug

REM Pokreni sve Destinations
echo [1-5] Pokretanje Destinations...
start "Dest-1" "%BUILD_DIR%\Destination.exe" 1
start "Dest-2" "%BUILD_DIR%\Destination.exe" 2
start "Dest-3" "%BUILD_DIR%\Destination.exe" 3
start "Dest-4" "%BUILD_DIR%\Destination.exe" 4
start "Dest-5" "%BUILD_DIR%\Destination.exe" 5
timeout /t 2 /nobreak >nul

REM Pokreni leaf agregatore (7, 6, 5, 4, 3)
echo [6-10] Pokretanje leaf agregatora...
start "Agr-7" "%BUILD_DIR%\Aggregator.exe" 7
start "Agr-6" "%BUILD_DIR%\Aggregator.exe" 6
start "Agr-5" "%BUILD_DIR%\Aggregator.exe" 5
start "Agr-4" "%BUILD_DIR%\Aggregator.exe" 4
start "Agr-3" "%BUILD_DIR%\Aggregator.exe" 3
timeout /t 2 /nobreak >nul

REM Pokreni srednji nivo (2, 1)
echo [11-12] Pokretanje srednjih agregatora...
start "Agr-2" "%BUILD_DIR%\Aggregator.exe" 2
start "Agr-1" "%BUILD_DIR%\Aggregator.exe" 1
timeout /t 2 /nobreak >nul

REM Pokreni root
echo [13] Pokretanje ROOT agregatora...
start "Agr-0" "%BUILD_DIR%\Aggregator.exe" 0
timeout /t 2 /nobreak >nul

REM Pokreni Source
echo [14] Pokretanje Source...
start "Source" "%BUILD_DIR%\Source.exe"

echo.
echo ================================================
echo  Sistem pokrenut!
echo ================================================
echo.
pause