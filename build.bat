@echo off
REM ===========================================================================================
REM build.bat - Build pipeline for ecs (format, configure, tidy, build).
REM             Does NOT run tests -- use test.bat for that.
REM ===========================================================================================
REM This script:
REM   1. clang-format - in-place formatting of src/ tests/
REM   2. cmake        - CMake configure with vcpkg manifest install and VS 17 2022 generator
REM   3. clang-tidy   - static analysis via Ninja compile_commands.json sidecar (build-cdb/)
REM   4. build        - Release build via cmake --build (compiles tests too, but
REM                     does not run them; test.bat runs the suite)
REM ===========================================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                              ECS BUILD PIPELINE
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: Run clang-format
REM ============================================================================
echo [1/4] Running clang-format...
echo ----------------------------------------------------------------------------

where clang-format >nul 2>&1
if errorlevel 1 (
    echo SKIP: clang-format not found in PATH
) else (
    for %%f in (src\*.hpp tests\*.cpp tests\*.hpp) do (
        if exist "%%f" clang-format -i "%%f"
    )
    echo Formatting complete.
)
echo.

REM ============================================================================
REM STEP 2: CMake Configuration
REM ============================================================================
echo [2/4] Configuring with CMake...
echo ----------------------------------------------------------------------------
cmake --preset default
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 3: Run clang-tidy
REM ============================================================================
echo [3/4] Running clang-tidy...
echo ----------------------------------------------------------------------------

where clang-tidy >nul 2>&1
if errorlevel 1 (
    echo SKIP: clang-tidy not found in PATH
) else (
    REM Reconfigure the sidecar every run: new test TUs must appear in the
    REM database or clang-tidy errors out on them.
    echo   Generating compile_commands.json via Ninja sidecar...
    cmake --preset compile-db >nul
    if !ERRORLEVEL! neq 0 (
        echo ERROR: compile-db configure failed
        exit /b 1
    )

    REM The sidecar preset compiles with clang++ directly, so the database is
    REM consumed as-is - no driver-mode tricks or normalization required.
    for %%f in (tests\*.cpp) do (
        if exist "%%f" (
            echo   tidy: %%f
            clang-tidy --quiet -p build-cdb "%%f"
            if !ERRORLEVEL! neq 0 (
                echo ERROR: clang-tidy reported issues in %%f
                exit /b 1
            )
        )
    )
    echo clang-tidy complete.
)
echo.

REM ============================================================================
REM STEP 4: Build Release
REM ============================================================================
echo [4/4] Building Release targets...
echo ----------------------------------------------------------------------------
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed
    exit /b %ERRORLEVEL%
)
echo.

echo ============================================================================
echo                           BUILD PIPELINE COMPLETE
echo ============================================================================
echo.
echo Build Output:
echo   Tests:     build\Release\ecs_tests.exe
echo.
echo ============================================================================

endlocal
exit /b 0
