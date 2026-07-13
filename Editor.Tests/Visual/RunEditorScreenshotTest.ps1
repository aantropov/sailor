#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $EditorExecutable,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputDirectory,

    [ValidateRange(1, 900)]
    [int] $TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
$screenshotPath = Join-Path $outputPath 'editor-screenshot.png'
$resultPath = [System.IO.Path]::ChangeExtension($screenshotPath, '.result.json')
$stdoutPath = Join-Path $outputPath 'editor-screenshot.stdout.log'
$stderrPath = Join-Path $outputPath 'editor-screenshot.stderr.log'
$runnerLogPath = Join-Path $outputPath 'editor-screenshot.runner.log'
$stateDirectory = Join-Path $outputPath '.screenshot-state'

[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

foreach ($path in @($screenshotPath, $resultPath, $stdoutPath, $stderrPath, $runnerLogPath)) {
    if ([System.IO.File]::Exists($path)) {
        [System.IO.File]::Delete($path)
    }
}

[System.IO.File]::WriteAllText($stdoutPath, [string]::Empty, $utf8WithoutBom)
[System.IO.File]::WriteAllText($stderrPath, [string]::Empty, $utf8WithoutBom)

function Write-RunnerLog {
    param(
        [Parameter(Mandatory)]
        [string] $Message
    )

    $line = '{0:O} {1}' -f [System.DateTimeOffset]::UtcNow, $Message
    [System.IO.File]::AppendAllText($runnerLogPath, "$line$([Environment]::NewLine)", $utf8WithoutBom)
    Write-Host $line
}

function Get-RequiredJsonProperty {
    param(
        [Parameter(Mandatory)]
        [psobject] $JsonObject,

        [Parameter(Mandatory)]
        [string] $Name
    )

    $property = $JsonObject.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Screenshot result is missing required property '$Name'."
    }

    return $property.Value
}

