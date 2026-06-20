# EchoRadar Developer Setup Guide

This guide walks you through setting up your development environment for EchoRadar on Windows.

## 1. Required Software

Before building EchoRadar, ensure you have installed:

| Tool | Version | Download |
|------|---------|----------|
| **Windows 10/11** | Build 19041+ | System |
| **Visual Studio 2022** | Community Edition or higher | https://visualstudio.microsoft.com/downloads/ |
| **CMake** | ≥ 3.20 | https://cmake.org/download/ |
| **Git** | Latest | https://git-scm.com/download/win |
| **Windows SDK** | 10.0 (for DirectX 11) | Installed via Visual Studio |

## 2. Visual Studio Installation

### Installation Steps

1. Download **Visual Studio 2022 Community Edition** from https://visualstudio.microsoft.com/downloads/
2. Run the installer
3. Select **Desktop development with C++** workload
4. Verify the following components are installed:
   - ✓ MSVC v143 x64/x86 build tools (latest)
   - ✓ CMake tools for Windows
   - ✓ Windows 10/11 SDK
   - ✓ C++ ATL
   - ✓ C++ Clang tools

### Verification

```powershell
# Check MSVC compiler
& "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.3*\bin\Hostx64\x64\cl.exe" -?
```

## 3. CMake Installation

### Installation Steps

1. Download CMake installer from https://cmake.org/download/
2. Run the `.msi` installer (recommended version ≥ 3.20)
3. **During installation**, select: ✓ "Add CMake to the system PATH for all users"
4. Complete the installation

### Verification

```powershell
cmake --version
# Expected output: cmake version 3.20 or higher
```

## 4. Build Commands

### Clean Build

```powershell
# Navigate to repository root
cd C:\path\to\EchoRadar

# Create/configure build directory (Release mode)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile the project
cmake --build build --config Release

# Optional: Build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Release -DECHORADAR_BUILD_TESTS=ON
cmake --build build --config Release
```

### Debug Build

```powershell
# For debugging with symbols
cmake -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug --config Debug
```

### Incremental Build

```powershell
# Rebuild only changed files
cmake --build build --config Release
```

### Clean Build (Remove all artifacts)

```powershell
# Remove build directory and rebuild from scratch
Remove-Item -Recurse build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## 5. Run Commands

### Run Main Executable

```powershell
# From repository root
.\build\src\app\Release\echo_sandbox.exe

# Expected output:
# EchoRadar initialized
```

### Run with Debugging

```powershell
# Open in Visual Studio debugger
cd build
cmake --open .
# In Visual Studio: Right-click echo_sandbox target → Set as Startup Project → Press F5
```

## 6. Test Commands

### Run All Tests

```powershell
# Run all unit tests
ctest --test-dir build --output-on-failure

# Run tests with verbose output
ctest --test-dir build --output-on-failure -VV
```

### Run Specific Test Suite

```powershell
# List all available tests
ctest --test-dir build --print-labels

# Run specific test by name
ctest --test-dir build -R "TestPattern" --output-on-failure
```

### Build Tests Only

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DECHORADAR_BUILD_TESTS=ON
cmake --build build --config Release --target RUN_TESTS
```

## Troubleshooting

### CMake configuration fails

**Problem**: "CMake not found" or "Unknown generator"

**Solution**:
```powershell
# Ensure CMake is in PATH
cmake --version

# If not found, add to PATH manually or reinstall with PATH option
```

### MSVC compiler not found

**Problem**: "Could not find MSVC compiler"

**Solution**:
1. Reinstall Visual Studio 2022 with C++ workload
2. Verify installation:
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
   cl.exe -?
   ```

### FetchContent fails to download dependencies

**Problem**: Network error downloading KissFFT, miniaudio, or GoogleTest

**Solution**:
```powershell
# Clean CMake cache and retry
Remove-Item build -Recurse
cmake -B build -DCMAKE_BUILD_TYPE=Release
# If still fails, check internet connection and proxy settings
```

### DirectX 11 headers not found (Windows SDK missing)

**Problem**: `#include <d3d11.h>` not found

**Solution**:
1. Run Visual Studio Installer
2. Modify installation → Add Windows 10/11 SDK if not present
3. Rebuild: `cmake --build build --clean-first`

## Development Workflow

### Typical development session:

```powershell
# 1. Clone/enter repository
git clone https://github.com/inbannable/EchoRadar.git
cd EchoRadar

# 2. Configure (once per branch)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# 3. Develop and build iteratively
# Edit source files...
cmake --build build --config Debug

# 4. Run tests
ctest --test-dir build --output-on-failure

# 5. Run application
.\build\src\app\Debug\echo_sandbox.exe
```

## CI/CD Build

For automated builds (CI/CD pipeline):

```powershell
# Deterministic build (no interactive prompts)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DECHORADAR_BUILD_TESTS=ON -DECHORADAR_BUILD_TOOLS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure --no-tests=error
```

## Next Steps

- Review [Architecture Documentation](README.md#Architecture)
- Check individual module READMEs in `src/*/`
- Run unit tests: `ctest --test-dir build --output-on-failure`
- Start with [Milestone 1: AudioCapture](docs/ROADMAP.md)
