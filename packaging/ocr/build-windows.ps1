param(
    [Parameter(Mandatory)][ValidateSet('x64','arm64')][string]$Architecture,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
$triplet = "$Architecture-windows-static"
$outputPath = [System.IO.Path]::GetFullPath($Output)
$vcpkg = Join-Path $env:RUNNER_TEMP 'orbit-vcpkg'
$installed = Join-Path $env:RUNNER_TEMP 'orbit-ocr-ports'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:RUNNER_TEMP 'orbit-vcpkg-cache'
New-Item -ItemType Directory -Path $env:VCPKG_DEFAULT_BINARY_CACHE -Force | Out-Null

git clone --depth 1 --branch 2025.08.27 https://github.com/microsoft/vcpkg.git $vcpkg
if ($LASTEXITCODE -ne 0) { throw 'Could not get the pinned vcpkg version' }
& (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw 'Could not build the vcpkg command' }
& (Join-Path $vcpkg 'vcpkg.exe') install --triplet $triplet --host-triplet $triplet "--x-manifest-root=$PSScriptRoot" "--x-install-root=$installed"
if ($LASTEXITCODE -ne 0) { throw 'Could not build Tesseract' }

$port = Join-Path $installed $triplet
$models = Join-Path $outputPath 'tessdata'
$licenses = Join-Path $outputPath 'licenses'
New-Item -ItemType Directory -Path $models, $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $port 'tools/tesseract/tesseract.exe') -Destination $outputPath
Get-ChildItem -Path (Join-Path $port 'share/*/copyright') -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenses ($_.Directory.Name + '.txt'))
}

$files = @{
    'eng.traineddata' = '7D4322BD2A7749724879683FC3912CB542F19906C83BCC1A52132556427170B2'
    'rus.traineddata' = 'E16E5E036CCE1D9EC2B00063CF8B54472625B9E14D893A169E2B0DEDEB4DF225'
    'LICENSE' = 'CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30'
}
foreach ($name in $files.Keys) {
    $path = if ($name -eq 'LICENSE') { Join-Path $licenses 'tessdata-fast.txt' } else { Join-Path $models $name }
    curl.exe --fail --location --silent --show-error --output $path "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/4.1.0/$name"
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $files[$name]) {
        throw "Could not verify $name"
    }
}

& (Join-Path $outputPath 'tesseract.exe') --tessdata-dir $models --list-langs
if ($LASTEXITCODE -ne 0) { throw 'The bundled OCR executable could not start' }
