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

# Deploy shaders + textures + configs into the build's game dir.
$ovrdOut = Join-Path $BuildDir "ovrd"
if (Test-Path (Join-Path $shaderOutDir "*.spv")) {
    if (-not (Test-Path (Join-Path $ovrdOut "shaders"))) {
        New-Item -ItemType Directory -Path (Join-Path $ovrdOut "shaders") -Force | Out-Null
    }
    Copy-Item (Join-Path $shaderOutDir "*.spv") (Join-Path $ovrdOut "shaders") -Force
}
$ovrdSrc = Join-Path $PSScriptRoot "ovrd"
if (Test-Path $ovrdSrc) {
    if (-not (Test-Path $ovrdOut)) { New-Item -ItemType Directory -Path $ovrdOut -Force | Out-Null }
    Copy-Item (Join-Path $ovrdSrc "*") $ovrdOut -Force
}

# Override-material pack (id1/ovrd_mat.pkz, checked in): the game loads its
# material overrides (emissive lava, normal maps, ...) from this .pkz.
$pkzSrc  = Join-Path $PSScriptRoot "id1\ovrd_mat.pkz"
$gameDir = Join-Path $BuildDir "id1"
if (Test-Path $pkzSrc) {
    if (-not (Test-Path $gameDir)) { New-Item -ItemType Directory -Path $gameDir -Force | Out-Null }
    Copy-Item $pkzSrc (Join-Path $gameDir "ovrd_mat.pkz") -Force
}

exit 0
