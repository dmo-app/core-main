# Gate do Core antes de um merge (Windows, PC do Diogo): configura, compila, corre os testes e escreve o resumo.
#
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1                        # testes "merge", build incremental
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1 -Clean -Desktop        # o de uma ENTREGA: build limpa + self-test
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1 -Build D:\Core-build\x -Juce D:\outra\juce
#
# JUCE (plano de testes T01): sempre de uma copia local, sem rede. Procura -Juce, depois D:\Core-deps\JUCE-9.0.5,
# depois D:\Core-build\tarefa-04-testes-ci\_deps\juce-src, e confirma que e a 9.0.5. Sem nenhuma, para logo com a
# mensagem do que falta. So -Online deixa o CMake descarregar do GitHub (o PC normalmente nao chega la).
#
# Codigo de saida: 0 = VERDE, 1 = VERMELHO. O resumo (ci/resumo.cmake: ramo, SHA, build limpa, contagem por label,
# duracao de cada teste, inventario contra ci/testes.txt e contra -Base) fica em <Build>\gate-resumo.txt.
# Nao instala nada, nao escreve na arvore do codigo (o build vai para fora dela).
param(
  [string]$Build = "",
  [switch]$Desktop,
  [switch]$Clean,
  [switch]$Online,
  [string]$Juce = "",
  [string]$Base = "main",
  [int]$Jobs = 8
)
$ErrorActionPreference = "Stop"
$src = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Build) { $Build = Join-Path "D:\Core-build" ("gate-" + (Split-Path $src -Leaf)) }

# --- JUCE local (antes de tudo: sem JUCE a resposta chega em segundos, nao num erro do CMake)
function Get-JuceVersion([string]$dir) {
  $h = Join-Path $dir "modules\juce_core\system\juce_StandardHeader.h"
  if (-not (Test-Path $h)) { return $null }
  $t = Get-Content -Raw $h
  $v = @("MAJOR", "MINOR", "BUILDNUMBER") | ForEach-Object {
    if ($t -match "#define\s+JUCE_${_}_VERSION\s+(\d+)") { $Matches[1] } else { "?" } }
  return ($v -join ".")
}
$juceDir = ""
if (-not $Online) {
  $candidates = @()
  if ($Juce) { $candidates += $Juce }
  $candidates += "D:\Core-deps\JUCE-9.0.5", "D:\Core-build\tarefa-04-testes-ci\_deps\juce-src"
  $seen = @()
  foreach ($c in $candidates) {
    $ver = Get-JuceVersion $c
    $seen += "  $c : " + $(if ($ver) { "JUCE $ver" } else { "nao existe ou nao e o JUCE" })
    if ($ver -eq "9.0.5") { $juceDir = (Resolve-Path $c).Path; break }
    if ($Juce -and $c -eq $Juce) { break }  # um -Juce dado e o unico que conta
  }
  if (-not $juceDir) {
    Write-Host "Gate do Core: VERMELHO (sem JUCE 9.0.5 local)"
    Write-Host "Procurei:"; $seen | ForEach-Object { Write-Host $_ }
    Write-Host "Copia o JUCE 9.0.5 para D:\Core-deps\JUCE-9.0.5, ou passa -Juce <pasta>, ou -Online para descarregar."
    exit 1
  }
}

# --- build limpa: apaga a pasta, mas so se for uma pasta de build (tem CMakeCache.txt) ou estiver vazia
$clean = "nao"
if ($Clean -and (Test-Path $Build)) {
  $isBuild = Test-Path (Join-Path $Build "CMakeCache.txt")
  $isEmpty = -not (Get-ChildItem -Force $Build | Select-Object -First 1)
  if (-not ($isBuild -or $isEmpty)) { Write-Host "Gate: $Build nao e uma pasta de build (sem CMakeCache.txt); nao apago."; exit 1 }
  Remove-Item -Recurse -Force $Build
}
if (-not (Test-Path (Join-Path $Build "CMakeCache.txt"))) { $clean = "sim" }
New-Item -ItemType Directory -Force -Path $Build | Out-Null

# vcvars64: o VS 18 BuildTools (o VS 2022 desta maquina nao tem vcvars64.bat); senao, o que o vswhere encontrar.
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat" }
  }
}
if (-not (Test-Path $vcvars)) { Write-Host "Gate do Core: VERMELHO (vcvars64.bat nao encontrado)"; exit 1 }

$juceArg = if ($juceDir) { "-DFETCHCONTENT_SOURCE_DIR_JUCE=`"$($juceDir -replace '\\','/')`" -DFETCHCONTENT_FULLY_DISCONNECTED=ON" } else { "" }
$label = if ($Desktop) { "" } else { "-L merge" }
$log = Join-Path $Build "gate.log"
$junit = Join-Path $Build "gate-junit.xml"
$rcFile = Join-Path $Build "gate-ctest-rc.txt"
$cmakeFile = Join-Path $Build "gate-cmake-path.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $junit, $rcFile, $cmakeFile
# Um .bat temporario numa pasta sem espacos: o cmd /c com caminhos com espacos ja partiu launchers antes.
$bat = Join-Path $env:TEMP "core-gate.bat"
@"
@echo off
call "$vcvars" > "$Build\vcvars.log" 2>&1 || exit /b 1
where cmake > "$cmakeFile"
cmake -S "$src" -B "$Build" -G Ninja -DCMAKE_BUILD_TYPE=Release $juceArg || exit /b 1
cmake --build "$Build" --parallel $Jobs || exit /b 1
ctest --test-dir "$Build" $label --no-tests=error --output-on-failure --timeout 600 --output-junit "$junit"
echo %ERRORLEVEL%> "$rcFile"
exit /b 0
"@ | Set-Content -Encoding ASCII $bat

$t0 = Get-Date
# O stderr junta-se ao stdout dentro do cmd: no PowerShell 5.1, com "Stop", um aviso do cmake no stderr redirigido
# pelo PowerShell (*>&1) virava erro fatal e o gate morria a meio do configure (medido no PC, 2026-11-16).
$ErrorActionPreference = "Continue"
cmd /c "`"$bat`" 2>&1" | Tee-Object -FilePath $log
$buildRc = $LASTEXITCODE
$secs = [int]((Get-Date) - $t0).TotalSeconds
$ctestRc = if (Test-Path $rcFile) { (Get-Content $rcFile -First 1).Trim() } else { "nao correu (configure ou build falhou)" }
$cmake = if (Test-Path $cmakeFile) { (Get-Content $cmakeFile -First 1).Trim() } else { "cmake" }
$resumo = Join-Path $Build "gate-resumo.txt"
& $cmake "-DSRC=$src" "-DBUILD=$Build" "-DJUNIT=$junit" "-DCTEST_RC=$(if ($ctestRc -match '^\d+$') { $ctestRc } else { 99 })" `
         "-DSECS=$secs" "-DCLEAN=$clean" "-DAPP=ON" "-DBASE=$Base" "-DOUT=$resumo" -P (Join-Path $src "ci\resumo.cmake") 2>&1 | Out-Null
$resumoRc = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($buildRc -ne 0) {
  "Gate do Core: VERMELHO (configure ou build falhou; ver $log)`n" + $(if (Test-Path $resumo) { Get-Content -Raw $resumo } else { "" }) |
    Set-Content -Encoding UTF8 $resumo
}
Get-Content $resumo
exit $(if ($buildRc -eq 0 -and $ctestRc -eq "0" -and $resumoRc -eq 0) { 0 } else { 1 })
