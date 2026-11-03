param(
  [string]$OcctRoot = 'C:\Programs\OpenCASCADE-7.7.0-vc14-64\opencascade-7.7.0',
  [string]$BindAddress = '127.0.0.1',
  [ValidateRange(1024,65534)][int]$Port = 8080,
  [ValidateRange(1,60)][int]$Fps = 30,
  [ValidateRange(200000,50000000)][int]$Bitrate = 4000000,
  [string]$Token = '',
  [switch]$NoDemo,
  [switch]$Wait
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$bin=Join-Path $projectRoot 'build\bin\Release'
if (!(Test-Path -LiteralPath "$bin\TKDrawRTC.dll")) { throw 'Run scripts\build.ps1 first.' }
if ($BindAddress -notmatch '^[a-zA-Z0-9.:-]+$') { throw 'Invalid bind address' }
if ($Token -and $Token -notmatch '^[a-zA-Z0-9_-]{16,128}$') { throw 'Token must be 16..128 letters, digits, underscores or hyphens' }
$installRoot=Split-Path -Parent $OcctRoot
$env:CASROOT=$OcctRoot
$env:DRAWHOME=Join-Path $OcctRoot 'src\DrawResources'
$env:DRAWDEFAULT=Join-Path $env:DRAWHOME 'DrawDefault'
$env:CSF_DrawPluginDefaults=Join-Path $projectRoot 'build\resources'
$env:CSF_OCCTResourcePath=Join-Path $OcctRoot 'src'
$env:CSF_ShadersDirectory=Join-Path $OcctRoot 'src\Shaders'
$env:CSF_MDTVFontDirectory=Join-Path $OcctRoot 'src\FontMFT'
$env:CSF_LANGUAGE='us'
$env:MMGT_CLEAR='1'
$resources=@{
  CSF_SHMessage='SHMessage'; CSF_MDTVTexturesDirectory='Textures'; CSF_XSMessage='XSMessage';
  CSF_TObjMessage='TObj'; CSF_StandardDefaults='StdResource'; CSF_PluginDefaults='StdResource';
  CSF_XCAFDefaults='StdResource'; CSF_TObjDefaults='StdResource'; CSF_StandardLiteDefaults='StdResource';
  CSF_IGESDefaults='XSTEPResource'; CSF_STEPDefaults='XSTEPResource'; CSF_XmlOcafResource='XmlOcafResource';
  CSF_MIGRATION_TYPES='StdResource\MigrationSheet.txt'
}
foreach($entry in $resources.GetEnumerator()) {
  [Environment]::SetEnvironmentVariable($entry.Key,(Join-Path "$OcctRoot\src" $entry.Value),'Process')
}
$env:TCL_LIBRARY=Join-Path $installRoot 'tcltk-86-64\lib\tcl8.6'
$env:TK_LIBRARY=Join-Path $installRoot 'tcltk-86-64\lib\tk8.6'
$runtimePaths=@($bin,"$OcctRoot\win64\vc14\bin","$installRoot\tcltk-86-64\bin",
  "$installRoot\freetype-2.5.5-vc14-64\bin","$installRoot\freeimage-3.17.0-vc14-64\bin",
  "$installRoot\tbb_2021.5-vc14-64\bin","$installRoot\ffmpeg-3.3.4-64\bin",
  "$installRoot\openvr-1.14.15-64\bin\win64","$installRoot\draco-1.4.1-vc14-64\bin",
  "$installRoot\vtk-6.1.0-vc14-64\bin","$installRoot\qt5.11.2-vc14-64\bin")
$env:PATH=($runtimePaths -join ';')+';'+$env:PATH
$script=@'
pload MODELING VISUALIZATION DRAWRTC
rtcinit -hidden 1 -width 1280 -height 720
'@
if (!$NoDemo) { $script += "`nbox demo 80 60 40`nvdisplay -dispMode 1 demo`nvfit`n" }
$script += "`nset rtc_url [rtcstart -bind $BindAddress -port $Port -fps $Fps -bitrate $Bitrate"
if ($Token) { $script += " -token $Token" }
$script += "]`n" + 'puts "DRAWRTC_URL=$rtc_url"' + "`nvwait ::drawrtc_shutdown`n"
$startup=Join-Path $projectRoot "build\start-$Port.tcl"
[System.IO.File]::WriteAllText($startup,$script,[System.Text.UTF8Encoding]::new($false))
$stdout=Join-Path $projectRoot "build\draw-$Port.log"
$stderr=Join-Path $projectRoot "build\draw-$Port.err.log"
$process=Start-Process -FilePath "$OcctRoot\win64\vc14\bin\DRAWEXE.exe" -ArgumentList @('-v','-f',('"'+$startup+'"')) -WorkingDirectory $projectRoot -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
Write-Host "DRAW PID: $($process.Id)"
Write-Host "Log: $stdout"
for($i=0;$i -lt 100;$i++) {
  Start-Sleep -Milliseconds 100
  $process.Refresh()
  if($process.HasExited) { Get-Content $stdout,$stderr -ErrorAction SilentlyContinue; throw "DRAW exited: $($process.ExitCode)" }
  $line=Get-Content $stdout -ErrorAction SilentlyContinue | Where-Object { $_ -match 'DRAWRTC_URL=' } | Select-Object -Last 1
  if($line) { Write-Host ($line -replace '^.*DRAWRTC_URL=',''); break }
}
if($Wait) { $process.WaitForExit() }
