param(
  [string]$OcctRoot = 'C:\Programs\OpenCASCADE-7.7.0-vc14-64\opencascade-7.7.0'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
function Run-Native([string]$program, [string[]]$arguments) {
  & $program @arguments
  if ($LASTEXITCODE -ne 0) { throw "$program failed ($LASTEXITCODE)" }
}
Push-Location $projectRoot
try {
  New-Item -ItemType Directory -Force third_party | Out-Null
  if (!(Test-Path third_party/libdatachannel/CMakeLists.txt)) {
    Run-Native git @('clone','--branch','v0.24.5','--depth','1','--recurse-submodules','--shallow-submodules',
      'https://github.com/paullouisageneau/libdatachannel.git','third_party/libdatachannel')
  }
  if (!(Test-Path third_party/mbedtls/CMakeLists.txt)) {
    Run-Native git @('clone','--branch','mbedtls-3.6.5','--depth','1','--recurse-submodules','--shallow-submodules',
      'https://github.com/Mbed-TLS/mbedtls.git','third_party/mbedtls')
  }
  if (!(Test-Path third_party/httplib/httplib.h)) {
    New-Item -ItemType Directory -Force third_party/httplib | Out-Null
    Invoke-WebRequest 'https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h' -OutFile third_party/httplib/httplib.h
  }
  $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
  else {
    $vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  }
  Run-Native $cmakePath @('-S','.', '-B','build','-G','Visual Studio 17 2022','-A','x64',"-DOCCT_ROOT=$OcctRoot")
  Run-Native $cmakePath @('--build','build','--config','Release','--target','TKDrawRTC','--parallel','8')
} finally { Pop-Location }
