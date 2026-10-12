param(
    # 启用 AddressSanitizer + UndefinedBehaviorSanitizer，产物输出到 build-san。
    # 注意：需要工具链自带 sanitizer 运行时（Linux/GCC、Clang 支持）。
    # 本机 MSYS2 ucrt64 的 g++ 缺少 libasan/libubsan，开启后会链接失败。
    [switch]$San
)

$ErrorActionPreference = "Stop"

# 优先使用 MSYS2 (ucrt64) 的 g++，否则回退 PATH 里的 g++
$msys2bin = "C:\msys64\ucrt64\bin"
if (Test-Path (Join-Path $msys2bin "g++.exe")) {
    $env:Path = "$msys2bin;$env:Path"   # 让 g++ 能找到其运行时 DLL
}
$gxx = "C:\msys64\ucrt64\bin\g++.exe"
if (-not (Test-Path $gxx)) { $gxx = "g++" }

# 用相对路径（避免中文绝对路径在 g++/ld 参数传递时被误编码导致无法写出文件）
Set-Location $PSScriptRoot

# 编译选项：默认 -O2；-San 时改用 ASan+UBSan（独立输出目录，避免与普通产物混淆）
$common = @("-Wall", "-Wextra", "-I", "include")
if ($San) {
    $flags = @("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1") + $common
    $out = "build-san"
} else {
    $flags = @("-O2") + $common
    $out = "build"
}
New-Item -ItemType Directory -Force -Path $out | Out-Null

$core  = @("src/page_cache.cpp", "src/central_cache.cpp", "src/memory_pool.cpp", "src/operator_new.cpp")

function Compile($name, $sources, $std = "c++14") {
    Write-Host "Compiling $name ..."
    $compArgs = @("-std=$std") + $flags + $sources + @("-o", (Join-Path $out "$name.exe"))
    & $gxx @compArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $name" }
}

Compile "test_size_class" (@("tests/test_size_class.cpp"))
Compile "test_page_map"   (@("tests/test_page_map.cpp"))
Compile "test_merge"      (@("tests/test_merge.cpp") + $core)
Compile "test_alloc_api"  (@("tests/test_alloc_api.cpp") + $core)
Compile "test_release_os" (@("tests/test_release_os.cpp") + $core)
Compile "test_sanity"     (@("tests/test_sanity.cpp") + $core)
Compile "test_interleave" (@("tests/test_interleave.cpp") + $core + @("-pthread"))
Compile "test_thread"     (@("tests/test_thread.cpp") + $core + @("-pthread"))
Compile "bench"           (@("benchmark/bench.cpp") + $core + @("-pthread", "-lpsapi"))
# 带对齐的 operator new 仅在 C++17 存在，故用 C++17 单独编译
Compile "test_aligned_new" (@("tests/test_aligned_new.cpp") + $core) "c++17"

Write-Host "Build OK -> $out"
