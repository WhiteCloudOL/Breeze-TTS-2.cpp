param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$models = Join-Path $repoRoot 'models'
$model = Join-Path $models 'breeze-tts-2-q8_0.gguf'
$expectedHash = 'a02bcc4b69b0601032727f8040c4942149b1b73aa0f69022fe5aaa6a8f0ef879'
$revision = 'e4fae6493ffb8bbeacc402d1b36f18dfa18fc8d5'
try {
    [IO.Directory]::CreateDirectory($models) | Out-Null
    if (-not (Test-Path -LiteralPath $model)) {
        $partial = "$model.part"
        Write-Host 'Downloading Q8_0 model. The download is approximately 3.6 GB.'
        & curl.exe --fail --location --retry 3 --continue-at - --output $partial "https://huggingface.co/HoppouAI/Breeze-TTS-2.cpp/resolve/$revision/breeze-tts-2-q8_0.gguf"
        if ($LASTEXITCODE -ne 0) { throw 'Model download failed. Run this script again to resume.' }
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $expectedHash) {
            throw "The downloaded model failed SHA-256 verification: $partial"
        }
        Move-Item -LiteralPath $partial -Destination $model
    }
    if ((Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "The model failed SHA-256 verification: $model"
    }
    Write-Host 'Q8_0 model SHA-256 verified.'
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
