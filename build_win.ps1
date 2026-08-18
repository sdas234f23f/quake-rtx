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

# Package the generated RT renderer shaders into <game>/shaders.pkz. The
# renderer loads them through the engine file system (pfnOpenFile ->
# COM_FindFile); mounted .pkz archives are searched BEFORE the game dir, so
# the shaders are found even without a loose shaders/ folder. Entries must be
# "shaders/<name>.spv" to match the renderer's shader folder path.
$gameDir = Join-Path $BuildDir "id1"
$shaderOutDir = Join-Path $PSScriptRoot "subprojects\vkpt\Build"
if (Test-Path (Join-Path $shaderOutDir "*.spv")) {
    python (Join-Path $PSScriptRoot "Tools\zip_shaders.py") `
        $shaderOutDir (Join-Path $gameDir "shaders.pkz") "shaders"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # the loose shaders/ folder is no longer needed (pkz is searched first)
    $shadersLoose = Join-Path $gameDir "shaders"
    if (Test-Path $shadersLoose) { Remove-Item $shadersLoose -Recurse -Force }
}

# Q2RTX material overrides: deploy the repo's materials/*.mat (VCS source of
# truth at subprojects/vkpt/Source/materials) into the game dir. The .pkz must
# NOT contain ovrd.mat - the material loader checks .pkz archives first, so a
# stale copy there would win over this deployed loose file.
$matSrcDir = Join-Path $PSScriptRoot "subprojects\vkpt\Source\materials"
if (Test-Path $matSrcDir) {
    $matsOut = Join-Path $gameDir "materials"
    if (-not (Test-Path $matsOut)) {
        New-Item -ItemType Directory -Path $matsOut -Force | Out-Null
    }
    Copy-Item (Join-Path $matSrcDir "*.mat") $matsOut -Force
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
            $dst = Join-Path $gameDir $pkz.Name
            # tolerate transient locks (Search indexer / antivirus): if the
            # target can't be replaced it is already present from a previous
            # build, so just warn and continue
            try {
                Remove-Item $dst -Force -ErrorAction Stop
                Copy-Item $pkz.FullName $dst -Force -ErrorAction Stop
            }
            catch {
                Write-Warning "could not refresh $($pkz.Name) (file busy): $($_.Exception.Message)"
            }
        }
    }
}

exit 0
