#!/usr/bin/env python3
"""把「重链接 launcher 需要的事实」录成 JSON，供 dist/ 独立使用。

── 为什么需要这个文件 ──

`link.py` 平时靠读 `CMakeCache.txt`（拿 link.exe）和 `build.ninja`（拿
launcher 的 obj 与库清单）来确定链接命令。这两个文件属于**构建目录**，
而 dist/ 是一个要被人整个拿走的发布目录——里面没有 CMake、没有构建目录，
拿它的人也不该为了打一个 exe 去装 CMake。

所以在构建期把答案抄下来。这是「构建时知道、发布时不再知道」的那类信息：
链接器在哪、用哪些 obj、链哪些库。

── 为什么不硬编码在 link.py 里 ──

那些路径随 MSVC 版本、随安装位置变。构建期算一次、存成数据，比在脚本里
猜要可靠；换了机器只要重新构建一次 dist 即可。

用法（由 CMakeLists.txt 的 aipy_dist 目标调用）：
  dist_recipe.py --output dist/link_recipe.json --linker <link.exe> \\
                 --object <launcher.obj> --library <a.lib> --library ...
"""

import argparse
import io
import json
import os


def main():
    parser = argparse.ArgumentParser(description='生成 dist 的重链接配方')
    parser.add_argument('--output', required=True, help='写出的 JSON 路径')
    parser.add_argument('--linker', required=True, help='link.exe 绝对路径')
    parser.add_argument('--object', action='append', default=[],
                        help='launcher 的 obj（可重复）')
    parser.add_argument('--object-name', default=None,
                        help='obj 在 dist 里的文件名（默认与源文件同名）')
    parser.add_argument('--library', action='append', default=[],
                        help='库名或绝对路径（可重复）')
    args = parser.parse_args()

    recipe = {
        # 版本号：link.py 读到不认识的版本时退回读 CMake 构建目录，
        # 而不是拿着半懂的字段去拼命令行。
        'version': 1,
        'linker': os.path.abspath(args.linker),
        # obj 与库都用**相对于配方文件**的路径：dist 整个目录被搬走时，
        # 绝对路径全部失效，相对路径仍然成立。
        #
        # obj 在 dist 里统一叫 launcher.obj（构建目录里是 main.cpp.obj，
        # 那个名字对拿到 dist 的人没有意义）。运行时库同理：无论构建目录
        # 里在哪，dist 一律放 lib/<名字>，所以这里只记文件名。
        'objects': [args.object_name or os.path.basename(p) for p in args.object],
        'libraries': [os.path.basename(p) if os.path.isabs(p) else p
                      for p in args.library],
    }

    directory = os.path.dirname(os.path.abspath(args.output))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with io.open(args.output, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(recipe, handle, ensure_ascii=False, indent=2)
        handle.write('\n')

    print('%s（linker=%s，%d 个 obj，%d 个库）'
          % (args.output, os.path.basename(recipe['linker']),
             len(recipe['objects']), len(recipe['libraries'])))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
