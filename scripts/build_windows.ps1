param(
    [ValidateRange(1, 64)]
    [int]$Jobs = 8,
    [string]$CudaArchitecture = '89'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
try {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Install Visual Studio C++ build tools and the NVIDIA CUDA Toolkit.'
    }
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) { throw 'Visual Studio C++ tools were not found.' }
    $env:VSLANG = '1033'
    $env:CL = "$env:CL /utf-8"
    & (Join-Path $visualStudio 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    $env:PATH = (Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin') + ';' +
        (Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
    $build = Join-Path $repoRoot 'build-cuda'
    & cmake -S $repoRoot -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBREEZE_VULKAN=OFF -DBREEZE_CUDA=ON "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitecture" -DBREEZE_BUILD_TESTS=ON -DGGML_NATIVE=OFF
    if ($LASTEXITCODE -ne 0) { throw 'CUDA build configuration failed.' }
    & cmake --build $build --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'CUDA compilation failed.' }
    & ctest --test-dir $build -R '^(bearer-auth|request-queue)$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Authentication or queue tests failed.' }
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
