# 7-Zip deflate 解码器（最小裁剪）

这里只有 7-Zip SDK 中**解压 deflate 流**必需的那部分源码，不是完整 SDK。

## 为什么是拷贝而不是引用

AiPy 必须能单独拷到另一个仓库继续构建。若 CMake 里写
`F:\AISoft\AiZip\third_party\7z` 这样的绝对路径，独立性当场就没了——
这正是 AGENTS.md 铁律五记录的那类错误。

## 为什么保持原目录结构

7-Zip 源码里的 include 是相对路径（`../../Common/Common.h`），
挪动位置就编不过。所以 `CPP/`、`C/` 两层原样保留。

## 文件清单

| 目录 | 内容 |
|---|---|
| `CPP/7zip/Compress/` | `DeflateDecoder` 本体、Huffman/Bit 解码、输出窗口 |
| `CPP/7zip/Common/` | `InBuffer` / `OutBuffer` |
| `CPP/Common/` | COM 与类型基础（`MyCom.h` 等） |
| `C/` | `Alloc.c`（`MidAlloc`）+ 类型头 |

## 升级方式

从新版 7-Zip SDK 里重新取这批文件，按同样路径覆盖即可。文件清单由
`tools/` 下的收集脚本确定，不是手工挑的。

## 许可

7-Zip 主体是 GNU LGPL。见 `LICENSE-7zip.txt`（原样保留）。
