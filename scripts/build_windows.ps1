param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $env:VULKAN_SDK) {
    throw "VULKAN_SDK is not set. Install the LunarG Vulkan SDK, then open a new terminal."
}

if (-not $VcpkgRoot) {
    throw "VCPKG_ROOT is not set. Pass -VcpkgRoot C:\\dev\\vcpkg or set the environment variable."
}

$Toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    throw "vcpkg toolchain not found at $Toolchain"
}

cmake -S $ProjectRoot -B "$ProjectRoot/build" `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain"

cmake --build "$ProjectRoot/build" --config $Config

$Exe = Join-Path $ProjectRoot "build/$Config/vulkan_shader_starter.exe"
if (-not (Test-Path $Exe)) {
    throw "Executable not found at $Exe"
}

& $Exe

