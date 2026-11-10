# T5Bridge

The native data and scripting backend for T54UE. It uses OpenMW components to read Skyrim Special Edition and Anniversary Edition assets and records, prepare scene data for Unreal Engine, and execute supported Papyrus scripts.

The Unreal client, **T54UE**, lives in a separate repository. Use this repository’s `bridge` branch and keep both checkouts side by side when building the client.

```text
workspace/
├── T5Bridge/
│   ├── Build.ps1
│   └── CMakeLists.txt
└── T54UE/
    ├── T54UE.uproject
    └── Data/
```

## Build on Windows

Install CMake, Visual Studio 2026 Build Tools with C++ and a Windows SDK. Extract the `vcpkg-x64-windows-2022-2026-10-10.7z` dependency archive. CMake fetches additional source dependencies during the first configuration. [OpenMW dependency release](https://github.com/OpenMW/openmw-deps-build/releases/tag/2026-10-10)

From the repository root in PowerShell, set the path to the extracted dependency directory and run the build.

```powershell
$Dependencies = 'C:\deps\vcpkg-x64-windows-2022-2026-10-10'
.\Build.ps1 -Dependencies $Dependencies
```

The bridge builds to `build/Release/T5DataBridge.dll`. For the full client setup, run `Build.ps1` from the neighboring T54UE checkout. It builds and installs the bridge alongside the Unreal client.

## Run the native regression tests

```powershell
.\Build.ps1 -Dependencies $Dependencies -Targets t5native_tests,tes5route_tests,t5movement_tests
$DependencyBin = Join-Path $Dependencies 'installed\x64-windows\bin'
$PreviousPath = $env:Path
try {
    $env:Path = "$DependencyBin;$DependencyBin\Release;$PreviousPath"
    foreach ($Test in 't5native_tests', 'tes5route_tests', 't5movement_tests') {
        & ".\build\Release\$Test.exe"
        if ($LASTEXITCODE) { throw "$Test failed with exit code $LASTEXITCODE" }
    }
} finally {
    $env:Path = $PreviousPath
}
```

## Source and licensing

This OpenMW fork contains the Skyrim resource loaders and Papyrus runtime used by T54UE. The OpenMW-derived source is distributed under the [GNU General Public License version 3](LICENSE). Upstream copyright notices, author credits and component licenses remain in the fork. [OpenMW](https://openmw.org/) · [Upstream authors](AUTHORS.md) · [Upstream source](https://gitlab.com/OpenMW/openmw)

The repository is distributed as source. Skyrim assets, local mods, the dependency bundle and generated binaries are supplied or built locally.

The current T54UE build loads `T5DataBridge.dll` in the Unreal process. License compatibility for the in-process client integration is a separate consideration. [Unreal Engine EULA §6(c)](https://www.unrealengine.com/eula/unreal)
