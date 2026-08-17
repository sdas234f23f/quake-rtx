# Builds the project with CMake + Ninja using the MSVC compiler from Visual Studio Build Tools.
# Usage: .\build_win.ps1 [Debug|Release] [build-dir]

param(
    [string]$Config = "Release",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path "build" $Config
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio Build Tools with the C++ workload are not installed."
}

# Import the MSVC environment (INCLUDE, LIB, PATH, etc.) into this session.
$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$envLines = cmd /c "`"$devCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match "^([^=]+)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

$cmakeArgs = @("-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ============================================================================
# RT renderer assets (only needed when rt_renderer is 1)
# ============================================================================
# Shaders: generate .spv with glslc from the vendored vkpt sources unless they
# are already built. The exe and the .spv must always come from the same
# source version -- if the vkpt shaders change, delete subprojects/vkpt/Build.
$shaderSrcDir = Join-Path $PSScriptRoot "subprojects\vkpt\Source\Shaders"
$shaderOutDir = Join-Path $PSScriptRoot "subprojects\vkpt\Build"
if (-not (Test-Path (Join-Path $shaderOutDir "*.spv"))) {
    $glslc = Get-Command glslc -ErrorAction SilentlyContinue
    if (-not $glslc -and $env:VULKAN_SDK) {
        $sdkGlslc = Join-Path $env:VULKAN_SDK "Bin\glslc.exe"
        if (Test-Path $sdkGlslc) { $glslc = $sdkGlslc }
    }
    if (-not $glslc) {
        # try the default SDK install locations
        $sdkRoots = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        foreach ($sdk in $sdkRoots) {
            $candidate = Join-Path $sdk.FullName "Bin\glslc.exe"
            if (Test-Path $candidate) { $glslc = $candidate; break }
        }
    }
    if ($glslc) {
        Write-Host "Generating RT renderer shaders (glslc)..."
        $oldPath = $env:PATH
        $env:PATH = (Split-Path $glslc -Parent) + ";" + $env:PATH
        Push-Location $shaderSrcDir
        python GenerateShaders.py
        $genExit = $LASTEXITCODE
        Pop-Location
        $env:PATH = $oldPath
        if ($genExit -ne 0) { exit $genExit }
    }
    else {
        Write-Warning "glslc not found -- RT renderer shaders were not generated (install the Vulkan SDK or set rt_renderer back to 0)."
    }
}

# Deploy the RT renderer shaders into the build's game dir (id1/shaders); the
# renderer loads them through the engine file system (pkz-aware).
$gameDir = Join-Path $BuildDir "id1"
if (Test-Path (Join-Path $shaderOutDir "*.spv")) {
    $shadersOut = Join-Path $gameDir "shaders"
    if (-not (Test-Path $shadersOut)) {
        New-Item -ItemType Directory -Path $shadersOut -Force | Out-Null
    }
    Copy-Item (Join-Path $shaderOutDir "*.spv") $shadersOut -Force
}

# Runtime configs/textures (checked in under ovrd/): texture_custom_info.txt,
# world_custom_lights.txt, world_custom_portals.txt, WaterNormal_n.ktx2.
$ovrdSrc = Join-Path $PSScriptRoot "ovrd"
if (Test-Path $ovrdSrc) {
    if (-not (Test-Path $gameDir)) { New-Item -ItemType Directory -Path $gameDir -Force | Out-Null }
    Copy-Item (Join-Path $ovrdSrc "*") $gameDir -Force
}

# .pkz packs (checked in under id1/ or ovrd/): material overrides, blue noise,
# Q2RTX shaders. The game loads them all through the engine file system.
foreach ($pkzRoot in @((Join-Path $PSScriptRoot "id1"), (Join-Path $PSScriptRoot "ovrd"))) {
    $pkzFiles = Get-ChildItem $pkzRoot -Filter "*.pkz" -ErrorAction SilentlyContinue
    if ($pkzFiles) {
        if (-not (Test-Path $gameDir)) { New-Item -ItemType Directory -Path $gameDir -Force | Out-Null }
        foreach ($pkz in $pkzFiles) {
            Copy-Item $pkz.FullName (Join-Path $gameDir $pkz.Name) -Force
        }
    }
}

exit 0
