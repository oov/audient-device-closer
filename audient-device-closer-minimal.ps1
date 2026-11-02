<#
.SYNOPSIS
  Minimal stand-in for audient-device-closer.

.DESCRIPTION
  Reads the same JSON settings as the program, kept inside this script or named by -ConfigPath,
  and does only what the recovery needs:
    1. find an Audient device whose audio endpoints are gone,
    2. ask the system to release it,
    3. close the mixers the refusal names,
    4. ask again,
    5. if it is refused again, list the processes that hold it.

  Every device the settings name is handled, one after another, and every mixer of the
  settings' list that the refusal names is closed.

  The release needs administrator rights; the script starts an elevated copy of itself,
  and says so and stops when that is refused.

.PARAMETER ConfigPath
  A settings file to read instead of the settings kept inside this script.  A file that is not
  there, or one that is not readable as settings, ends the run with exit code 2.

.PARAMETER DryRun
  Only look and report; never release or close anything.

.PARAMETER Force
  Try a device even when it is not in the state this script is for.

.PARAMETER MaxAttempts
  How many removals one device is given.  Two is what the program does: the mixers are
  closed after the first refusal, and the second refusal is the answer.
#>
[CmdletBinding()]
param(
  [string]$ConfigPath = '',
  [switch]$DryRun,
  [switch]$Force,
  [ValidateRange(2, 10)][int]$MaxAttempts = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The settings of audient-device-closer.json are kept inside this script so that it can be
# carried around as one file.  To bring in a new settings file, paste its whole text between
# the two markers below; nothing else in this script has to change.  A test compares these
# settings with the file, so a change there has to come here too.
$EmbeddedSettings = @'
{
  "ks_service": "audientusbaudioks",
  "devices": [
    { "name": "iD22", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0001&KS" },
    { "name": "iD14", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS" },
    { "name": "iD4", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0003&KS" },
    { "name": "Sono", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0004&KS" },
    { "name": "iD44", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0005&KS" },
    { "name": "EVO4", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0006&KS" },
    { "name": "EVO8", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0007&KS" },
    { "name": "iD14", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS" },
    { "name": "iD4", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0009&KS" },
    { "name": "EVO16", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_000A&KS" },
    { "name": "iD44", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS" },
    { "name": "iD24", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_000D&KS" },
    { "name": "ORIA", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_000E&KS" },
    { "name": "iD4 Stream OTG", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_000F&KS" },
    { "name": "iD14 Stream OTG", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0010&KS" },
    { "name": "iD48", "id": "TUSBAUDIO_ENUM\\VID_2708&PID_0012&KS" }
  ],
  "veto_log_name": "System",
  "veto_provider_name": "Microsoft-Windows-Kernel-PnP",
  "veto_event_id": 225,
  "mixer_process_names": [
    "iD.exe",
    "EVO.exe",
    "Sono.exe",
    "ORIA Control.exe"
  ],
  "task_dir": "AudientDeviceCloser",
  "task_name": "Device recovery after resume from sleep",
  "auto_run_delay_ms": 5000,
  "confirm_max_wait_ms": 5000,
  "veto_max_wait_ms": 5000,
  "use_theme": true,
  "allow_audio_service_pause": false,
  "simulate": ""
}
'@

Add-Type -Namespace Adc -Name Cfg -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("cfgmgr32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint CM_Locate_DevNodeW(out uint devinst, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder devnode, uint flags);

[System.Runtime.InteropServices.DllImport("cfgmgr32.dll")]
public static extern uint CM_Get_Parent(out uint devinst, uint child, uint flags);

[System.Runtime.InteropServices.DllImport("cfgmgr32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint CM_Get_Device_IDW(uint devinst, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder buffer, uint length, uint flags);

[System.Runtime.InteropServices.DllImport("cfgmgr32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint CM_Get_Device_Interface_List_SizeW(out uint length, ref System.Guid classGuid, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string deviceId, uint flags);

[System.Runtime.InteropServices.DllImport("cfgmgr32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint CM_Get_Device_Interface_ListW(ref System.Guid classGuid, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string deviceId, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder buffer, uint bufferLength, uint flags);

[System.Runtime.InteropServices.DllImport("cfgmgr32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint CM_Query_And_Remove_SubTreeW(uint devinst, out int vetoType, [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder vetoName, uint nameLength, uint flags);
'@

# CONFIGRET values, from cfgmgr32.h of the Windows SDK.
$CR_SUCCESS = 0x00000000
$CR_INVALID_DEVNODE = 0x00000005
$CR_NO_SUCH_DEVNODE = 0x0000000D
$CR_REMOVE_VETOED = 0x00000017
$CR_ACCESS_DENIED = 0x00000033

# The audio interface categories of the KS filter: render and capture.
# The presence of either one is what "the audio device is back" means here.
$KsCategories = @(
  [guid]'65e8773e-8f56-11d0-a3b9-00a0c9223196',
  [guid]'65e8773d-8f56-11d0-a3b9-00a0c9223196'
)
$CM_GET_DEVICE_INTERFACE_LIST_PRESENT = 0
$CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES = 1

# PNP_VETO_TYPE, from cfgmgr32.h: only the ones a user can act on are named.
$VetoTypes = @{
  0  = 'unknown'
  1  = 'legacy device'
  2  = 'a close still pending'
  3  = 'an application'
  4  = 'a Windows service'
  5  = 'a handle still open'
  6  = 'the device itself'
  7  = 'a driver'
  10 = 'a device that cannot be disabled'
  11 = 'a legacy driver'
}

# The bounds the program puts on the settings, so the same file is refused the same way.
$WaitMaxMs = 600000
$DefaultWaitMs = 5000
$EventIdMax = 65535

function Test-Admin {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-JsonProperty {
  <#
    .SYNOPSIS
      One property of a JSON object, matched by its exact name.
  #>
  param($Object, [string]$Key)

  foreach ($property in $Object.PSObject.Properties) {
    if ($property.Name -ceq $Key) { return $property }
  }
  return $null
}

function Get-JsonString {
  param($Object, [string]$Key, [bool]$Required = $true)

  $property = Get-JsonProperty -Object $Object -Key $Key
  if ($null -eq $property) {
    if ($Required) { throw "The settings file has no $Key." }
    return $null
  }
  if ($property.Value -isnot [string]) { throw "$Key must be a string." }
  $value = [string]$property.Value
  if ($Required -and ($value.Length -eq 0)) { throw "$Key must not be empty." }
  return $value
}

function Get-JsonInt {
  param($Object, [string]$Key, [bool]$Required = $true, [long]$Min = 0, [long]$Max = [long]::MaxValue, [long]$Default = 0)

  $property = Get-JsonProperty -Object $Object -Key $Key
  if ($null -eq $property) {
    if ($Required) { throw "The settings file has no $Key." }
    return $Default
  }
  $value = $property.Value
  # A JSON number arrives as Int32, Int64 or Double: a key that counts something takes no
  # fraction.
  if (($value -isnot [int]) -and ($value -isnot [long])) { throw "$Key must be a whole number." }
  if (($value -lt $Min) -or ($value -gt $Max)) { throw "$Key must be between $Min and $Max." }
  return [long]$value
}

function Get-JsonArray {
  param($Object, [string]$Key)

  $property = Get-JsonProperty -Object $Object -Key $Key
  if ($null -eq $property) { throw "The settings file has no $Key." }
  if ($property.Value -isnot [System.Array]) { throw "$Key must be an array." }
  return @($property.Value)
}

function Read-SettingsText {
  <#
    .SYNOPSIS
      Read settings as JSON text, and refuse what the program refuses.
  #>
  param([string]$Text, [string]$Source)

  try {
    $json = ConvertFrom-Json -InputObject $Text -ErrorAction Stop
  } catch {
    throw "The settings are not readable as JSON ($Source): $($_.Exception.Message)"
  }
  if (($null -eq $json) -or ($json -isnot [System.Management.Automation.PSCustomObject])) {
    throw 'The settings file must hold one JSON object.'
  }

  $ksService = Get-JsonString -Object $json -Key 'ks_service'

  $deviceItems = @(Get-JsonArray -Object $json -Key 'devices')
  if ($deviceItems.Count -eq 0) { throw 'devices must name at least one device.' }
  $devices = @()
  foreach ($item in $deviceItems) {
    if (($null -eq $item) -or ($item -isnot [System.Management.Automation.PSCustomObject])) {
      throw 'Every entry of devices must be an object.'
    }
    $name = Get-JsonString -Object $item -Key 'name'
    $id = Get-JsonString -Object $item -Key 'id'
    $devices += [pscustomobject]@{ Name = $name; Id = $id }
  }

  $mixerNames = @()
  foreach ($item in @(Get-JsonArray -Object $json -Key 'mixer_process_names')) {
    if ($item -isnot [string]) { throw 'Every entry of mixer_process_names must be a string.' }
    if ($item.Length -eq 0) { throw 'mixer_process_names must not hold an empty name.' }
    $mixerNames += [string]$item
  }
  if ($mixerNames.Count -eq 0) { throw 'mixer_process_names must name at least one image.' }

  $logName = Get-JsonString -Object $json -Key 'veto_log_name'
  $providerName = Get-JsonString -Object $json -Key 'veto_provider_name'
  $eventId = Get-JsonInt -Object $json -Key 'veto_event_id' -Min 1 -Max $EventIdMax
  $confirmWaitMs = Get-JsonInt -Object $json -Key 'confirm_max_wait_ms' -Required $false -Min 0 -Max $WaitMaxMs -Default $DefaultWaitMs
  $vetoWaitMs = Get-JsonInt -Object $json -Key 'veto_max_wait_ms' -Required $false -Min 0 -Max $WaitMaxMs -Default $DefaultWaitMs

  return [pscustomobject]@{
    Path             = $Source
    KsService        = $ksService
    Devices          = $devices
    MixerNames       = $mixerNames
    VetoLogName      = $logName
    VetoProviderName = $providerName
    VetoEventId      = [int]$eventId
    ConfirmWaitMs    = [int]$confirmWaitMs
    VetoWaitMs       = [int]$vetoWaitMs
  }
}

function Read-Settings {
  <#
    .SYNOPSIS
      Read a settings file, with the same refusals as the text it holds.
  #>
  param([string]$Path)

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "The settings file was not found: $Path"
  }
  return Read-SettingsText -Text (Get-Content -LiteralPath $Path -Raw -Encoding UTF8) -Source $Path
}

function Test-DeviceIdMatch {
  <#
    .SYNOPSIS
      Does a devnode belong to the device an identifier names.

    .DESCRIPTION
      The identifier without the instance part names every unit of the device, so the
      devnode only has to start with it.  With the instance part it names one unit, so the
      whole string has to match.  The case is folded as the system folds it.
  #>
  param([string]$Devnode, [string]$Id)

  if ((-not $Devnode) -or (-not $Id)) { return $false }
  if ($Devnode.Length -lt $Id.Length) { return $false }
  $delimiters = ([regex]::Matches($Id, '\\')).Count
  if (($delimiters -gt 1) -and ($Devnode.Length -ne $Id.Length)) { return $false }
  return [string]::Equals($Devnode.Substring(0, $Id.Length), $Id, [System.StringComparison]::OrdinalIgnoreCase)
}

function Find-DeviceEntry {
  param([string]$Devnode, $Devices)

  foreach ($entry in @($Devices)) {
    if (Test-DeviceIdMatch -Devnode $Devnode -Id $entry.Id) { return $entry }
  }
  return $null
}

function Get-AudioInterfaces {
  <#
    .SYNOPSIS
      Count the interfaces of the two audio categories for a devnode.
  #>
  param([string]$Devnode)

  $present = 0
  $registered = 0
  foreach ($category in $KsCategories) {
    foreach ($flags in @($CM_GET_DEVICE_INTERFACE_LIST_PRESENT, $CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES)) {
      $length = 0
      $guid = $category
      $cr = [Adc.Cfg]::CM_Get_Device_Interface_List_SizeW([ref]$length, [ref]$guid, $Devnode, [uint32]$flags)
      if (($cr -eq $CR_NO_SUCH_DEVNODE) -or ($cr -eq $CR_INVALID_DEVNODE)) { continue }
      if ($cr -ne $CR_SUCCESS) { throw "CM_Get_Device_Interface_List_SizeW failed: CONFIGRET=$cr" }
      if ($length -le 0) { continue }
      $buffer = [System.Text.StringBuilder]::new([int]$length)
      $guid = $category
      $cr = [Adc.Cfg]::CM_Get_Device_Interface_ListW([ref]$guid, $Devnode, $buffer, [int]$length, [uint32]$flags)
      if ($cr -ne $CR_SUCCESS) { throw "CM_Get_Device_Interface_ListW failed: CONFIGRET=$cr" }
      # The list is MULTI_SZ.  A device with no interface answers with an empty list, not
      # with an error, so an empty string has to count as none.
      $text = $buffer.ToString().Trim([char]0)
      $count = 0
      if ($text.Length -gt 0) {
        $count = @($text.Split([char]0, [System.StringSplitOptions]::RemoveEmptyEntries) | Where-Object { $_.Length -gt 0 }).Count
      }
      if ($flags -eq $CM_GET_DEVICE_INTERFACE_LIST_PRESENT) { $present += $count } else { $registered += $count }
    }
  }
  return [pscustomobject]@{ Present = $present; Registered = $registered }
}

function Get-Targets {
  <#
    .SYNOPSIS
      Every present devnode of the service the settings name, with its state.

    .DESCRIPTION
      The settings list the devices this program knows; a devnode no entry names is not a
      target, however it answers.  Every target is measured, so several devices, and
      several units of one device, all come out of this one walk.
  #>
  param($Settings)

  $devices = Get-CimInstance -ClassName Win32_PnPEntity -Filter "Service='$($Settings.KsService)'" -ErrorAction SilentlyContinue
  foreach ($device in @($devices)) {
    if ($null -eq $device) { continue }
    $entry = Find-DeviceEntry -Devnode ([string]$device.DeviceID) -Devices $Settings.Devices
    if ($null -eq $entry) { continue }
    $interfaces = Get-AudioInterfaces -Devnode ([string]$device.DeviceID)
    $problem = [int]$device.ConfigManagerErrorCode
    $state =
      if ($interfaces.Present -gt 0) { 'healthy' }
      elseif ($problem -ne 0) { 'problem' }
      elseif ($interfaces.Registered -eq 0) { 'no audio interface' }
      else { 'faulted' }
    [pscustomobject]@{
      Devnode    = [string]$device.DeviceID
      Name       = [string]$device.Name
      EntryName  = $entry.Name
      Problem    = $problem
      Present    = $interfaces.Present
      Registered = $interfaces.Registered
      State      = $state
    }
  }
}

function Get-ParentDevnode {
  param([string]$Devnode)

  $ks = 0
  $builder = [System.Text.StringBuilder]::new($Devnode)
  $cr = [Adc.Cfg]::CM_Locate_DevNodeW([ref]$ks, $builder, 0)
  if ($cr -ne $CR_SUCCESS) { throw "CM_Locate_DevNodeW failed: CONFIGRET=$cr" }
  $parent = 0
  $cr = [Adc.Cfg]::CM_Get_Parent([ref]$parent, $ks, 0)
  if ($cr -ne $CR_SUCCESS) { throw "CM_Get_Parent failed: CONFIGRET=$cr" }
  $id = [System.Text.StringBuilder]::new(256)
  $cr = [Adc.Cfg]::CM_Get_Device_IDW($parent, $id, 256, 0)
  if ($cr -ne $CR_SUCCESS) { throw "CM_Get_Device_IDW failed: CONFIGRET=$cr" }
  return [pscustomobject]@{ Devinst = $parent; Devnode = $id.ToString() }
}

function Remove-Subtree {
  <#
    .SYNOPSIS
      Ask the system to release the device, and report what it answered.
  #>
  param([uint32]$Devinst)

  $vetoType = 0
  $vetoName = [System.Text.StringBuilder]::new(512)
  $cr = [Adc.Cfg]::CM_Query_And_Remove_SubTreeW($Devinst, [ref]$vetoType, $vetoName, 512, 0)
  [pscustomobject]@{ Code = $cr; VetoType = $vetoType; VetoName = $vetoName.ToString() }
}

function Get-VetoBaseline {
  <#
    .SYNOPSIS
      The record number the log stood at, so only later records are read.
  #>
  param($Settings)

  $newest = Get-WinEvent -FilterHashtable @{
    LogName      = $Settings.VetoLogName
    ProviderName = $Settings.VetoProviderName
    Id           = $Settings.VetoEventId
  } -MaxEvents 1 -ErrorAction SilentlyContinue
  if ($null -eq $newest) { return 0 }
  return [int]$newest.RecordId
}

function ConvertFrom-VetoEvent {
  param($Event)

  try { [xml]$xml = $Event.ToXml() } catch { return $null }
  $data = @{}
  foreach ($node in @($xml.Event.EventData.Data)) {
    if ($null -eq $node) { continue }
    $data[$node.GetAttribute('Name')] = [string]$node.InnerText
  }
  if (-not $data.ContainsKey('ProcessId')) { return $null }
  $instance = ''
  if ($data.ContainsKey('DeviceInstance')) {
    $instance = ($data['DeviceInstance'] -split "`r?`n")[0]
  }
  return [pscustomobject]@{
    RecordId       = [int]$Event.RecordId
    Time           = $Event.TimeCreated
    Pid            = [int]$data['ProcessId']
    ImagePath      = if ($data.ContainsKey('ProcessName')) { $data['ProcessName'] } else { '' }
    DeviceInstance = $instance
  }
}

function Select-VetoRecords {
  <#
    .SYNOPSIS
      The records of this attempt: written after the baseline, naming this device.
  #>
  param($Records, [int]$SinceRecordId, [string]$DeviceInstance)

  foreach ($record in @($Records)) {
    if ($null -eq $record) { continue }
    if ($record.RecordId -le $SinceRecordId) { continue }
    if ($DeviceInstance -and ($record.DeviceInstance -ine $DeviceInstance)) { continue }
    $record
  }
}

function Get-VetoRecords {
  <#
    .SYNOPSIS
      Wait for the records of a refusal this run caused, and hand them back.
  #>
  param($Settings, [int]$SinceRecordId, [string]$DeviceInstance, [int]$WaitMs)

  $deadline = (Get-Date).AddMilliseconds($WaitMs)
  while ($true) {
    $events = Get-WinEvent -FilterHashtable @{
      LogName      = $Settings.VetoLogName
      ProviderName = $Settings.VetoProviderName
      Id           = $Settings.VetoEventId
    } -MaxEvents 20 -ErrorAction SilentlyContinue
    $records = @()
    foreach ($event in @($events)) {
      if ($null -eq $event) { continue }
      $record = ConvertFrom-VetoEvent -Event $event
      if ($null -eq $record) { continue }
      $records += $record
    }
    $records = @(Select-VetoRecords -Records $records -SinceRecordId $SinceRecordId -DeviceInstance $DeviceInstance)
    if ($records.Count -gt 0) { return $records }
    if ((Get-Date) -ge $deadline) { return @() }
    Start-Sleep -Milliseconds 250
  }
}

function Get-Blockers {
  <#
    .SYNOPSIS
      What the records name, as the machine is now.
  #>
  param($Records, [string[]]$MixerNames)

  foreach ($record in @($Records)) {
    $running = $null
    if ($record.Pid -gt 0) {
      $running = Get-Process -Id $record.Pid -ErrorAction SilentlyContinue
    }
    $name = ''
    $reused = $false
    $alive = $false
    if ($null -ne $running) {
      $name = $running.ProcessName + '.exe'
      $alive = $true
      # A pid is handed out again: a process that started after the record was written
      # is not the one the record named, so it must not be closed.
      try {
        if ($running.StartTime -gt $record.Time.ToLocalTime().AddSeconds(1)) {
          $reused = $true
          $alive = $false
        }
      } catch { }
    } else {
      $name = [System.IO.Path]::GetFileName($record.ImagePath)
    }
    $mixer = $false
    $audio = $false
    if ($alive) {
      foreach ($candidate in @($MixerNames)) {
        if ($candidate -ieq $name) { $mixer = $true }
      }
      $audio = ($name -ieq 'audiodg.exe')
    }
    $kind = 'other'
    if ($mixer) { $kind = 'mixer' } elseif ($audio) { $kind = 'audio' }
    [pscustomobject]@{
      Pid      = $record.Pid
      Name     = $name
      Path     = if ($null -ne $running -and -not $reused) { $running.Path } else { $record.ImagePath }
      Alive    = $alive
      Reused   = $reused
      Mixer    = $mixer
      Kind     = $kind
      Time     = $record.Time
      RecordId = $record.RecordId
    }
  }
}

function Format-Blocker {
  param($Blocker)

  $what =
    if (-not $Blocker.Alive) {
      if ($Blocker.Reused) { 'the id now belongs to another program' } else { 'no longer running' }
    } elseif ($Blocker.Kind -eq 'mixer') {
      'in use (mixer)'
    } elseif ($Blocker.Kind -eq 'audio') {
      'in use (Windows audio)'
    } else {
      'in use'
    }
  return "  - $($Blocker.Name) (PID $($Blocker.Pid)): $what"
}

function Stop-Blocker {
  <#
    .SYNOPSIS
      End one process the settings allow this script to close.
  #>
  param($Blocker)

  Stop-Process -Id $Blocker.Pid -Force -ErrorAction Stop
}

function Wait-Endpoints {
  param([string]$Devnode, [int]$WaitMs)

  $deadline = (Get-Date).AddMilliseconds($WaitMs)
  while ($true) {
    try {
      if ((Get-AudioInterfaces -Devnode $Devnode).Present -gt 0) { return $true }
    } catch {
      Write-Verbose $_
    }
    if ((Get-Date) -ge $deadline) { return $false }
    Start-Sleep -Milliseconds 500
  }
}

function Invoke-Target {
  <#
    .SYNOPSIS
      Take one device through the release and report whether it is free.

    .OUTPUTS
      true when nothing is left to do for this device, false when it is still held.
  #>
  param($Target, $Settings, [bool]$DryRun, [bool]$Force, [int]$MaxAttempts)

  Write-Host "$($Target.Name): $($Target.State)"
  Write-Verbose "devnode=$($Target.Devnode) entry=$($Target.EntryName) present=$($Target.Present) registered=$($Target.Registered)"

  if ($Target.State -eq 'healthy' -and -not $Force) {
    Write-Host '  The audio device is there. Nothing to do.'
    return $true
  }
  if ($Target.State -eq 'problem') {
    Write-Host '  The device is in a state this script does not repair. Nothing to do.'
    Write-Verbose "problem=$($Target.Problem)"
    return $false
  }
  if ($Target.State -eq 'no audio interface' -and -not $Force) {
    Write-Host '  No audio interface is registered for the device. It is out of scope for this script (try -Force).'
    return $false
  }

  $parent = Get-ParentDevnode -Devnode $Target.Devnode
  Write-Verbose "remove devnode=$($parent.Devnode)"
  if ($DryRun) {
    Write-Host '  The device has to be released. Nothing is done because of -DryRun.'
    return $false
  }

  $attempt = 0
  while ($attempt -lt $MaxAttempts) {
    $attempt++
    $baseline = Get-VetoBaseline -Settings $Settings
    $result = Remove-Subtree -Devinst $parent.Devinst
    if ($result.Code -eq $CR_SUCCESS) {
      Write-Host '  The device was released.'
      if (Wait-Endpoints -Devnode $Target.Devnode -WaitMs $Settings.ConfirmWaitMs) {
        Write-Host '  The audio device came back.'
        return $true
      }
      Write-Host '  The device was released but the audio device did not come back.'
      return $false
    }
    if ($result.Code -eq $CR_ACCESS_DENIED) {
      Write-Host '  The device was not released: not enough rights. Run this script as administrator.'
      return $false
    }
    if ($result.Code -ne $CR_REMOVE_VETOED) {
      Write-Host '  The device could not be released.'
      Write-Verbose "removal failed: CONFIGRET=$($result.Code)"
      return $false
    }

    $reason = if ($VetoTypes.ContainsKey($result.VetoType)) { $VetoTypes[$result.VetoType] } else { 'unknown' }
    Write-Verbose "removal vetoed by=$reason name=$($result.VetoName)"
    $records = @(Get-VetoRecords -Settings $Settings -SinceRecordId $baseline -DeviceInstance $parent.Devnode -WaitMs $Settings.VetoWaitMs)
    if ($records.Count -eq 0) {
      if ($attempt -lt $MaxAttempts) { continue }
      Write-Host '  The device is in use. The program holding it could not be identified.'
      return $false
    }
    $blockers = @(Get-Blockers -Records $records -MixerNames $Settings.MixerNames)
    $mixers = @($blockers | Where-Object { $_.Alive -and $_.Mixer })

    # A process that is already gone leaves nothing to close, so the removal is asked again:
    # the device may be free now.
    if ($blockers.Count -eq 0) {
      if ($attempt -lt $MaxAttempts) { continue }
      Write-Host '  The device is in use. The program holding it could not be identified.'
      return $false
    }
    if ($mixers.Count -eq 0) {
      Write-Host '  The device is still in use. The programs holding it:'
      foreach ($blocker in $blockers) { Write-Host (Format-Blocker -Blocker $blocker) }
      return $false
    }
    if (($mixers.Count -gt 0) -and ($attempt -lt $MaxAttempts)) {
      foreach ($mixer in $mixers) {
        Write-Host "   Closing the mixer: $($mixer.Name) (PID $($mixer.Pid))"
        try {
          Stop-Blocker -Blocker $mixer
        } catch {
          Write-Host "   The mixer could not be closed: $($_.Exception.Message)"
        }
      }
      Start-Sleep -Milliseconds 500
      continue
    }

    Write-Host '  The device is still in use. The programs holding it:'
    foreach ($blocker in $blockers) { Write-Host (Format-Blocker -Blocker $blocker) }
    return $false
  }
  return $false
}

function New-ElevationArguments {
  <#
    .SYNOPSIS
      The command line that makes an elevated copy of this script do the same work.
    .DESCRIPTION
      Start-Process joins the arguments with spaces, so every value that can hold a path is
      written with double quotes.
  #>
  param(
    [Parameter(Mandatory)][string]$ScriptPath,
    [string]$SettingsPath = '',
    [switch]$Force,
    [int]$Attempts = 2,
    [switch]$VerboseOutput
  )
  $tokens = @('-NoExit', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $ScriptPath))
  if ($SettingsPath -ne '') { $tokens += @('-ConfigPath', ('"{0}"' -f $SettingsPath)) }
  if ($Force) { $tokens += '-Force' }
  if ($VerboseOutput) { $tokens += '-Verbose' }
  $tokens += @('-MaxAttempts', [string]$Attempts)
  return ($tokens -join ' ')
}

function Start-ElevatedSelf {
  <#
    .SYNOPSIS
      Starts an elevated copy of this script, which keeps its window open for the report.
    .OUTPUTS
      $true when the copy was started, $false when the raise of rights was refused.
  #>
  param(
    [Parameter(Mandatory)][string]$ScriptPath,
    [string]$SettingsPath = '',
    [switch]$Force,
    [int]$Attempts = 2,
    [switch]$VerboseOutput
  )
  $arguments = New-ElevationArguments -ScriptPath $ScriptPath -SettingsPath $SettingsPath -Force:$Force -Attempts $Attempts -VerboseOutput:$VerboseOutput
  try {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -WorkingDirectory (Split-Path -Parent $ScriptPath) -ArgumentList $arguments | Out-Null
    return $true
  } catch {
    return $false
  }
}

function Invoke-Main {
  <#
    .OUTPUTS
      The exit code: 0 when every device is free or the work went to an elevated copy, 1 when
      one is left, 2 when the run cannot start at all.
  #>
  $settings = $null
  try {
    if ($ConfigPath -eq '') {
      $settings = Read-SettingsText -Text $EmbeddedSettings -Source 'the settings inside this script'
    } else {
      $settings = Read-Settings -Path $ConfigPath
    }
  } catch {
    Write-Host $_.Exception.Message
    return 2
  }

  if (-not (Test-Admin) -and -not $DryRun) {
    if ($PSCommandPath -ne '' -and (Start-ElevatedSelf -ScriptPath $PSCommandPath -SettingsPath $ConfigPath -Force:$Force -Attempts $MaxAttempts -VerboseOutput:($VerbosePreference -ne 'SilentlyContinue'))) {
      return 0
    }
    Write-Host 'Run this script as administrator.'
    return 2
  }

  $targets = @(Get-Targets -Settings $settings)
  if ($targets.Count -eq 0) {
    Write-Host 'None of the devices in the settings file was found.'
    return 1
  }

  $failed = $false
  foreach ($target in $targets) {
    if (-not (Invoke-Target -Target $target -Settings $settings -DryRun $DryRun -Force $Force -MaxAttempts $MaxAttempts)) {
      $failed = $true
    }
  }
  if ($failed) { return 1 }
  return 0
}

# Functions stay callable when the script is dot-sourced for testing.
if ($MyInvocation.InvocationName -ne '.') {
  $exitCode = Invoke-Main
  # Text that came from Invoke-Expression has no file to hand to an elevated copy, and exit
  # would end the session that holds the report; the code is still kept for the caller.
  if ($PSCommandPath -ne '') { exit $exitCode }
  $host.SetShouldExit($exitCode)
}
