# プロセスを一時停止して、指定の時間後に再開する (送出側の短い途切れを作る試験用)
#   powershell -File tools/testsrc/suspend.ps1 -ProcessId <pid> -Milliseconds <ms>
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][int]$Milliseconds
)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace Denpachan -Name Ntdll -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr handle);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr handle);
'@
$p = Get-Process -Id $ProcessId
[void][Denpachan.Ntdll]::NtSuspendProcess($p.Handle)
Start-Sleep -Milliseconds $Milliseconds
[void][Denpachan.Ntdll]::NtResumeProcess($p.Handle)
