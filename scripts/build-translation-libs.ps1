# Windows counterpart of build-translation-libs.sh: CTranslate2 (MIT) and SentencePiece (Apache-2.0) as static
# libraries in a prefix, for -DCLIPITTO_TRANSLATE_PREFIX=PREFIX. Run from a Visual Studio developer prompt (cl,
# cmake, ninja on the PATH).
#   PREFIX\include\..., PREFIX\lib\ctranslate2_all.lib (CTranslate2 and its helpers merged), PREFIX\lib\sentencepiece.lib,
#   PREFIX\licenses\
param([Parameter(Mandatory = $true)][string]$Prefix)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
$Prefix = (Resolve-Path $Prefix).Path
$work = if ($env:CLIPITTO_TRANSLATE_WORK) { $env:CLIPITTO_TRANSLATE_WORK } else { "$Prefix-work" }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$ct2Tag = 'v4.5.0'; $ct2Commit = '383d063daf0bf338a22b491864cb2018eb8efd15'
$spTag = 'v0.2.0'; $spCommit = '17d7580d6407802f85855d2cc9190634e2c95624'

function Run([string]$what, [scriptblock]$block) {
    foreach ($attempt in 1..3) {
        & $block
        if ($LASTEXITCODE -eq 0) { return }
        Write-Host "::warning::$what failed (attempt $attempt); trying again"
        Start-Sleep -Seconds 20
    }
    throw "$what failed"
}
function Fetch([string]$name, [string]$url, [string]$tag, [string]$commit) {
    $dir = Join-Path $work $name
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        Run "git clone $name" { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; git clone -q --depth 1 --branch $tag $url $dir }
    }
    $got = (git -C $dir rev-parse HEAD).Trim()
    if ($got -ne $commit) { throw "${name}: expected commit $commit, got $got" }
    Run "git submodules of $name" { git -C $dir submodule update -q --init --recursive --depth 1 }
}
Fetch 'ctranslate2' 'https://github.com/OpenNMT/CTranslate2.git' $ct2Tag $ct2Commit
Fetch 'sentencepiece' 'https://github.com/google/sentencepiece.git' $spTag $spCommit

# CTranslate2
$ct2 = Join-Path $work 'ct2-build'
cmake -S "$work\ctranslate2" -B $ct2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF `
    -DWITH_MKL=OFF -DWITH_DNNL=OFF -DWITH_RUY=ON -DOPENMP_RUNTIME=NONE -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if ($LASTEXITCODE -ne 0) { throw 'CTranslate2 configure failed' }
cmake --build $ct2
if ($LASTEXITCODE -ne 0) { throw 'CTranslate2 build failed' }
$libs = @(Get-ChildItem -Recurse -Filter *.lib $ct2 | Where-Object { $_.Name -ne 'ctranslate2.lib' } | Sort-Object FullName | ForEach-Object { $_.FullName })
New-Item -ItemType Directory -Force -Path "$Prefix\lib", "$Prefix\include", "$Prefix\licenses" | Out-Null
lib.exe /nologo "/OUT:$Prefix\lib\ctranslate2_all.lib" "$ct2\ctranslate2.lib" @libs
if ($LASTEXITCODE -ne 0) { throw 'merging the CTranslate2 libraries failed' }
Copy-Item -Recurse -Force "$work\ctranslate2\include\*" "$Prefix\include\"

# SentencePiece
$sp = Join-Path $work 'sp-build'
cmake -S "$work\sentencepiece" -B $sp -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPM_ENABLE_SHARED=OFF -DSPM_BUILD_TEST=OFF `
    "-DCMAKE_INSTALL_PREFIX=$work\sp-install" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if ($LASTEXITCODE -ne 0) { throw 'SentencePiece configure failed' }
cmake --build $sp
if ($LASTEXITCODE -ne 0) { throw 'SentencePiece build failed' }
cmake --install $sp | Out-Null
Copy-Item -Force "$work\sp-install\lib\sentencepiece.lib" "$Prefix\lib\"
Copy-Item -Force "$work\sp-install\include\sentencepiece_processor.h" "$Prefix\include\"

# Licences that travel with the app
Copy-Item "$work\ctranslate2\LICENSE" "$Prefix\licenses\CTranslate2-LICENSE.txt"
Copy-Item "$work\ctranslate2\third_party\cpu_features\LICENSE" "$Prefix\licenses\cpu_features-LICENSE.txt"
Copy-Item "$work\ctranslate2\third_party\ruy\LICENSE" "$Prefix\licenses\ruy-LICENSE.txt"
Copy-Item "$work\ctranslate2\third_party\spdlog\LICENSE" "$Prefix\licenses\spdlog-LICENSE.txt"
Copy-Item "$work\sentencepiece\LICENSE" "$Prefix\licenses\SentencePiece-LICENSE.txt"
$cpuinfo = "$work\ctranslate2\third_party\ruy\third_party\cpuinfo\LICENSE"
if (Test-Path $cpuinfo) { Copy-Item $cpuinfo "$Prefix\licenses\cpuinfo-LICENSE.txt" }
Write-Host "Translation libraries are in $Prefix"
