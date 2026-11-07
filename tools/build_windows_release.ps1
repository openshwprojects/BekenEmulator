$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$manifestPath = Join-Path $PSScriptRoot "release_firmwares.txt"
$distPath = Join-Path $repoRoot "dist"

Push-Location $repoRoot
try {
    foreach ($candidate in @(
        "C:\msys64\ucrt64\bin",
        "C:\msys64\mingw64\bin",
        "C:\msys64\usr\bin",
        "W:\TOOLS\msys64\ucrt64\bin",
        "W:\TOOLS\msys64\usr\bin"
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            $env:PATH = "$candidate;$env:PATH"
        }
    }

    $firmwareNames = @(
        Get-Content -LiteralPath $manifestPath |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith("#") }
    )
    if ($firmwareNames.Count -eq 0) {
        throw "The release firmware manifest is empty: $manifestPath"
    }
    if (($firmwareNames | Sort-Object -Unique).Count -ne $firmwareNames.Count) {
        throw "The release firmware manifest contains duplicate names."
    }

    foreach ($name in $firmwareNames) {
        $source = Join-Path $repoRoot (Join-Path "firmwares" $name)
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Release firmware does not exist: $source"
        }
    }

    & make -j4 cli tools
    if ($LASTEXITCODE -ne 0) {
        throw "make failed with exit code $LASTEXITCODE"
    }

    New-Item -ItemType Directory -Force -Path $distPath | Out-Null
    $exePath = Join-Path $distPath "BekenEmulator.exe"
    $cliPath = Join-Path $distPath "bekensim.exe"
    Copy-Item -LiteralPath (Join-Path $repoRoot "sim_ui.exe") -Destination $exePath -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "bekensim.exe") -Destination $cliPath -Force

    # Append BKSFWB01 firmware bundle trailer so BekenEmulator.exe runs standalone anywhere
    $stream = [System.IO.File]::Open($exePath, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
    try {
        $writer = New-Object System.IO.BinaryWriter($stream)
        [uint32]$totalBytes = 0
        foreach ($name in $firmwareNames) {
            $source = Join-Path $repoRoot (Join-Path "firmwares" $name)
            $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($name)
            $dataBytes = [System.IO.File]::ReadAllBytes($source)
            $writer.Write([uint32]$nameBytes.Length)
            $writer.Write($nameBytes)
            $writer.Write([uint32]$dataBytes.Length)
            $writer.Write($dataBytes)
            $totalBytes += [uint32](8 + $nameBytes.Length + $dataBytes.Length)
        }
        $writer.Write([uint32]$firmwareNames.Count)
        $writer.Write([uint32]$totalBytes)
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("BKSFWB01"))
        $writer.Flush()
    }
    finally {
        $stream.Close()
    }

    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
        throw "Build did not create $exePath"
    }
    Write-Host "Built $exePath"
}
finally {
    Pop-Location
}
