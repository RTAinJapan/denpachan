# 複数のプロセスを同時に一時停止して、指定の時間後に再開する (PC 全体が止まったのを模す)
#   pwsh -File tools/testsrc/suspend_many.ps1 -ProcessIds 1,2,3 -Milliseconds 600
param(
    [Parameter(Mandatory)][string]$ProcessIds,  # コンマ区切り (-File では配列を渡せない)
    [Parameter(Mandatory)][int]$Milliseconds
)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace Denpachan -Name NtdllMany -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr handle);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr handle);
'@
$ps = $ProcessIds.Split(',') | ForEach-Object { Get-Process -Id ([int]$_) }
foreach ($p in $ps) { [void][Denpachan.NtdllMany]::NtSuspendProcess($p.Handle) }
Start-Sleep -Milliseconds $Milliseconds
foreach ($p in $ps) { [void][Denpachan.NtdllMany]::NtResumeProcess($p.Handle) }
