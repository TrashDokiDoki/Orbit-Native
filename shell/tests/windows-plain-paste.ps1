param([Parameter(Mandatory)][string]$Executable)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OrbitPasteTest {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr data);
}
'@

$profile = Join-Path $env:RUNNER_TEMP ('orbit-paste-' + [guid]::NewGuid())
$env:APPDATA = $profile
$env:LOCALAPPDATA = $profile
$env:WEBVIEW2_USER_DATA_FOLDER = Join-Path $profile 'webview'
New-Item -ItemType Directory -Path $profile | Out-Null
$app = Start-Process -FilePath $Executable -PassThru
$window = [Windows.Forms.Form]::new()
$window.Text = 'Orbit paste fixture'
$editor = [Windows.Forms.RichTextBox]::new()
$editor.Dock = 'Fill'
$window.Controls.Add($editor)
$keys = @(0x11, 0x12, 0x10, 0x56)

function Wait-Editor([int]$Milliseconds) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $Milliseconds) {
        [Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 20
    }
}

try {
    $window.Show()
    Wait-Editor 4000
    if ($app.HasExited) { throw 'Orbit exited before the clipboard test' }
    [void][OrbitPasteTest]::SetForegroundWindow($window.Handle)
    [void]$editor.Focus()
    if ([OrbitPasteTest]::GetForegroundWindow() -ne $window.Handle) { throw 'Test editor is not foreground' }
    $data = [Windows.Forms.DataObject]::new()
    $data.SetText('Orbit clipboard fixture')
    $data.SetText('{\rtf1\ansi\b Orbit clipboard fixture}', [Windows.Forms.TextDataFormat]::Rtf)
    [Windows.Forms.Clipboard]::SetDataObject($data, $true)
    try {
        foreach ($key in $keys) { [OrbitPasteTest]::keybd_event($key, 0, 0, [UIntPtr]::Zero) }
        Wait-Editor 350
        if ($editor.Text) { throw 'Text arrived before releasing the shortcut' }
    } finally {
        for ($i = $keys.Count - 1; $i -ge 0; $i--) {
            [OrbitPasteTest]::keybd_event($keys[$i], 0, 2, [UIntPtr]::Zero)
        }
    }
    Wait-Editor 1200
    if ($editor.Text -ne 'Orbit clipboard fixture' -or $editor.SelectionFont.Bold) {
        throw "Expected unformatted fixture text, received '$($editor.Text)'"
    }
    $editor.SelectAll()
    if (!$editor.SelectionFont -or $editor.SelectionFont.Bold) { throw 'Pasted text retained formatting' }
    'Plain-text paste passed'
} finally {
    $window.Dispose()
    [Windows.Forms.Clipboard]::Clear()
    if (!$app.HasExited) { Stop-Process -Id $app.Id }
}
