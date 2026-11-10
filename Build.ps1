[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Dependencies,
    [string]$Generator = 'Visual Studio 18 2026',
    [ValidateSet('T5DataBridge', 't5native_tests', 't5movement_tests', 'tes5route_tests', 't5model_probe')]
    [string[]]$Targets = @('T5DataBridge'),
    [ValidateRange(1, 32)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$build = Join-Path $PSScriptRoot 'build'
$dependenciesRoot = (Resolve-Path -LiteralPath $Dependencies).Path.Replace('\', '/')
$toolchain = "$dependenciesRoot/scripts/buildsystems/vcpkg.cmake"
$installed = "$dependenciesRoot/installed/x64-windows"
if (!(Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "Missing OpenMW dependency toolchain at $toolchain"
}

$options = @(
    '-S', $source, '-B', $build, '-G', $Generator, '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DLuaJit_INCLUDE_DIR=$installed/include/luajit",
    "-DLuaJit_LIBRARY=$installed/lib/lua51.lib",
    '-DBUILD_TES5_UE_BRIDGE=ON', '-DBUILD_OPENMW=OFF', '-DBUILD_LAUNCHER=OFF', '-DBUILD_WIZARD=OFF',
    '-DBUILD_MWINIIMPORTER=OFF', '-DBUILD_OPENCS=OFF', '-DBUILD_ESSIMPORTER=OFF', '-DBUILD_BSATOOL=OFF',
    '-DBUILD_ESMTOOL=OFF', '-DBUILD_NIFTEST=OFF', '-DBUILD_NAVMESHTOOL=OFF', '-DBUILD_BULLETOBJECTTOOL=OFF',
    '-DBUILD_TESTING=OFF', '-DOPENMW_USE_SYSTEM_SQLITE3=OFF', '-DOPENMW_USE_SYSTEM_YAML_CPP=OFF',
    '-DYAML_CPP_STATIC=ON', '-DOPENMW_MP_BUILD=ON', '-DOPENMW_DEPLOY_RUNTIME_DEPENDENCIES=OFF'
)
& cmake @options
if ($LASTEXITCODE -ne 0) { throw "Backend configuration failed with exit code $LASTEXITCODE" }
& cmake --build $build --config Release --target @Targets --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Backend build failed with exit code $LASTEXITCODE" }
