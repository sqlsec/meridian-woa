#Requires -Version 5.1
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 meridian-woa contributors
<#
.SYNOPSIS
Install and manage meridian-woa on a Windows on Arm dual-boot laptop.

.DESCRIPTION
Run from an elevated PowerShell. Nothing here touches the Windows or Limine
loaders; only \EFI\Meridian on the Windows ESP, one Boot#### entry,
BootNext/BootOrder and (Tidy) limine.conf are changed.

  Install  Copy Meridian.efi + config.conf to \EFI\Meridian on the Windows ESP,
           create/refresh the "Meridian Obsidian" entry and set BootNext, so only
           the NEXT boot goes through Meridian. BootOrder is left alone.
  Promote  Put Meridian first in BootOrder (after both systems booted through it).
  Demote   Restore the BootOrder saved by Promote.
  Status   Read-only: detected ESPs, entries, BootOrder/BootNext/BootCurrent.
  Remove   Delete the entry and BootNext, restore BootOrder, remove \EFI\Meridian.
  Tidy     On the Linux ESP: make Limine quiet (timeout 1) and drop any Windows
           entry from limine.conf, and copy meridian-guard.sh to it (run that once
           inside Linux: sudo bash /boot/meridian-guard.sh).

ESPs and the Windows Boot Manager / Limine Boot#### numbers are detected
automatically; override with -WindowsEsp, -LinuxEsp, -WindowsBootNum, -LinuxBootNum.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Install
#>
[CmdletBinding()]
param(
    [ValidateSet('Install','Promote','Demote','Status','Remove','Tidy')][string]$Action = 'Status',
    [string]$Efi = '',
    [Guid]$WindowsEsp = [Guid]::Empty,
    [Guid]$LinuxEsp = [Guid]::Empty,
    [string]$WindowsBootNum = '',
    [string]$LinuxBootNum = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Efi) { $Efi = Join-Path $Root 'Meridian.efi' }
$ConfigTemplate = Join-Path $Root 'config.conf'
$GuardScript = Join-Path $Root 'meridian-guard.sh'
$StatePath = Join-Path $Root 'state.json'          # machine-specific, gitignored
$BackupRoot = Join-Path $Root 'backups'            # machine-specific, gitignored
$EspType = [Guid]'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
$LoaderPath = '\EFI\Meridian\Meridian.efi'
$Description = 'Meridian Obsidian'
$Mounted = [Collections.Generic.List[string]]::new()

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }
if (-not ('MeridianBoot.Native' -as [type])) { Add-Type -Path (Join-Path $Root 'UefiNative.cs') }

function Read-Var([string]$Name) { return [MeridianBoot.Native]::Read($Name) }
function Fmt-Order($v) { if ($null -eq $v) { return 'none' }; return (([MeridianBoot.Native]::DecodeOrder($v.Data)) | ForEach-Object { '{0:X4}' -f $_ }) -join ',' }
function Save-State($s) { [IO.File]::WriteAllText($StatePath, ($s | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false)) }

function Get-EntryDescription($v) {
    # EFI_LOAD_OPTION: UINT32 Attributes, UINT16 FilePathListLength, CHAR16 Description[]
    if ($null -eq $v -or $v.Data.Length -lt 8) { return $null }
    $s = [Text.Encoding]::Unicode.GetString($v.Data, 6, $v.Data.Length - 6)
    $end = $s.IndexOf([char]0)
    if ($end -lt 0) { return $s }
    return $s.Substring(0, $end)
}

function Get-BootEntries {
    $order = Read-Var 'BootOrder'
    $nums = [Collections.Generic.List[int]]::new()
    if ($null -ne $order) { foreach ($n in [MeridianBoot.Native]::DecodeOrder($order.Data)) { $nums.Add([int]$n) } }
    # Low numbers hold the vendor/OS entries; 0x9000+ is where Install puts Meridian.
    foreach ($n in @(0..0x2F) + @(0x9000..0x900F)) { if (-not $nums.Contains($n)) { $nums.Add($n) } }
    $list = @()
    foreach ($n in $nums) {
        $v = Read-Var ('Boot{0:X4}' -f $n)
        if ($null -ne $v) { $list += [pscustomobject]@{ Number = $n; Name = ('{0:X4}' -f $n); Description = (Get-EntryDescription $v) } }
    }
    return $list
}