function ConvertTo-ResultInt64 {
    param(
        [AllowNull()]
        [object] $Value,

        [Parameter(Mandatory)]
        [string] $Name
    )

    if ($null -eq $Value) {
        throw "Screenshot result property '$Name' must not be null."
    }

    try {
        return [System.Convert]::ToInt64($Value, [System.Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "Screenshot result property '$Name' is not an integer: $Value"
    }
}

function Read-PngEvidence {
    param(
        [Parameter(Mandatory)]
        [string] $Path
    )

    Add-Type -AssemblyName PresentationCore -ErrorAction Stop

    $stream = $null
    try {
        $stream = [System.IO.File]::Open(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        $decoder = [System.Windows.Media.Imaging.PngBitmapDecoder]::new(
            $stream,
            [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
            [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)

        if ($decoder.Frames.Count -ne 1) {
            throw "Expected one PNG frame, found $($decoder.Frames.Count)."
        }

        $frame = $decoder.Frames[0]
        $width = $frame.PixelWidth
        $height = $frame.PixelHeight
        if ($width -lt 800 -or $height -lt 600 -or $width -gt 8192 -or $height -gt 8192) {
            throw "PNG dimensions are outside the expected range: ${width}x${height}."
        }

        $bitmap = [System.Windows.Media.Imaging.FormatConvertedBitmap]::new(
            $frame,
            [System.Windows.Media.PixelFormats]::Bgra32,
            $null,
            0)
        $stride = $width * 4
        $pixels = [byte[]]::new($stride * $height)
        $bitmap.CopyPixels($pixels, $stride, 0)

        $sampleColumns = 17
        $sampleRows = 13
        $sampledColors = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)

        for ($row = 0; $row -lt $sampleRows; $row++) {
            $y = [System.Convert]::ToInt32(
                [System.Math]::Round((($row + 1) * ($height - 1)) / ($sampleRows + 1)))
            for ($column = 0; $column -lt $sampleColumns; $column++) {
                $x = [System.Convert]::ToInt32(
                    [System.Math]::Round((($column + 1) * ($width - 1)) / ($sampleColumns + 1)))
                $offset = ($y * $stride) + ($x * 4)
                $color = '{0:X2}{1:X2}{2:X2}' -f `
                    $pixels[$offset + 2], `
                    $pixels[$offset + 1], `
                    $pixels[$offset]
                [void] $sampledColors.Add($color)
            }
        }

        if ($sampledColors.Count -lt 2) {
            throw 'PNG is visually uniform across the sampled pixel grid.'
        }

        return [pscustomobject]@{
            Width = $width
            Height = $height
            SampledColorCount = $sampledColors.Count
        }
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

$process = $null
$processStarted = $false
$stdoutTask = $null
$stderrTask = $null
$failure = $null
$processCompleted = $false

try {
    $editorPath = [System.IO.Path]::GetFullPath($EditorExecutable)
    Write-RunnerLog "Editor executable: $editorPath"
    Write-RunnerLog "Screenshot output: $screenshotPath"
    Write-RunnerLog "Result output: $resultPath"
    Write-RunnerLog "Timeout: $TimeoutSeconds seconds"

    if ([System.IO.Directory]::Exists($stateDirectory)) {
        Write-RunnerLog "Removing previous isolated screenshot state: $stateDirectory"
        [System.IO.Directory]::Delete($stateDirectory, $true)
    }

    if (-not [System.IO.File]::Exists($editorPath)) {
        throw "Editor executable does not exist: $editorPath"
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $editorPath
    $startInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($editorPath)
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $false
    $startInfo.ArgumentList.Add("--editor-screenshot=$screenshotPath")

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start Editor executable: $editorPath"
    }

    $processStarted = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    Write-RunnerLog "Started Editor process $($process.Id)."

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Write-RunnerLog "Editor process exceeded the $TimeoutSeconds-second timeout."
        try {
            $process.Kill($true)
            $processCompleted = $process.WaitForExit(10000)
            if (-not $processCompleted) {
                Write-RunnerLog 'Timed-out Editor process did not exit within the cleanup window.'
            }
        }
        catch {
            Write-RunnerLog "Failed to terminate timed-out process tree: $($_.Exception.Message)"
        }

        throw "Editor screenshot test timed out after $TimeoutSeconds seconds."
    }

    $process.WaitForExit()
    $processCompleted = $true
    Write-RunnerLog "Editor process exited with code $($process.ExitCode)."

    if (-not [System.IO.File]::Exists($resultPath)) {
        throw "Editor exited with code $($process.ExitCode) without creating the screenshot result: $resultPath"
    }

    $result = [System.IO.File]::ReadAllText($resultPath, $utf8WithoutBom) | ConvertFrom-Json
    $success = Get-RequiredJsonProperty -JsonObject $result -Name 'success'
    if ($success -isnot [bool]) {
        throw "Screenshot result property 'success' must be a Boolean."
    }

    $reportedError = Get-RequiredJsonProperty -JsonObject $result -Name 'error'
    if (-not $success) {
        throw "Editor reported screenshot failure (process exit code $($process.ExitCode)): $reportedError"
    }

    if ($null -ne $reportedError) {
        throw "Successful screenshot result unexpectedly contains an error: $reportedError"
    }

    if ($process.ExitCode -ne 0) {
        throw "Editor reported screenshot success but exited with code $($process.ExitCode)."
    }

    $reportedPngPath = Get-RequiredJsonProperty -JsonObject $result -Name 'pngPath'
    if ($reportedPngPath -isnot [string] -or
        -not [System.IO.Path]::IsPathFullyQualified($reportedPngPath)) {
        throw "Screenshot result property 'pngPath' must be an absolute path."
    }

    $resolvedReportedPngPath = [System.IO.Path]::GetFullPath($reportedPngPath)
    if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals(
        $resolvedReportedPngPath,
        $screenshotPath)) {
        throw "Screenshot result points to '$resolvedReportedPngPath' instead of '$screenshotPath'."
    }

    if (-not [System.IO.File]::Exists($screenshotPath)) {
        throw "Editor reported success without creating the PNG: $screenshotPath"
    }

    $readyAtText = Get-RequiredJsonProperty -JsonObject $result -Name 'readyAtUtc'
    $capturedAtText = Get-RequiredJsonProperty -JsonObject $result -Name 'capturedAtUtc'
    try {
        $readyAt = [System.DateTimeOffset]::Parse(
            $readyAtText,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::AssumeUniversal)
        $capturedAt = [System.DateTimeOffset]::Parse(
            $capturedAtText,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::AssumeUniversal)
    }
    catch {
        throw 'Screenshot result timestamps must be valid ISO-8601 values.'
    }

    $delayMilliseconds = ConvertTo-ResultInt64 `
        -Value (Get-RequiredJsonProperty -JsonObject $result -Name 'delayMilliseconds') `
        -Name 'delayMilliseconds'
    if ($delayMilliseconds -lt 5000) {
        throw "Reported screenshot delay is less than five seconds: $delayMilliseconds ms."
    }

    $actualDelayMilliseconds = ($capturedAt - $readyAt).TotalMilliseconds
    if ($actualDelayMilliseconds -lt 5000) {
        throw "Screenshot was captured less than five seconds after readiness: $actualDelayMilliseconds ms."
    }

    $reportedWidth = ConvertTo-ResultInt64 `
        -Value (Get-RequiredJsonProperty -JsonObject $result -Name 'width') `
        -Name 'width'
    $reportedHeight = ConvertTo-ResultInt64 `
        -Value (Get-RequiredJsonProperty -JsonObject $result -Name 'height') `
        -Name 'height'
    $reportedByteCount = ConvertTo-ResultInt64 `
        -Value (Get-RequiredJsonProperty -JsonObject $result -Name 'byteCount') `
        -Name 'byteCount'
    $pngByteCount = ([System.IO.FileInfo]::new($screenshotPath)).Length

    if ($reportedByteCount -ne $pngByteCount) {
        throw "Reported PNG byte count ($reportedByteCount) differs from the file length ($pngByteCount)."
    }

    if ($pngByteCount -le 4096) {
        throw "PNG is unexpectedly small: $pngByteCount bytes."
    }

    $pngEvidence = Read-PngEvidence -Path $screenshotPath
    if ($reportedWidth -ne $pngEvidence.Width -or $reportedHeight -ne $pngEvidence.Height) {
        throw "Reported PNG dimensions (${reportedWidth}x${reportedHeight}) differ from the decoded dimensions ($($pngEvidence.Width)x$($pngEvidence.Height))."
    }

    Write-RunnerLog (
        'Validated PNG: {0}x{1}, {2} bytes, {3} distinct sampled colors, {4:N0} ms after readiness.' -f
        $pngEvidence.Width,
        $pngEvidence.Height,
        $pngByteCount,
        $pngEvidence.SampledColorCount,
        $actualDelayMilliseconds)
}
catch {
    $failure = $_
    Write-RunnerLog "FAILED: $($_.Exception.Message)"
}
finally {
    if ($processStarted -and $null -ne $process) {
        try {
            if (-not $process.HasExited) {
                Write-RunnerLog "Terminating remaining Editor process tree $($process.Id)."
                $process.Kill($true)
            }

            $processCompleted = $process.WaitForExit(10000)
            if (-not $processCompleted) {
                Write-RunnerLog 'Editor process did not exit within the final cleanup window.'
            }
        }
        catch {
            Write-RunnerLog "Process cleanup warning: $($_.Exception.Message)"
        }
    }

    if ($null -ne $stdoutTask -and $processCompleted) {
        try {
            [System.IO.File]::WriteAllText(
                $stdoutPath,
                $stdoutTask.GetAwaiter().GetResult(),
                $utf8WithoutBom)
        }
        catch {
            Write-RunnerLog "Could not persist Editor stdout: $($_.Exception.Message)"
        }
    }

    if ($null -ne $stderrTask -and $processCompleted) {
        try {
            [System.IO.File]::WriteAllText(
                $stderrPath,
                $stderrTask.GetAwaiter().GetResult(),
                $utf8WithoutBom)
        }
        catch {
            Write-RunnerLog "Could not persist Editor stderr: $($_.Exception.Message)"
        }
    }

    if (-not $processCompleted -and $processStarted) {
        [System.IO.File]::AppendAllText(
            $stderrPath,
            "Editor process output could not be drained because the process did not exit.$([Environment]::NewLine)",
            $utf8WithoutBom)
    }

    if ($null -ne $process) {
        $process.Dispose()
    }
}

if ($null -ne $failure) {
    throw $failure
}

Write-RunnerLog 'Editor screenshot smoke test passed.'
