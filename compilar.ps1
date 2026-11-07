# Builds Ocarina Recomp from your own copy of the game: the commands of docs/build-by-hand.md, shown before they run.
#
#   powershell -ExecutionPolicy Bypass -File .\compilar.ps1 -Jogo "C:\path\to\game.nup or folder" [-Simular] [-SemPerguntar] [-Jobs 8]
#
# -Simular       shows every command and runs none of them
# -SemPerguntar  does not ask before each step
#
# What it does: needs no administrator and installs nothing (verificar.ps1 lists what is missing). It writes only inside this
# folder. It never changes your game: --consume below only runs on the copy extracted to build\dump. It goes online only for
# the Python packages (pip), the translator's .NET packages (build_xlate.py) and, when main is compressed, kinnay/zbic.
# To uninstall, delete this folder.
param(
    [Parameter(Mandatory = $true)][string]$Jogo,
    [switch]$Simular,
    [switch]$SemPerguntar,
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
$PT = (Get-UICulture).TwoLetterISOLanguageName -eq 'pt'
function L($textoPt, $textoEn) { if ($PT) { return $textoPt } return $textoEn }   # not $pt: names are case-insensitive, it would hide $PT

$raiz = $PSScriptRoot
Set-Location -LiteralPath $raiz
$py = '.venv\Scripts\python.exe'
$ZBIC = '11b08f2712264bbed731545085cbd9702096ceb7'
# each folder once: cmd.exe, which some build steps use, stops finding programs when PATH is around 8 KB long
$vistos = @{}
$env:PATH = (("C:\msys64\ucrt64\bin;C:\Program Files (x86)\Microsoft Visual Studio\Installer;$env:PATH" -split ';') | Where-Object { $_ -and -not $vistos.ContainsKey($_.ToLower()) -and ($vistos[$_.ToLower()] = $true) }) -join ';'
$env:PYTHONUTF8 = '1'

# Runs one program, or only shows it with -Simular. A failure stops the build.
function Cmd($exe) {
    $mostrar = ($args | ForEach-Object { if ("$_" -match '\s') { "`"$_`"" } else { "$_" } }) -join ' '
    Write-Host "  > $exe $mostrar" -ForegroundColor DarkGray
    if ($Simular) { return }
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw ((L 'O comando falhou (código {0}): {1}' 'The command failed (code {0}): {1}') -f $LASTEXITCODE, $exe) }
}
# A step: its title and what it does, then a question unless -SemPerguntar or -Simular.
function Etapa($numero, $titulo, $explica) {
    Write-Host ''
    Write-Host "[$numero/7] $titulo" -ForegroundColor Cyan
    Write-Host "  $explica"
    if ($Simular -or $SemPerguntar) { return }
    $r = Read-Host (L '  Continuar? (S/N)' '  Continue? (Y/N)')
    if ($r -notmatch '^[sSyY]') { Write-Host (L 'Parado. Rode de novo para continuar de onde parou.' 'Stopped. Run again to continue from here.'); exit 0 }
}
function Pulo($texto) { Write-Host "  $texto" -ForegroundColor Green }
function HashOk() {
    $certo = (Get-Content 'tools\data\expected-oot111.json' -Raw | ConvertFrom-Json).exefs_sha256.main
    return (Test-Path -LiteralPath 'build\exefs\main') -and (Get-FileHash -LiteralPath 'build\exefs\main').Hash -eq $certo.ToUpper()
}
# build\dump is ours: removing anything else is refused.
function ApagarDump() {
    $alvo = Join-Path $raiz 'build\dump'
    Write-Host "  > Remove-Item $alvo" -ForegroundColor DarkGray
    if (-not $Simular -and (Test-Path -LiteralPath $alvo)) { Remove-Item -LiteralPath $alvo -Recurse -Force }
}

try {
    if ($Simular) { Write-Host (L '=== SIMULAÇÃO: nada será executado ===' '=== SIMULATION: nothing will run ===') -ForegroundColor Yellow }
    else {
        & (Join-Path $raiz 'verificar.ps1') -Jogo $Jogo
        if ($LASTEXITCODE -ne 0) { exit 1 }
    }

    $ehNup = $Jogo -match '\.nup$'
    $base = if (Test-Path -LiteralPath (Join-Path $Jogo 'base\exefs\main')) { Join-Path $Jogo 'base' } else { $Jogo }
    $opcoes = @('--package', '0')

    Etapa 1 (L 'Preparar o Python' 'Prepare Python') (L 'Cria um ambiente em .venv e instala os pacotes de tools\requirements-build.txt. Baixa da internet.' 'Creates an environment in .venv and installs the packages in tools\requirements-build.txt. Downloads from the internet.')
    if (-not (Test-Path -LiteralPath $py)) { Cmd 'py' -3.12 -m venv .venv }
    Cmd $py -m pip install -q --disable-pip-version-check -r tools\requirements-build.txt
    if ($ehNup -and -not $Simular -and ((& $py tools\extract_nup.py $Jogo --list | Out-String) -notmatch ': 1 package')) { $opcoes = @('--overlay-update') }

    Etapa 2 (L 'Extrair e conferir o executável do jogo' "Extract and check the game's executable") (L 'Copia o exefs para build\exefs e confere a versão 1.1.1 pelo hash. Se o main estiver comprimido, baixa o zbic para descomprimir a cópia.' 'Copies the exefs to build\exefs and checks version 1.1.1 by its hash. If main is compressed, downloads zbic to decompress the copy.')
    if (-not $Simular -and (HashOk)) {
        Pulo (L 'Já feito: build\exefs é a versão 1.1.1.' 'Already done: build\exefs is version 1.1.1.')
    } else {
        $origem = if ($ehNup) { 'build\dump\exefs' } else { Join-Path $base 'exefs' }
        if ($ehNup) { Cmd $py tools\extract_nup.py $Jogo --out build\dump --only exefs --quiet @opcoes }
        Write-Host "  > Copy $origem -> build\exefs" -ForegroundColor DarkGray
        if (-not $Simular) {
            New-Item -ItemType Directory -Force 'build\exefs' | Out-Null
            Get-ChildItem -LiteralPath $origem -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path 'build\exefs' $_.Name) -Force }
        }
        if ($Simular -or -not (HashOk)) {
            if ($Simular) { Write-Host (L '  (só se o main estiver comprimido:)' '  (only if main is compressed:)') }
            if (-not (Test-Path -LiteralPath 'build\zbic\.git')) { Cmd 'git' clone https://github.com/kinnay/zbic build\zbic }
            Cmd 'git' -C build\zbic checkout -q $ZBIC
            Cmd 'clang' -O2 -w -Ibuild\zbic\src tools\oot\unzbic.c -o build\unzbic.exe
            Cmd 'build\unzbic.exe' "$origem\main" build\exefs\main
            if (-not $Simular -and -not (HashOk)) { throw (L 'Este jogo não é a versão 1.1.1, a única que funciona por enquanto.' 'This game is not version 1.1.1, the only one that works for now.') }
        }
        if ($ehNup) { ApagarDump }
    }

    Etapa 3 (L 'Compilar o tradutor de shaders' 'Build the shader translator') (L 'Compila o tradutor em build\xlate. Baixa pacotes .NET da internet.' 'Builds the translator in build\xlate. Downloads .NET packages from the internet.')
    Cmd $py tools\build_xlate.py

    Etapa 4 (L 'Converter o código do jogo' 'Convert the game code') (L 'Traduz o código do jogo para C em generated\oot111.' 'Translates the game code to C in generated\oot111.')
    Cmd $py recomp\gen.py --exefs build\exefs --out generated\oot111 --modules 'main,sdk,subsdk0'
    Cmd $py tools\oot\patch_generated.py generated\oot111

    Etapa 5 (L 'Preparar os arquivos do jogo' 'Prepare the game files') (L 'Monta a imagem de 26 GB em build\romfs. O seu arquivo original não é alterado.' 'Builds the 26 GB image in build\romfs. Your original file is not changed.')
    if (Test-Path -LiteralPath 'build\romfs\index.json') {
        Pulo (L 'Já feito: build\romfs está pronto.' 'Already done: build\romfs is ready.')
    } elseif ($ehNup) {
        Cmd $py tools\extract_nup.py $Jogo --out build\dump --only romfs @opcoes
        # --consume deletes each loose file once it is in the image: only ever on the copy extracted just above
        Cmd $py tools\pack_romfs.py --dir build\dump\romfs --out build\romfs --consume
        ApagarDump
    } else {
        Cmd $py tools\pack_romfs.py --dir (Join-Path $base 'romfs') --out build\romfs
    }

    Etapa 6 (L 'Compilar o jogo' 'Build the game') (L 'A parte mais demorada. Pode levar mais de uma hora e deixar o PC lento.' 'The longest part. It can take over an hour and slow your PC down.')
    if ($Jobs -le 0) {
        $ramGb = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
        $Jobs = [Math]::Max(2, [Math]::Min([Environment]::ProcessorCount, [int]($ramGb / 3)))
    }
    Cmd $py tools\build_game.py --version oot111 --gen generated\oot111 --out build\oot111 --expected tools\data\expected-oot111.json --exefs build\exefs --romfs build\romfs --jobs $Jobs

    Etapa 7 (L 'Configuração inicial' 'Initial settings') (L 'Copia jogar\totk.ini, se ainda não houver uma configuração.' 'Copies jogar\totk.ini, if there are no settings yet.')
    Write-Host '  > Copy jogar\totk.ini -> build\oot111\totk.ini' -ForegroundColor DarkGray
    if (-not $Simular -and -not (Test-Path -LiteralPath 'build\oot111\totk.ini')) { Copy-Item -LiteralPath 'jogar\totk.ini' -Destination 'build\oot111\totk.ini' }

    Write-Host ''
    if ($Simular) {
        Write-Host (L 'Fim da simulação. Nada foi executado.' 'End of simulation. Nothing was run.') -ForegroundColor Yellow
    } else {
        Write-Host (L 'Pronto! Para jogar:' 'Done! To play:') -ForegroundColor Green
        Write-Host '  powershell -ExecutionPolicy Bypass -File jogar\jogar-ocarina.ps1'
        Write-Host (L 'Não compartilhe o ocarina.exe, generated\ nem build\: eles saem do seu jogo.' 'Do not share ocarina.exe, generated\ or build\: they come from your game.')
    }
} catch {
    Write-Host ''
    Write-Host "$($_.Exception.Message)" -ForegroundColor Red
    Write-Host (L 'Rode o mesmo comando de novo depois de resolver: ele continua de onde parou.' 'Run the same command again after fixing it: it continues from where it stopped.')
    exit 1
}