function Find-BootNum([string]$Override, [string]$Pattern, [string]$What) {
    if ($Override) {
        if ($Override -notmatch '^[0-9A-Fa-f]{4}$') { throw "$What Boot#### must be 4 hex digits, got '$Override'." }
        return $Override.ToUpperInvariant()
    }
    $hits = @(Get-BootEntries | Where-Object { $_.Description -match $Pattern })
    if ($hits.Count -eq 0) { throw "No firmware entry for $What found; pass it explicitly (see -Action Status)." }
    return $hits[0].Name
}

function Mount-Partition($Partition) {
    $vol = @($Partition.AccessPaths | Where-Object { $_ -match '^\\\\\?\\Volume\{' } | Select-Object -First 1)
    $used = @((Get-PSDrive -PSProvider FileSystem).Name)
    $letter = @('R','S','T','U','V','W','X','Y','Z' | Where-Object { $_ -notin $used -and -not (Test-Path ($_ + ':\')) } | Select-Object -First 1)
    if ($vol.Count -ne 1 -or $letter.Count -ne 1) { throw "Cannot mount ESP $($Partition.Guid)." }
    & mountvol.exe ($letter[0] + ':') $vol[0]
    if ($LASTEXITCODE -ne 0) { throw "mountvol failed: $LASTEXITCODE" }
    $Mounted.Add($letter[0] + ':')
    return $letter[0] + ':\'
}

$script:EspCache = @{}
function Get-Esp([Guid]$Override, [string]$Marker, [string]$What) {
    # Returns @{ Root; Partition } for the ESP holding $Marker (or the given GUID).
    $key = $What
    if ($script:EspCache.ContainsKey($key)) { return $script:EspCache[$key] }
    $esps = @(Get-Partition | Where-Object { $_.GptType -and [Guid]$_.GptType -eq $EspType -and $_.Guid })
    if ($Override -ne [Guid]::Empty) { $esps = @($esps | Where-Object { [Guid]$_.Guid -eq $Override }) }
    foreach ($p in $esps) {
        $root = Mount-Partition $p
        if (Test-Path -LiteralPath (Join-Path $root $Marker)) {
            $script:EspCache[$key] = @{ Root = $root; Partition = $p }
            return $script:EspCache[$key]
        }
    }
    throw "No ESP containing $Marker found for $What; pass its partition GUID explicitly."
}
function Get-WindowsEsp { return Get-Esp $WindowsEsp 'EFI\Microsoft\Boot\bootmgfw.efi' 'Windows' }
function Get-LinuxEsp { return Get-Esp $LinuxEsp 'limine.conf' 'Linux (Limine)' }

function Render-Config([string]$WinNum, [string]$LinNum) {
    # Point the two menuentry stanzas at the detected firmware entries.
    $text = [IO.File]::ReadAllText($ConfigTemplate)
    $text = [regex]::Replace($text, '(?s)(menuentry\s+"Windows[^"]*"\s*\{.*?firmware_bootnum\s+)[0-9A-Fa-f]{4}', "`${1}$WinNum")
    $text = [regex]::Replace($text, '(?s)(menuentry\s+"Omarchy[^"]*"\s*\{.*?firmware_bootnum\s+)[0-9A-Fa-f]{4}', "`${1}$LinNum")
    return $text
}

function Invoke-Tidy {
    $lin = Get-LinuxEsp
    $conf = Join-Path $lin.Root 'limine.conf'
    $text = [IO.File]::ReadAllText($conf)
    New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
    $backup = Join-Path $BackupRoot ('limine.conf.' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
    Copy-Item -LiteralPath $conf -Destination $backup
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    # Drop top-level Windows entries (a "/Name" line whose stanza chain-loads bootmgfw.efi).
    $new = [regex]::Replace($text, '(?m)(\r?\n)*^/[^/\r\n][^\r\n]*\r?\n(?:[ \t]+[^\r\n]*(?:\r?\n|$))*', {
        param($m) if ($m.Value -match '(?i)bootmgfw\.efi') { $nl } else { $m.Value } })
    $new = [regex]::Replace($new, '(?m)^timeout:[^\r\n]*', 'timeout: 1')
    if ($new -match '(?m)^quiet:') { $new = [regex]::Replace($new, '(?m)^quiet:[^\r\n]*', 'quiet: yes') }
    else { $new = "quiet: yes$nl" + $new }
    $new = $new.TrimEnd("`r", "`n") + $nl
    [IO.File]::WriteAllText($conf, $new, [Text.UTF8Encoding]::new($false))
    Write-Host "limine.conf: quiet, timeout 1, no Windows entry (backup: $backup)" -ForegroundColor Green

    $guard = ([IO.File]::ReadAllText($GuardScript)) -replace "`r`n", "`n"
    [IO.File]::WriteAllText((Join-Path $lin.Root 'meridian-guard.sh'), $guard, [Text.UTF8Encoding]::new($false))
    Write-Host 'Copied meridian-guard.sh to the Linux ESP. Inside Linux run once: sudo bash /boot/meridian-guard.sh' -ForegroundColor Green
}

try {
    $state = if (Test-Path -LiteralPath $StatePath) { Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json } else { $null }
    if ($null -eq $state) {
        # Adopt an existing entry with our description instead of creating a second one.
        $own = @(Get-BootEntries | Where-Object { $_.Description -eq $Description } | Select-Object -First 1)
        if ($own.Count -eq 1) { $state = [pscustomobject]@{ BootNumber = $own[0].Number; BootName = 'Boot' + $own[0].Name; OriginalBootOrder = $null; Promoted = $false } }
    }

    if ($Action -eq 'Tidy') { Invoke-Tidy }
    elseif ($Action -eq 'Install') {
        if (Confirm-SecureBootUEFI) { throw 'Secure Boot is on; this build is unsigned and would not load.' }
        if (-not (Test-Path -LiteralPath $Efi)) { throw "Meridian.efi not found at $Efi (build it or download a release, or pass -Efi)." }
        [MeridianBoot.Native]::ValidateArm64Efi($Efi)
        $winNum = Find-BootNum $WindowsBootNum '^Windows Boot Manager' 'Windows Boot Manager'
        $linNum = Find-BootNum $LinuxBootNum '(?i)limine|omarchy' 'Limine'
        $config = Render-Config $winNum $linNum
        Write-Host "Windows entry: Boot$winNum   Linux entry: Boot$linNum"

        $win = Get-WindowsEsp
        $dest = Join-Path $win.Root 'EFI\Meridian'
        $next = Read-Var 'BootNext'
        $ownNext = $null -ne $state -and $null -ne $next -and [BitConverter]::ToUInt16($next.Data,0) -eq [uint16]$state.BootNumber
        if ($null -ne $next -and -not $ownNext) { throw ('BootNext=' + ('{0:X4}' -f [BitConverter]::ToUInt16($next.Data,0)) + ' was set by something else; not overwriting it.') }
        $vol = Get-Volume -FilePath $win.Root -ErrorAction SilentlyContinue
        if ($null -ne $vol -and $vol.SizeRemaining -lt ((Get-Item -LiteralPath $Efi).Length * 2 + 4MB)) { throw 'Not enough free space on the Windows ESP.' }

        if (Test-Path -LiteralPath $dest) {
            New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
            $keep = Join-Path $BackupRoot ('EFI-Meridian.' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
            Copy-Item -LiteralPath $dest -Destination $keep -Recurse
            Remove-Item -LiteralPath $dest -Recurse -Force
            Write-Host "Previous \EFI\Meridian backed up to $keep"
        }
        New-Item -ItemType Directory -Path $dest | Out-Null
        Copy-Item -LiteralPath $Efi -Destination (Join-Path $dest 'Meridian.efi')
        if ((Get-FileHash $Efi).Hash -ne (Get-FileHash (Join-Path $dest 'Meridian.efi')).Hash) { throw 'Copy verification failed: Meridian.efi' }
        [IO.File]::WriteAllText((Join-Path $dest 'config.conf'), $config, [Text.UTF8Encoding]::new($false))

        $order = Read-Var 'BootOrder'
        if ($null -eq $state) {
            $num = $null
            foreach ($c in 0x9000..0x9fff) { if ($null -eq (Read-Var ('Boot{0:X4}' -f $c))) { $num = $c; break } }
            $state = [pscustomobject]@{ BootNumber = [int]$num; BootName = ('Boot{0:X4}' -f $num); OriginalBootOrder = $null; Promoted = $false }
        }
        $p = $win.Partition
        $sector = [uint64](Get-Disk -Number $p.DiskNumber).LogicalSectorSize
        $bytes = [MeridianBoot.Native]::BuildLoadOption([uint32]$p.PartitionNumber, [uint64]($p.Offset/$sector), [uint64]($p.Size/$sector), [Guid]$p.Guid, $LoaderPath, $Description)
        [MeridianBoot.Native]::Write($state.BootName, $bytes, 7)
        [MeridianBoot.Native]::Write('BootNext', [BitConverter]::GetBytes([uint16]$state.BootNumber), 7)
        $after = Read-Var 'BootOrder'
        if ($null -ne $order -and -not [MeridianBoot.Native]::Equal($order.Data, $after.Data)) { [MeridianBoot.Native]::Write('BootOrder', $order.Data, $order.Attributes) }
        Save-State $state
        Write-Host "Installed $Description as $($state.BootName). BootNext is set for the next boot only; BootOrder unchanged." -ForegroundColor Green
        Write-Host 'Reboot and pick Windows; run Install again and pick Linux. When both work: -Action Promote.'
    }
    elseif ($Action -eq 'Promote') {
        if ($null -eq $state -or $null -eq (Read-Var $state.BootName)) { throw 'Not installed yet; run -Action Install first.' }
        $order = Read-Var 'BootOrder'
        $entries = @([MeridianBoot.Native]::DecodeOrder($order.Data))
        if ($entries.Count -gt 0 -and $entries[0] -eq [uint16]$state.BootNumber) { Write-Host 'Meridian is already first.' }
        else {
            if (-not $state.Promoted) { $state.OriginalBootOrder = [Convert]::ToBase64String($order.Data); $state.Promoted = $true }
            $new = @([uint16]$state.BootNumber) + @($entries | Where-Object { $_ -ne [uint16]$state.BootNumber })
            [MeridianBoot.Native]::Write('BootOrder', [MeridianBoot.Native]::EncodeOrder([uint16[]]$new), $order.Attributes)
            Save-State $state
        }
    }
    elseif ($Action -eq 'Demote' -or $Action -eq 'Remove') {
        if ($null -ne $state) {
            $order = Read-Var 'BootOrder'
            if ($state.Promoted -and $state.OriginalBootOrder) {
                [MeridianBoot.Native]::Write('BootOrder', [Convert]::FromBase64String($state.OriginalBootOrder), $order.Attributes)
                $state.Promoted = $false
            } else {
                $entries = @([MeridianBoot.Native]::DecodeOrder($order.Data))
                $rest = @($entries | Where-Object { $_ -ne [uint16]$state.BootNumber })
                if ($rest.Count -ne $entries.Count -and $rest.Count -gt 0) { [MeridianBoot.Native]::Write('BootOrder', [MeridianBoot.Native]::EncodeOrder([uint16[]]$rest), $order.Attributes) }
            }
        }
        if ($Action -eq 'Remove') {
            if ($null -ne $state) {
                $next = Read-Var 'BootNext'
                if ($null -ne $next -and [BitConverter]::ToUInt16($next.Data,0) -eq [uint16]$state.BootNumber) { [MeridianBoot.Native]::Delete('BootNext') }
                [MeridianBoot.Native]::Delete($state.BootName)
            }
            $dest = Join-Path (Get-WindowsEsp).Root 'EFI\Meridian'
            if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
            if (Test-Path -LiteralPath $StatePath) { Remove-Item -LiteralPath $StatePath }
            Write-Host 'Removed the Meridian entry, BootNext and \EFI\Meridian. Windows and Limine loaders untouched.' -ForegroundColor Green
        } elseif ($null -ne $state) { Save-State $state }
    }

    Write-Host '== Status ==' -ForegroundColor Cyan
    try { Write-Host ('Windows ESP: ' + (Get-WindowsEsp).Partition.Guid) } catch { Write-Host "Windows ESP: $($_.Exception.Message)" }
    try { Write-Host ('Linux ESP:   ' + (Get-LinuxEsp).Partition.Guid) } catch { Write-Host "Linux ESP:   $($_.Exception.Message)" }
    Write-Host ('BootOrder:   ' + (Fmt-Order (Read-Var 'BootOrder')))
    $n = Read-Var 'BootNext'; Write-Host ('BootNext:    ' + $(if ($null -eq $n) { 'none' } else { '{0:X4}' -f [BitConverter]::ToUInt16($n.Data,0) }))
    $c = Read-Var 'BootCurrent'; if ($null -ne $c) { Write-Host ('BootCurrent: ' + ('{0:X4}' -f [BitConverter]::ToUInt16($c.Data,0))) }
    Write-Host 'Firmware entries:'
    foreach ($e in Get-BootEntries) { Write-Host ('  Boot{0}  {1}{2}' -f $e.Name, $e.Description, $(if ($null -ne $state -and $e.Number -eq [int]$state.BootNumber) { '   <- meridian-woa' } else { '' })) }
} finally {
    foreach ($m in $Mounted) { & mountvol.exe $m /D | Out-Null }
}
