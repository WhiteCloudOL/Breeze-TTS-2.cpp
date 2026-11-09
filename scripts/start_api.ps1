param(
    [ValidateSet('original', 'auth')]
    [string]$Mode = 'auth',
    [string]$BindAddress = '127.0.0.1',
    [ValidateRange(0, 65535)]
    [int]$Port = 0,
    [ValidateRange(-1, 65535)]
    [int]$WsPort = 0,
    [string]$ModelPath = '',
    [string]$KeyFile = '',
    [ValidateRange(1, 32)]
    [int]$Parallel = 1,
    [switch]$NoWebUI,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
try {
    if ($Help) {
        Write-Host 'Options: -BindAddress ADDRESS -Port PORT -WsPort PORT -ModelPath FILE -KeyFile FILE -Parallel N -NoWebUI'
        Write-Host 'Default HTTP ports: auth 7863, original 7865. WebSocket uses HTTP port + 1.'
        Write-Host 'Default generation slots: 1. Additional requests wait in FIFO order.'
        Write-Host 'Auth keys: BREEZE_API_KEYS, BREEZE_API_KEY, BREEZE_API_KEY_FILE, then .env.api-key (one per line).'
        exit 0
    }
    Set-Location -LiteralPath $repoRoot
    $server = Join-Path $repoRoot 'build-cuda\breeze-server.exe'
    if (-not (Test-Path -LiteralPath $server -PathType Leaf)) {
        throw 'Run powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 first.'
    }
    if (-not $ModelPath) { $ModelPath = Join-Path $repoRoot 'models\breeze-tts-2-q8_0.gguf' }
    if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) {
        throw 'Run powershell -ExecutionPolicy Bypass -File scripts\download_q8.ps1 first.'
    }
    if ($Port -eq 0) { $Port = if ($Mode -eq 'auth') { 7863 } else { 7865 } }
    if ($WsPort -eq 0 -and $Port -eq 65535) { throw 'Specify -WsPort -1 or a separate WebSocket port.' }
    if ($WsPort -eq $Port) { throw 'HTTP and WebSocket need different ports.' }
    if ($env:CUDA_PATH) {
        $env:PATH = (Join-Path $env:CUDA_PATH 'bin') + ';' +
            (Join-Path $env:CUDA_PATH 'bin\x64') + ';' + $env:PATH
    }
    $runtimeArguments = @($ModelPath, '--host', $BindAddress, '--port', $Port.ToString(),
        '--ws-port', $WsPort.ToString(), '--parallel', $Parallel.ToString(), '--require-cuda')
    if (-not $NoWebUI) { $runtimeArguments += '--webui' }
    if ($Mode -eq 'auth') {
        if (-not $KeyFile) { $KeyFile = Join-Path $repoRoot '.env.api-key' }
        if (-not $env:BREEZE_API_KEYS -and -not $env:BREEZE_API_KEY -and -not $env:BREEZE_API_KEY_FILE) {
            if (-not (Test-Path -LiteralPath $KeyFile)) {
                $bytes = New-Object byte[] 32
                $random = [Security.Cryptography.RandomNumberGenerator]::Create()
                try { $random.GetBytes($bytes) } finally { $random.Dispose() }
                $key = [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
                $writer = [IO.StreamWriter]::new([IO.File]::Open($KeyFile, [IO.FileMode]::CreateNew), [Text.Encoding]::ASCII)
                try { $writer.Write($key) } finally { $writer.Dispose() }
                Write-Host "Created API key file: $KeyFile"
            }
        }
        $runtimeArguments += @('--auth', '--api-key-file', $KeyFile)
    }
    Write-Host "Starting $Mode Q8_0 API with native CUDA acceleration."
    Write-Host "Generation slots: $Parallel. Additional requests wait in FIFO order."
    Write-Host "WebUI: http://127.0.0.1:$Port/"
    Write-Host "API docs: http://127.0.0.1:$Port/docs"
    Write-Host 'Wait for the model warmup and listening message. Press Ctrl+C to stop.'
    & $server @runtimeArguments
    exit $LASTEXITCODE
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
