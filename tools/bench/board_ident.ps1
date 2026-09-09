<#
.SYNOPSIS
  Which board is on which COM port, answered from the USB descriptor rather than by guessing.

.DESCRIPTION
  A COM number is a Windows registry entry, not a board identity. It is bound to the device's
  own USB serial number, so it does normally follow a board through any hub port - but that is
  a property of the driver database, not a guarantee, and it stops holding the moment a third
  Espressif board joins the bench, a driver is reinstalled, or the COM name arbiter is cleared.
  Flashing the wrong board on a bad assumption costs its settings and its firmware.

  The ESP32-S3's USB-Serial-JTAG reports its base MAC as the serial number, but on the PARENT
  composite device rather than on the CDC function child. The child's own instance id is derived
  from the hub topology and looks deliberately anonymous, which is what makes the two boards
  appear identical at first glance. DEVPKEY_Device_Parent walks to the real one.

  Nothing here opens a port, resets a board or enters download mode. That is the point:
  esptool read_mac would answer the same question by parking the board in download mode, which
  on the harness board drops every switch line it is holding.

  NO EXPECTED MAC IS WRITTEN INTO THIS FILE. Board identities stay out of the repo; pass the
  one you mean on the command line, from BOARD_TESTING.

.PARAMETER Port
  The COM port to check, e.g. COM3. Omit to list every port.

.PARAMETER ExpectMac
  The MAC the named port must have. Case and separator do not matter. Exits 1 on any mismatch,
  so this can gate a flash or an esptool call.

.EXAMPLE
  powershell -File tools/bench/board_ident.ps1
  List every serial port with the MAC of the board behind it.

.EXAMPLE
  powershell -File tools/bench/board_ident.ps1 -Port COM3 -ExpectMac aa:bb:cc:dd:ee:ff
  Exit 0 only if COM3 really is that board.
#>
[CmdletBinding()]
param(
  [string]$Port,
  [string]$ExpectMac
)

function Normalise-Mac([string]$m) {
  if ([string]::IsNullOrWhiteSpace($m)) { return '' }
  ($m -replace '[^0-9A-Fa-f]', '').ToUpper()
}

function Get-BoardPorts {
  $out = @()
  $ports = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match '\(COM(\d+)\)' }
  foreach ($p in $ports) {
    $name = if ($p.Name -match '\((COM\d+)\)') { $Matches[1] } else { $p.Name }
    # The serial number is on the parent, never on the MI_00 child - see the notes above
    $parent = (Get-PnpDeviceProperty -InstanceId $p.DeviceID -ErrorAction SilentlyContinue |
               Where-Object { $_.KeyName -eq 'DEVPKEY_Device_Parent' }).Data
    $mac = ''
    if ($parent -match '\\([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})$') { $mac = $Matches[1].ToUpper() }
    $out += [PSCustomObject]@{
      Port   = $name
      Mac    = if ($mac) { $mac } else { '(no serial number)' }
      Parent = $parent
    }
  }
  $out | Sort-Object Port
}

$boards = Get-BoardPorts

if (-not $boards -or $boards.Count -eq 0) {
  Write-Output 'No serial ports found. Both boards may be unplugged, or the harness may have'
  Write-Output 'isolated the far board''s USB data pair - which looks identical from here.'
  exit 1
}

if (-not $Port) {
  $boards | Format-Table Port, Mac -AutoSize | Out-String | Write-Output
  if ($ExpectMac) { Write-Output 'ExpectMac ignored: it only means something with -Port.' }
  exit 0
}

$hit = $boards | Where-Object { $_.Port -ieq $Port }
if (-not $hit) {
  Write-Output ("$Port is not present. Ports found: " + (($boards | ForEach-Object { $_.Port }) -join ', '))
  exit 1
}

Write-Output ("{0} is MAC {1}" -f $hit.Port, $hit.Mac)

if (-not $ExpectMac) { exit 0 }

$want = Normalise-Mac $ExpectMac
$got = Normalise-Mac $hit.Mac
if ($want.Length -ne 12) {
  Write-Output "ExpectMac '$ExpectMac' is not 12 hex digits. Refusing to compare."
  exit 1
}
if ($got -ne $want) {
  Write-Output ("REFUSING: {0} is {1}, wanted {2}. Do not flash." -f $hit.Port, $hit.Mac, $ExpectMac)
  exit 1
}
Write-Output ("Confirmed: {0} is the board you meant." -f $hit.Port)
exit 0
