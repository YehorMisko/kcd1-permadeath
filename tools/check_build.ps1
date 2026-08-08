# Permadeath - build check
#
# Reports whether this copy of Kingdom Come: Deliverance is a build the mod's
# game-over hook can attach to, and prints what is needed to diagnose it if not.
#
# Run it by right-clicking -> "Run with PowerShell", or from a PowerShell
# window:   powershell -ExecutionPolicy Bypass -File check_build.ps1
#
# It only READS files. It changes nothing.

$ErrorActionPreference = "Stop"

# First 32 bytes of TriggerGameOver: the prologue the mod borrows, plus enough
# of the body to be unique. Stops before the first RIP-relative operand, which
# is the first thing that differs between storefront builds.
$sig = [byte[]](
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57,
    0x48,0x83,0xEC,0x60, 0x83,0x79,0x18,0x00, 0x8B,0xF2,
    0x48,0x8B,0xF9, 0x0F,0x85,0x48,0x01,0x00,0x00, 0x48,0x8B
)

Write-Output "Permadeath build check"
Write-Output "======================"
Write-Output ""

# --- find WHGame.dll -------------------------------------------------------
$dll = $null
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($rel in @("Bin\Win64\WHGame.dll", "bin\Win64\WHGame.dll")) {
    foreach ($root in @($here, (Split-Path -Parent $here))) {
        $try = Join-Path $root $rel
        if (Test-Path $try) { $dll = $try; break }
    }
    if ($dll) { break }
}
if (-not $dll) {
    foreach ($root in @($here, (Split-Path -Parent $here))) {
        $hit = Get-ChildItem -Path $root -Filter "WHGame.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $dll = $hit.FullName; break }
    }
}
if (-not $dll) {
    Write-Output "Could not find WHGame.dll."
    Write-Output "Put this script in your KingdomComeDeliverance folder (the one with"
    Write-Output "Bin and Data in it) and run it again."
    Read-Host "Press Enter to close"
    exit 1
}

$item = Get-Item $dll
Write-Output ("file   : " + $item.FullName)
Write-Output ("size   : " + $item.Length + " bytes")
Write-Output ("sha256 : " + (Get-FileHash $item.FullName -Algorithm SHA256).Hash)
Write-Output ""

$buf = [System.IO.File]::ReadAllBytes($item.FullName)

# --- PE section table, to turn a file offset into an RVA -------------------
$peOff = [BitConverter]::ToInt32($buf, 0x3C)
$nSec  = [BitConverter]::ToUInt16($buf, $peOff + 6)
$optSz = [BitConverter]::ToUInt16($buf, $peOff + 20)
$secOff = $peOff + 24 + $optSz
$sections = @()
for ($i = 0; $i -lt $nSec; $i++) {
    $o = $secOff + ($i * 40)
    $sections += [pscustomobject]@{
        Name = ([System.Text.Encoding]::ASCII.GetString($buf, $o, 8)).Trim([char]0)
        VA   = [BitConverter]::ToUInt32($buf, $o + 12)
        Raw  = [BitConverter]::ToUInt32($buf, $o + 20)
        RawSz= [BitConverter]::ToUInt32($buf, $o + 16)
    }
}

function OffsetToRva($sections, $off) {
    foreach ($s in $sections) {
        if ($off -ge $s.Raw -and $off -lt ($s.Raw + $s.RawSz)) { return ($s.VA + ($off - $s.Raw)) }
    }
    return 0
}

# --- scan ------------------------------------------------------------------
# Latin-1 maps every byte 0-255 to the same code point, so the file and the
# pattern survive the round trip unchanged and String.IndexOf can do the work.
# A byte-at-a-time PowerShell loop over 47MB takes minutes; this takes about a
# second.
$enc = [System.Text.Encoding]::GetEncoding(28591)
$hay = $enc.GetString($buf)
$needle = $enc.GetString($sig)

$hits = @()
$at = $hay.IndexOf($needle, [System.StringComparison]::Ordinal)
while ($at -ge 0) {
    $hits += $at
    $at = $hay.IndexOf($needle, $at + 1, [System.StringComparison]::Ordinal)
}

if ($hits.Count -eq 1) {
    $rva = OffsetToRva $sections $hits[0]
    Write-Output ("RESULT : supported. Function found at WHGame.dll+0x" + ("{0:X}" -f $rva))
    Write-Output ""
    Write-Output "The mod's game-over hook will attach to this build, so the last death"
    Write-Output "will show the unique ending."
} elseif ($hits.Count -eq 0) {
    Write-Output "RESULT : NOT supported by the current signature (0 matches)."
    Write-Output ""
    Write-Output "The mod will still count lives and archive the run - it refuses to patch"
    Write-Output "a build it does not recognise rather than guessing - but the last death"
    Write-Output "will show the ordinary death screen instead of the ending."
    Write-Output ""
    Write-Output "Send the file/size/sha256 above to the mod author and it can be added."
} else {
    Write-Output ("RESULT : ambiguous (" + $hits.Count + " matches) - the mod will refuse to patch.")
    foreach ($h in $hits) { Write-Output ("   offset 0x" + ("{0:X}" -f $h)) }
}

Write-Output ""
Read-Host "Press Enter to close"
