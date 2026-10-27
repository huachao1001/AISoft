$ErrorActionPreference = 'Stop'
$root = "C:\Users\chaohua\Desktop\MyZip\third_party\7z\src"
$out  = "C:\Users\chaohua\Desktop\MyZip\third_party\7z\sources.cmake"

# 各 OBJS 组对应的源码目录
$dirMap = [ordered]@{
  'COMMON_OBJS'        = "CPP\Common"
  'WIN_OBJS'           = "CPP\Windows"
  '7ZIP_COMMON_OBJS'   = "CPP\7zip\Common"
  'AR_OBJS'            = "CPP\7zip\Archive"
  'AR_COMMON_OBJS'     = "CPP\7zip\Archive\Common"
  '7Z_OBJS'            = "CPP\7zip\Archive\7z"
  'CAB_OBJS'           = "CPP\7zip\Archive\Cab"
  'CHM_OBJS'           = "CPP\7zip\Archive\Chm"
  'ISO_OBJS'           = "CPP\7zip\Archive\Iso"
  'NSIS_OBJS'          = "CPP\7zip\Archive\Nsis"
  'RAR_OBJS'           = "CPP\7zip\Archive\Rar"
  'TAR_OBJS'           = "CPP\7zip\Archive\Tar"
  'UDF_OBJS'           = "CPP\7zip\Archive\Udf"
  'WIM_OBJS'           = "CPP\7zip\Archive\Wim"
  'ZIP_OBJS'           = "CPP\7zip\Archive\Zip"
  'COMPRESS_OBJS'      = "CPP\7zip\Compress"
  'CRYPTO_OBJS'        = "CPP\7zip\Crypto"
}

# 解析 Arc.mak（含续行拼接）
$txt = (Get-Content "$root\CPP\7zip\Bundles\Format7zF\Arc.mak" -Raw) -replace "\\\r?\n", " "
$groups = [ordered]@{}
foreach ($line in ($txt -split "`r?`n")) {
  if ($line -match '^\s*(\w+_OBJS)\s*=\s*(.*)$') {
    $objs = [regex]::Matches($matches[2], '\$O\\(\w+)\.obj') | ForEach-Object { $_.Groups[1].Value }
    if ($objs.Count -gt 0) { $groups[$matches[1]] = @($objs) }
  }
}

$cpp = [System.Collections.Generic.List[string]]::new()
$c   = [System.Collections.Generic.List[string]]::new()
$missing = [System.Collections.Generic.List[string]]::new()

foreach ($key in $groups.Keys) {
  if ($key -eq 'C_OBJS') { continue }
  $dir = $dirMap[$key]
  if (-not $dir) { Write-Output "!! 未映射组: $key"; continue }
  foreach ($obj in $groups[$key]) {
    $rel = ($dir -replace '\\', '/') + "/$obj.cpp"
    $p = "$root\$dir\$obj.cpp"
    if (Test-Path $p) { $cpp.Add($rel) } else { $missing.Add($rel) }
  }
}

# C_OBJS：Arc.mak 基础 + 各 .mak 条件追加。
# 注意只取「无条件追加」的项；AesOpt/Sha1Opt/Sha256Opt/LzFindOpt/Sort/7zCrcOpt/XzCrc64Opt
# 都有 C 和 ASM 两个版本，官方在 x64 下走 ASM 分支，这里同理由汇编清单提供，
# 若同时编进 C 版本会与 .asm 的同名符号重复定义。
$baseC = @($groups['C_OBJS'])
$extraC = @('Aes', '7zCrc', 'XzCrc64', 'Sha1', 'Sha256')
foreach ($obj in ($baseC + $extraC | Select-Object -Unique)) {
  if (Test-Path "$root\C\$obj.c") { $c.Add("C/$obj.c") } else { $missing.Add("C/$obj.c") }
}

# 汇编
$asm = @('7zCrcOpt','AesOpt','Sha1Opt','Sha256Opt','Sort','LzFindOpt','LzmaDecOpt','XzCrc64Opt')
$asmOk = @()
foreach ($a in $asm) {
  if (Test-Path "$root\Asm\x86\$a.asm") { $asmOk += "Asm/x86/$a.asm" } else { $missing.Add("Asm/x86/$a.asm") }
}

Write-Output "cpp=$($cpp.Count) c=$($c.Count) asm=$($asmOk.Count) missing=$($missing.Count)"
if ($missing.Count -gt 0) { Write-Output "--- 缺失 ---"; $missing | ForEach-Object { Write-Output "  $_" } }

function ToCmakeList($items, $indent) {
  ($items | ForEach-Object { $indent + '"' + $_ + '"' }) -join "`n"
}

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("# 由 Tools/gen_sources.ps1 生成，勿手改")
[void]$sb.AppendLine("# 对象清单来源：7-Zip 26.05 CPP/7zip/Bundles/Format7zF/Arc.mak（即官方 7z.dll 的构建配方）")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("set(Z7_CPP_SOURCES")
[void]$sb.AppendLine((ToCmakeList $cpp "  "))
[void]$sb.AppendLine(")")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("set(Z7_C_SOURCES")
[void]$sb.AppendLine((ToCmakeList $c "  "))
[void]$sb.AppendLine(")")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("set(Z7_ASM_SOURCES")
[void]$sb.AppendLine((ToCmakeList $asmOk "  "))
[void]$sb.AppendLine(")")

Set-Content -Path $out -Value $sb.ToString() -Encoding UTF8
Write-Output "written $out"
