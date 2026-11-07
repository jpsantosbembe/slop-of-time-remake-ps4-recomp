# Checks whether this PC can build Ocarina Recomp. It only looks: it changes, installs and downloads nothing, and needs no administrator.
#
#   powershell -ExecutionPolicy Bypass -File .\verificar.ps1 [-Jogo "C:\path\to\game.nup or folder"]
#
# Exit code 0: all required items are present. compilar.ps1 runs this first.
param([string]$Jogo)

$PT = (Get-UICulture).TwoLetterISOLanguageName -eq 'pt'
function L($textoPt, $textoEn) { if ($PT) { return $textoPt } return $textoEn }   # not $pt: names are case-insensitive, it would hide $PT
$script:falhas = 0

function Item($ok, $texto, $correcao) {
    if ($ok) {
        Write-Host "  [ok]    $texto" -ForegroundColor Green
    } else {
        Write-Host "  [falta] $texto" -ForegroundColor Red
        if ($correcao) { Write-Host "          $correcao" -ForegroundColor Yellow }
        $script:falhas++
    }
}
function Aviso($texto) { Write-Host "  [aviso] $texto" -ForegroundColor Yellow }
function TemComando($c) { return [bool](Get-Command $c -ErrorAction SilentlyContinue) }

$raiz = $PSScriptRoot
Write-Host ''
Write-Host (L 'Ocarina Recomp: verificando este PC (nada será alterado)' 'Ocarina Recomp: checking this PC (nothing will be changed)') -ForegroundColor Cyan
Write-Host ''

Item ($env:PROCESSOR_ARCHITECTURE -eq 'AMD64') (L 'Windows de 64 bits' '64-bit Windows') ''
$ram = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
if ($ram -ge 15) { Item $true ((L 'Memória RAM: {0} GB' 'RAM: {0} GB') -f $ram) '' }
else { Aviso ((L 'Memória RAM: {0} GB. Com menos de 16 GB, nunca testamos: a compilação ou o jogo podem falhar.' 'RAM: {0} GB. We never tested with less than 16 GB: the build or the game may fail.') -f $ram) }

$unidade = $raiz.Substring(0, 1)
$livre = [Math]::Floor((Get-PSDrive $unidade).Free / 1GB)
$romfsPronto = Test-Path -LiteralPath (Join-Path $raiz 'build\romfs\index.json')
Item ($romfsPronto -or $livre -ge 60) ((L 'Espaço livre em {0}: {1} GB (precisa de 60)' 'Free space on {0}: {1} GB (60 required)') -f $unidade, $livre) ''
Item ($raiz -notmatch '[^\x21-\x7E]|[\[\]%&]') ((L 'Pasta do projeto sem espaços nem acentos: {0}' 'Project folder has no spaces or accents: {0}') -f $raiz) (L 'Mova o projeto para uma pasta como C:\OcarinaRecomp' 'Move the project to a folder like C:\OcarinaRecomp')

$gpus = (Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }) -join ', '
if ($gpus -match 'NVIDIA') { Item $true ((L 'Placa de vídeo: {0}' 'Graphics card: {0}') -f $gpus) '' }
else { Aviso ((L 'Só testamos em NVIDIA. Sua placa ({0}) pode não funcionar.' 'We only tested on NVIDIA. Your card ({0}) may not work.') -f $gpus) }

Write-Host ''
Item (TemComando 'git') 'Git' 'winget install --id Git.Git -e'
$python = (TemComando 'py') -and ((& py -3.12 --version 2>$null) -match '3\.12')
Item $python 'Python 3.12' 'winget install --id Python.Python.3.12 -e'
$dotnet = (TemComando 'dotnet') -and (& dotnet --list-sdks | Select-String '^10\.')
Item $dotnet '.NET SDK 10' 'winget install --id Microsoft.DotNet.SDK.10 -e'

$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cpp = $false
if (Test-Path -LiteralPath "$vs\vswhere.exe") {
    $vc = & "$vs\vswhere.exe" -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cpp = [bool]$vc -and (Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Lib\*\um\x64\kernel32.lib")
}
Item $cpp (L 'Ferramentas de C++ da Microsoft' "Microsoft's C++ tools") (L 'Veja o passo 1 do guia (docs/compilar-manual.md)' 'See step 1 of the guide (docs/build-by-hand.md)')

$msys = 'C:\msys64\ucrt64\bin'
$bins = 'clang.exe', 'ninja.exe', 'glslc.exe', '..\lib\libdav1d.a'
$faltando = $bins | Where-Object { -not (Test-Path -LiteralPath "$msys\$_") }
Item (-not $faltando) (L 'Compilador (MSYS2)' 'Compiler (MSYS2)') (L 'Veja o passo 1 do guia (docs/compilar-manual.md)' 'See step 1 of the guide (docs/build-by-hand.md)')

Write-Host ''
if ($Jogo) {
    $ehNup = ($Jogo -match '\.nup$') -and (Test-Path -LiteralPath $Jogo -PathType Leaf)
    $base = if (Test-Path -LiteralPath (Join-Path $Jogo 'base\exefs\main')) { Join-Path $Jogo 'base' } else { $Jogo }
    $ehPasta = (Test-Path -LiteralPath (Join-Path $base 'exefs\main')) -and (Test-Path -LiteralPath (Join-Path $base 'romfs'))
    Item ($ehNup -or $ehPasta) ((L 'Jogo encontrado: {0}' 'Game found: {0}') -f $Jogo) (L 'Indique um arquivo .nup ou uma pasta com exefs e romfs' 'Point to a .nup file or a folder with exefs and romfs')
    Write-Host (L '          A versão 1.1.1 é conferida pelo hash durante a compilação.' '          Version 1.1.1 is checked by its hash during the build.')
    Write-Host ''
}

if ($script:falhas) {
    Write-Host ((L 'Faltam {0} item(ns). Resolva o que está acima e rode de novo.' '{0} item(s) missing. Fix what is above and run this again.') -f $script:falhas) -ForegroundColor Red
    exit 1
}
Write-Host (L 'Tudo certo para compilar.' 'All set to build.') -ForegroundColor Green
exit 0
