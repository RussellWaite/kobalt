param(
    [ValidateSet("mingw", "msvc")]
    [string]$Toolchain = "mingw",
    [string]$ConfigurePreset = "windows-mingw",
    [string]$BuildPreset = "windows-mingw",
    [string]$QtPrefixPath = "",
    [switch]$Fresh,
    [switch]$SkipQtDeploy
)

$ErrorActionPreference = "Stop"

function Resolve-CMakePath {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "CMake was not found. Install CMake 3.22+ and ensure cmake.exe is available."
}

function Resolve-VcVarsPath {
    if ($env:VCVARS64_BAT -and (Test-Path $env:VCVARS64_BAT)) {
        return $env:VCVARS64_BAT
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $fallbackRoots = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    )
    foreach ($root in $fallbackRoots) {
        $candidate = Join-Path $root "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "vcvars64.bat was not found. Install Visual Studio 2022 Build Tools with the C++ workload."
}

function Resolve-QtPrefixPath {
    param([string]$RequestedPath)

    $pathsToCheck = @()
    if ($RequestedPath) {
        $pathsToCheck += $RequestedPath
    }
    if ($env:KOBALT_QT_PREFIX) {
        $pathsToCheck += $env:KOBALT_QT_PREFIX
    }
    if ($Toolchain -eq "msvc") {
        $pathsToCheck += @(
            "D:\dev\Qt\6.11.3\msvc2022_64",
            "C:\Qt\6.11.3\msvc2022_64",
            "C:\Qt\6.11.2\msvc2022_64",
            "C:\Qt\6.11.1\msvc2022_64",
            "C:\Qt\6.11.0\msvc2022_64"
        )
    } else {
        $pathsToCheck += @(
            "D:\dev\Qt\6.11.3\mingw_64",
            "C:\Qt\6.11.3\mingw_64",
            "C:\Qt\6.11.2\mingw_64"
        )
    }

    foreach ($path in $pathsToCheck | Select-Object -Unique) {
        if (-not $path) {
            continue
        }
        if (Test-Path (Join-Path $path "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $path
        }
    }

    return ""
}

function Resolve-WindeployQtPath {
    param([string]$QtPrefix)

    if ($QtPrefix) {
        $candidate = Join-Path $QtPrefix "bin\windeployqt.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $command = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    return ""
}

function Resolve-KobaltExecutable {
    param(
        [string]$RepositoryRoot,
        [string]$PresetName
    )

    $preset = $PresetName.ToLowerInvariant()
    if ($preset -match "msvc") {
        $candidate = Join-Path $RepositoryRoot "build\desktop-windows-msvc\Kobalt.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    } else {
        $candidate = Join-Path $RepositoryRoot "build\desktop-windows-mingw\Kobalt.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $matches = Get-ChildItem -Path (Join-Path $RepositoryRoot "build") -Filter "Kobalt.exe" -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($matches -and $matches.Count -gt 0) {
        return $matches[0].FullName
    }

    return ""
}

function Resolve-RuntimeAssetSource {
    param(
        [string]$RepositoryRoot,
        [string]$PresetName
    )

    $preset = $PresetName.ToLowerInvariant()
    if ($preset -match "msvc") {
        $candidate = Join-Path $RepositoryRoot "build\desktop-windows-msvc\desktop\assets\runtime\manifest.json"
    } else {
        $candidate = Join-Path $RepositoryRoot "build\desktop-windows-mingw\desktop\assets\runtime\manifest.json"
    }
    if (Test-Path $candidate) {
        return Split-Path -Parent $candidate
    }

    $matches = Get-ChildItem -Path (Join-Path $RepositoryRoot "build") -Filter "manifest.json" -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "[\\/]desktop[\\/]assets[\\/]runtime[\\/]manifest\.json$" } |
        Sort-Object LastWriteTime -Descending
    if ($matches -and $matches.Count -gt 0) {
        return $matches[0].DirectoryName
    }

    return ""
}

function Stage-RuntimeAssets {
    param(
        [string]$RepositoryRoot,
        [string]$PresetName,
        [string]$ExecutablePath
    )

    if (-not (Test-Path $ExecutablePath)) {
        throw "Kobalt executable was not found: $ExecutablePath"
    }

    $source = Resolve-RuntimeAssetSource -RepositoryRoot $RepositoryRoot -PresetName $PresetName
    if (-not $source) {
        throw "Runtime asset source directory was not found under build/. Run a full configure/build and try again."
    }

    $destination = Join-Path (Split-Path -Parent $ExecutablePath) "assets\runtime"
    if (Test-Path $destination) {
        Remove-Item -Recurse -Force $destination
    }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -Path (Join-Path $source "*") -Destination $destination -Recurse -Force
    Write-Host "Staged runtime assets: $destination"
}

function Resolve-PatchExecutable {
    $command = Get-Command patch -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "C:\Program Files\Git\usr\bin\patch.exe",
        "C:\Program Files\Git\mingw64\bin\patch.exe",
        "C:\msys64\usr\bin\patch.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return ""
}

function Resolve-NinjaPath {
    $command = Get-Command ninja -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "C:\Users\russe\AppData\Local\Microsoft\WinGet\Links\ninja.exe",
        "C:\Program Files\Ninja\ninja.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return ""
}

function Resolve-MakePath {
    $command = Get-Command make -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "D:\dev\Qt\Tools\mingw1310_64\bin\mingw32-make.exe",
        "C:\msys64\usr\bin\make.exe",
        "C:\Program Files\Git\usr\bin\make.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return ""
}

function Resolve-PkgConfigPath {
    $command = Get-Command pkg-config -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "C:\msys64\mingw64\bin\pkg-config.exe",
        "C:\msys64\usr\bin\pkg-config.exe",
        "C:\Program Files\Git\usr\bin\pkg-config.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return ""
}

$useMsvc = $Toolchain -eq "msvc" -or $ConfigurePreset -match "msvc" -or $BuildPreset -match "msvc"
$cmakeExe = Resolve-CMakePath
$vcvars = ""
if ($useMsvc) {
    $vcvars = Resolve-VcVarsPath
}
$windowsPlatformDir = $PSScriptRoot
$repositoryRoot = (Resolve-Path (Join-Path $windowsPlatformDir "..\..")).Path
$resolvedQtPrefixPath = Resolve-QtPrefixPath -RequestedPath $QtPrefixPath
$windeployqtExe = Resolve-WindeployQtPath -QtPrefix $resolvedQtPrefixPath
$patchExe = Resolve-PatchExecutable
$ninjaExe = Resolve-NinjaPath
$makeExe = Resolve-MakePath
$pkgConfigExe = Resolve-PkgConfigPath
$patchDir = ""
if ($patchExe) {
    $patchDir = Split-Path -Parent $patchExe
}
$ninjaDir = ""
if ($ninjaExe) {
    $ninjaDir = Split-Path -Parent $ninjaExe
}
$mingwToolDir = ""
if (-not $useMsvc) {
    $mingwToolDir = "D:\dev\Qt\Tools\mingw1310_64\bin"
}
$qtBinDir = ""
if ($resolvedQtPrefixPath) {
    $qtBinDir = Join-Path $resolvedQtPrefixPath "bin"
}
$configureArgs = @("--preset", $ConfigurePreset)
if ($resolvedQtPrefixPath) {
    $configureArgs += "-DCMAKE_PREFIX_PATH=$resolvedQtPrefixPath"
}
if ($ninjaExe) {
    $configureArgs += "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
}
if ($makeExe) {
    $configureArgs += "-DMAKE_EXECUTABLE=$makeExe"
}
if ($pkgConfigExe) {
    $configureArgs += "-DKOBALT_HOST_PKG_CONFIG=$pkgConfigExe"
}
if (-not $useMsvc) {
    $msysRoot = "C:\msys64\mingw64"
    $pngLibrary = Join-Path $msysRoot "lib\libpng16.a"
    $pngInclude = Join-Path $msysRoot "include"
    $zlibLibrary = Join-Path $msysRoot "lib\libz.a"
    $zlibInclude = Join-Path $msysRoot "include"
    if ((Test-Path $pngLibrary) -and (Test-Path (Join-Path $pngInclude "png.h"))) {
        $configureArgs += "-DPNG_LIBRARY=$pngLibrary"
        $configureArgs += "-DPNG_PNG_INCLUDE_DIR=$pngInclude"
    }
    if ((Test-Path $zlibLibrary) -and (Test-Path (Join-Path $zlibInclude "zlib.h"))) {
        $configureArgs += "-DZLIB_LIBRARY=$zlibLibrary"
        $configureArgs += "-DZLIB_INCLUDE_DIR=$zlibInclude"
    }
}
if ($Fresh) {
    $configureArgs += "--fresh"
}

if ($useMsvc) {
    Write-Host "Using vcvars64.bat: $vcvars"
} else {
    Write-Host "Using toolchain: MinGW"
}
Write-Host "Using cmake: $cmakeExe"
if ($resolvedQtPrefixPath) {
    Write-Host "Using Qt prefix path: $resolvedQtPrefixPath"
} else {
    if ($useMsvc) {
        Write-Warning "Qt 6.11+ (msvc2022_64) was not detected. Set -QtPrefixPath or KOBALT_QT_PREFIX."
    } else {
        Write-Warning "Qt 6.11+ (mingw_64) was not detected. Set -QtPrefixPath or KOBALT_QT_PREFIX."
    }
}
if ($patchExe) {
    Write-Host "Using patch: $patchExe"
} else {
    Write-Warning "patch was not found. Install Git for Windows with Unix tools in PATH, or set PATH to patch.exe."
}
if ($ninjaExe) {
    Write-Host "Using ninja: $ninjaExe"
} else {
    Write-Warning "ninja was not found. Install Ninja and ensure ninja.exe is available."
}
if ($makeExe) {
    Write-Host "Using make: $makeExe"
} else {
    Write-Warning "make was not found. Install a make tool or set MAKE_EXECUTABLE in CMake."
}
if ($pkgConfigExe) {
    Write-Host "Using pkg-config: $pkgConfigExe"
} else {
    Write-Warning "pkg-config was not found. Install pkg-config or set KOBALT_HOST_PKG_CONFIG in CMake."
}

$commandParts = @(
    ($(if ($patchDir -or $ninjaDir -or $mingwToolDir -or $qtBinDir) {
        $pathPrefix = @()
        if ($patchDir) { $pathPrefix += $patchDir }
        if ($ninjaDir) { $pathPrefix += $ninjaDir }
        if ($mingwToolDir -and (Test-Path $mingwToolDir)) { $pathPrefix += $mingwToolDir }
        if ($qtBinDir -and (Test-Path $qtBinDir)) { $pathPrefix += $qtBinDir }
        "set `"PATH=$($pathPrefix -join ';');%PATH%`""
    } else { "" })),
    ($(if ($useMsvc) { "call `"$vcvars`" >nul" } else { "" })),
    "cd /d `"$windowsPlatformDir`"",
    "`"$cmakeExe`" $($configureArgs.ForEach({ if ($_ -match '\s') { '\"' + $_ + '\"' } else { $_ } }) -join ' ')",
    "`"$cmakeExe`" --build --preset $BuildPreset"
) | Where-Object { $_ -ne "" } | ForEach-Object { $_ }
$command = $commandParts -join " && "

& $env:ComSpec /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Windows build failed with exit code $LASTEXITCODE."
}

$kobaltExe = Resolve-KobaltExecutable -RepositoryRoot $repositoryRoot -PresetName $BuildPreset
if (-not $kobaltExe) {
    throw "Kobalt.exe was not found under build/ after a successful build."
}
Stage-RuntimeAssets -RepositoryRoot $repositoryRoot -PresetName $BuildPreset -ExecutablePath $kobaltExe

if (-not $SkipQtDeploy) {
    if (-not $windeployqtExe) {
        Write-Warning "windeployqt was not found. The executable may miss Qt runtime DLLs."
    } else {
        Write-Host "Deploying Qt runtime with: $windeployqtExe"
        & $windeployqtExe --no-translations --no-system-d3d-compiler $kobaltExe
        if ($LASTEXITCODE -ne 0) {
            throw "Qt runtime deployment failed with exit code $LASTEXITCODE."
        }
    }
}
