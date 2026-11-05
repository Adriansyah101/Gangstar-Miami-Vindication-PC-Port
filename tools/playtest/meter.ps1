# Logs the default output device's peak meter, and whether a session of a given process is active.
param([string]$Out, [int]$Seconds = 60)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo {}
[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
  int EnumAudioEndpoints(int flow, int mask, out IntPtr devs);
  int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev);
}
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
  int Activate([MarshalAs(UnmanagedType.LPStruct)] Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
}
[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation { int GetPeakValue(out float peak); }
public static class Meter {
  static IAudioMeterInformation m;
  public static void Open() {
    var e = (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
    IMMDevice d; e.GetDefaultAudioEndpoint(0, 0, out d);
    object o; d.Activate(typeof(IAudioMeterInformation).GUID, 23, IntPtr.Zero, out o);
    m = (IAudioMeterInformation)o;
  }
  public static float Peak() { float p; m.GetPeakValue(out p); return p; }
}
'@
[Meter]::Open()
$t0 = Get-Date
$lines = New-Object System.Collections.ArrayList
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $g = Get-Process GangstarMV -ErrorAction SilentlyContinue
    [void]$lines.Add(('{0:F1} {1:F4} {2}' -f ((Get-Date) - $t0).TotalSeconds, [Meter]::Peak(), [int][bool]$g))
    Start-Sleep -Milliseconds 100
}
$lines | Set-Content $Out
