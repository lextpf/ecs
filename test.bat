@echo off
REM ============================================================================
REM test.bat - Run ecs unit tests
REM ============================================================================
REM This script:
REM   1. Configures CMake if needed
REM   2. Builds the ecs_tests executable
REM   3. Runs the ecs_tests executable
REM ============================================================================

setlocal

echo ============================================================================
echo                            ECS TEST RUNNER
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: Configure CMake
REM ============================================================================
echo [1/3] Checking CMake configuration...
echo ----------------------------------------------------------------------------
if not exist "build\CMakeCache.txt" (
    echo   Configuring CMake...
    cmake --preset default
    if errorlevel 1 (
        echo ERROR: CMake configuration failed!
        pause
        exit /b 1
    )
) else (
    echo   Found existing configuration
)
echo.

REM ============================================================================
REM STEP 2: Build Tests
REM ============================================================================
echo [2/3] Building test executable...
echo ----------------------------------------------------------------------------
cmake --build build --config Release --target ecs_tests
if errorlevel 1 (
    echo ERROR: Build failed!
    pause
    exit /b 1
)
echo.

REM ============================================================================
REM STEP 3: Run Tests
REM ============================================================================
echo [3/3] Running tests...
echo ----------------------------------------------------------------------------
echo.

set ALL_PASSED=1

REM Run the suite
echo === ecs_tests ===
if exist "build\Release\ecs_tests.exe" (
    build\Release\ecs_tests.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else if exist "build\ecs_tests.exe" (
    build\ecs_tests.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else (
    echo ERROR: ecs_tests.exe not found!
    set ALL_PASSED=0
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
if %ALL_PASSED%==1 (
    echo                            ALL TESTS PASSED
) else (
    echo                            SOME TESTS FAILED
)
echo ============================================================================

pause
if %ALL_PASSED%==1 (
    exit /b 0
) else (
    exit /b 1
)
