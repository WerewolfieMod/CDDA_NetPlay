# DDA MP memory harness: scripted host + RAM sampling of the client process.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO

$proto = 0x0001
function PutU16([byte[]]$b, [UInt16]$v) { $b += [byte]($v -band 0xFF); $b += [byte](($v -shr 8) -band 0xFF); $b }
function PutU32([byte[]]$b, [UInt32]$v) {
    for( $i = 0; $i -lt 4; $i++ ) { $b += [byte](($v -shr (8*$i)) -band 0xFF) }
    $b
}

$builder = New-Object System.Collections.Generic.List[byte]
# :")
Write-Output "== harness: listening on 24224"
$listener = [System.Net.Sockets.TcpListener]::new( [System.Net.IPAddress]::Loopback, 24224 )
$listener.Start()

# Start the client
$game = Start-Process -FilePath 'D:\Games\CataDDA\cdda\cataclysm-tiles.exe' `
    -ArgumentList '--mpclient','127.0.0.1:24224' -WorkingDirectory 'D:\Games\CataDDA\cdda' `
    -PassThru
Start-Sleep -Seconds 3
Write-Output "== client PID: $($game.Id)"

# Accept (waiting up to 25s)
$cli = $null
for( $i = 0; $i -lt 25 -and -not $cli; $i++ ) {
    if( $listener.Pending() ) { $cli = $listener.AcceptTcpClient() } else { Start-Sleep -Milliseconds 500 }
}
if( -not $cli ) { Write-Output "!! no connection"; Stop-Process -Id $game.Id -Force; exit 1 }

$cli.ReceiveTimeout = 10000
$cli.SendTimeout = 10000
$ns = $cli.GetStream()

# Read HELLO
$hdr = New-Object byte[] 6
$got = 0
while( $got -lt 6 ) {
    $n = $ns.Read( $hdr, $got, 6 - $got )
    if( $n -le 0 ) { break }
    $got += $n
}
$op  = [BitConverter]::ToUInt16( $hdr, 0 )
$len = [BitConverter]::ToUInt32( $hdr, 2 )
$pl  = New-Object byte[] $len
$got = 0
while( $got -lt $len ) {
    $n = $ns.Read( $pl, $got, [int]($len - $got) )
    if( $n -le 0 ) { break }
    $got += $n
}
$protoRx = [BitConverter]::ToUInt16( $pl, 0 )
$name = [System.Text.Encoding]::ASCII.GetString( $pl, 2, $pl.Length - 2 )
Write-Output "== HELLO: op=$op proto=$protoRx name='$name'"

# WELCOME
$payload = @()
$payload = PutU16 $payload $proto
$payload = PutU32 $payload 1
$frame = @()
$frame = PutU16 $frame 2
$frame = PutU32 $frame ([UInt32]$payload.Count)
$frame + $payload | ForEach-Object { $ns.WriteByte( [byte]$_ ) }
$ns.Flush()
Write-Output "== WELCOME sent"

# RAM sampling phase 1 (no snapshot)
$series = @()
for( $i = 0; $i -lt 12; $i++ ) {
    Start-Sleep -Seconds 1
    $game.Refresh()
    $series += [math]::Round( $game.WorkingSet64 / 1MB, 0 )
}
Write-Output ("== RAM (MB): " + ($series -join ' '))

# Snapshot 80x25, glyph 'x' (120), pair index 7. Payload format v2:
# [tag byte: 0=raw, 1=zstd][u16 w][u16 h][str status][cells]
$w = 80; $h = 25
$statusStr = [System.Text.Encoding]::ASCII.GetBytes( "TEST STATUS HP 100/100" )
$payload = @( 0 )  # tag: raw
$payload = PutU16 $payload ([UInt16]$w)
$payload = PutU16 $payload ([UInt16]$h)
$payload = PutU16 $payload ([UInt16]$statusStr.Length)
foreach( $b in $statusStr ) { $payload += $b }
for( $i = 0; $i -lt ($w*$h); $i++ ) {
    $glyph = [UInt32]120
    $pair  = [UInt32]((($i % 2) + 1))
    $payload = PutU32 $payload $glyph
    $payload = PutU32 $payload $pair
}
$frame = @()
$frame = PutU16 $frame 5
$frame = PutU32 $frame ([UInt32]$payload.Count)
$frame + $payload | ForEach-Object { $ns.WriteByte( [byte]$_ ) }
$ns.Flush()
Write-Output "== SNAPSHOT 80x25 sent"

# RAM sampling phase 2 (after snapshot)
$series = @()
for( $i = 0; $i -lt 12; $i++ ) {
    Start-Sleep -Seconds 1
    $game.Refresh()
    $series += [math]::Round( $game.WorkingSet64 / 1MB, 0 )
}
Write-Output ("== RAM after snapshot (MB): " + ($series -join ' '))

# Cleanup
$game.Refresh()
Write-Output "== final WS: $([math]::Round($game.WorkingSet64/1MB,0)) MB"
Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
$listener.Stop()
Write-Output "== done"
