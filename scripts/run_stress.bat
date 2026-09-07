@echo off
setlocal

rem GPU-driven stress benchmark.
rem Usage: run_stress.bat [instance-count] [frames]

set "ROOT_DIR=%~dp0.."
rem Override with:
rem   set HALCYON_EXE=D:\path\to\HalcyonExample03PbrScenes.exe
if "%HALCYON_EXE%"=="" set "HALCYON_EXE=%ROOT_DIR%\out\build\gpu-driven-msvc-relwithdebinfo\Examples\HalcyonExample03PbrScenes\HalcyonExample03PbrScenes.exe"
if not exist "%HALCYON_EXE%" if "%HALCYON_EXE%"=="%ROOT_DIR%\out\build\gpu-driven-msvc-relwithdebinfo\Examples\HalcyonExample03PbrScenes\HalcyonExample03PbrScenes.exe" set "HALCYON_EXE=%ROOT_DIR%\out\build\clang-check\Examples\HalcyonExample03PbrScenes\HalcyonExample03PbrScenes.exe"
set "EXE=%HALCYON_EXE%"
set "OUTPUT_DIR=%ROOT_DIR%\out\captures"
set "GPU_OUTPUT_CSV=%OUTPUT_DIR%\stress-gpu.csv"
set "LEGACY_OUTPUT_CSV=%OUTPUT_DIR%\stress-cpu.csv"
set "INSTANCE_COUNT=%~1"
set "FRAMES=%~2"

if "%INSTANCE_COUNT%"=="" set "INSTANCE_COUNT=100000"
if "%FRAMES%"=="" set "FRAMES=8"

if not exist "%EXE%" (
    echo ERROR: executable not found:
    echo   "%EXE%"
    echo Build HalcyonExample03PbrScenes first, then run this script again.
    exit /b 1
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

echo Running GPU-driven stress benchmark...
echo   Instances: %INSTANCE_COUNT%
echo   Frames:    %FRAMES%
echo   GPU CSV:   "%GPU_OUTPUT_CSV%"
echo   CPU CSV:   "%LEGACY_OUTPUT_CSV%"

pushd "%ROOT_DIR%"
echo.
echo [1/2] Running GPU-driven path...
"%EXE%" ^
    --scene stress ^
    --instance-count %INSTANCE_COUNT% ^
    --frames %FRAMES% ^
    --fixed-dt 0.016666 ^
    --no-taa ^
    --gpu-driven ^
    --two-phase-occlusion ^
    --no-validation ^
    --perf-csv "%GPU_OUTPUT_CSV%"
set "GPU_EXIT_CODE=%ERRORLEVEL%"

echo.
echo [2/2] Running traditional CPU path...
"%EXE%" ^
    --scene stress ^
    --instance-count %INSTANCE_COUNT% ^
    --frames %FRAMES% ^
    --fixed-dt 0.016666 ^
    --no-taa ^
    --no-gpu-driven ^
    --no-validation ^
    --perf-csv "%LEGACY_OUTPUT_CSV%"
set "LEGACY_EXIT_CODE=%ERRORLEVEL%"
popd

if not "%GPU_EXIT_CODE%"=="0" (
    echo GPU-driven stress benchmark failed with exit code %GPU_EXIT_CODE%.
) else if not "%LEGACY_EXIT_CODE%"=="0" (
    echo Traditional CPU stress benchmark failed with exit code %LEGACY_EXIT_CODE%.
) else (
    echo Both stress benchmarks completed successfully.
)

set "EXIT_CODE=0"
if not "%GPU_EXIT_CODE%"=="0" set "EXIT_CODE=%GPU_EXIT_CODE%"
if not "%LEGACY_EXIT_CODE%"=="0" set "EXIT_CODE=%LEGACY_EXIT_CODE%"
exit /b %EXIT_CODE%
