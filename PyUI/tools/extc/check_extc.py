#!/usr/bin/env python3
# pyui_extc 的行为检查：清单校验、依赖闭包、链接集。
#
# ── 为什么这条要单独一个测试 ──
#
# pyui_extc 决定「哪些扩展库进 exe」。它错了的症状有两条，都很贵：
#   * 少算一个库 -> 链接期「无法解析的外部符号」，而且报的是 C++ 符号名，
#     没人会想到根因在 JSON 清单里；
#   * 多算一个库 -> exe 白涨体积，与「用了才链」的承诺相反，且没有报错。
#
# 所以两头都要断言：该链的必须链，不该链的必须不链。
#
# 用法：
#   python tools/extc/check_extc.py <pyui_extc.exe 路径>
import json
import os
import subprocess
import sys
import tempfile


# 每个用例：构造一组清单 + used.json，跑工具，断言退出码与输出内容。
def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)


def module(name, functions=None, libs=None, depends=None, header='h.h'):
    return {
        'name': name,
        'header': header,
        'functions': [{'python': f, 'cpp': 'impl_' + f, 'arity': [0, 0]}
                      for f in (functions or [])],
        'depends': list(depends or []),
        'native': {
            'include': [],
            'lib': libs if libs is not None else {'win-x64': 'lib/win-x64/%s.lib' % name},
        },
    }


def package(root, name, modules):
    write(os.path.join(root, name, 'manifest.json'),
          {'format': 1, 'name': name, 'modules': modules})


def run(tool, root, used, manifests, extra=()):
    used_path = os.path.join(root, 'used.json')
    write(used_path, used)
    out_json = os.path.join(root, 'out.json')
    out_rsp = os.path.join(root, 'out.rsp')
    command = [tool, '--used', used_path, '--out-json', out_json,
               '--out-rsp', out_rsp, '--base-dir', root]
    for manifest in manifests:
        command += ['--manifest', manifest]
    command += list(extra)
    result = subprocess.run(command, capture_output=True, text=True)
    payload = None
    rsp = ''
    if os.path.isfile(out_json):
        with open(out_json, encoding='utf-8') as handle:
            payload = json.load(handle)
    if os.path.isfile(out_rsp):
        with open(out_rsp, encoding='utf-8') as handle:
            rsp = handle.read()
    return result, payload, rsp


def modules_in(payload):
    return [m['name'] for m in (payload or {}).get('modules', [])]


def libs_in(rsp):
    return [line.strip().strip('"') for line in rsp.splitlines() if line.strip()]


def run_all(tool):
    failures = []

    def check(name, condition, detail=''):
        if not condition:
            failures.append('%s%s' % (name, ('：' + detail) if detail else ''))

    with tempfile.TemporaryDirectory() as root:
        package(root, 'fs', [module('fs', ['read_file', 'write_file'])])
        package(root, 'image', [module('image', ['load'], depends=['zlib'])])
        package(root, 'zlib', [module('zlib', ['inflate'])])
        manifests = [os.path.join(root, n, 'manifest.json') for n in ('fs', 'image', 'zlib')]

        # 1. 只用了 fs：只链 fs
        result, payload, rsp = run(tool, root, {'modules': [{'name': 'fs'}]}, manifests)
        check('only_used', result.returncode == 0 and modules_in(payload) == ['fs'],
              str(result.stderr))
        check('only_used_libs', len(libs_in(rsp)) == 1 and libs_in(rsp)[0].endswith('fs.lib'),
              str(libs_in(rsp)))

        # 2. 用 image：闭包必须把 zlib 带进来，否则链接期缺符号
        result, payload, rsp = run(tool, root, {'modules': [{'name': 'image'}]}, manifests)
        check('closure', sorted(modules_in(payload)) == ['image', 'zlib'],
              str(modules_in(payload)))
        check('closure_libs', len(libs_in(rsp)) == 2, str(libs_in(rsp)))

        # 3. 什么都没用：链接集必须为空（「用了才链」的另一半）
        result, payload, rsp = run(tool, root, {'modules': []}, manifests)
        check('nothing_used', result.returncode == 0 and modules_in(payload) == [],
              str(modules_in(payload)))
        check('nothing_used_rsp', rsp.strip() == '', repr(rsp))

        # 4. 直接依赖与传递依赖要能区分（体积归因靠它）
        result, payload, _ = run(tool, root, {'modules': [{'name': 'image'}]}, manifests)
        direct = {m['name']: m['direct'] for m in (payload or {}).get('modules', [])}
        check('direct_flag', direct.get('image') is True and direct.get('zlib') is False,
              str(direct))

        # 5. 缺失的传递依赖必须报错，且指出是谁依赖的
        package(root, 'broken', [module('broken', ['x'], depends=['nothere'])])
        result, _, _ = run(tool, root, {'modules': [{'name': 'broken'}]},
                           manifests + [os.path.join(root, 'broken', 'manifest.json')])
        check('missing_dep', result.returncode != 0, '应当报错')
        check('missing_dep_message', 'broken' in result.stderr and 'nothere' in result.stderr,
              result.stderr)

        # 6. 目标平台缺库必须报错（否则会静默产出空链接集）
        package(root, 'maconly', [module('maconly', ['x'],
                                         libs={'linux-x64': 'lib/linux-x64/m.a'})])
        result, _, _ = run(tool, root, {'modules': [{'name': 'maconly'}]},
                           [os.path.join(root, 'maconly', 'manifest.json')])
        check('platform_missing', result.returncode != 0, '应当报错')

        # 7. 清单结构错误要带字段名，不能只说「解析失败」
        write(os.path.join(root, 'bad', 'manifest.json'), {
            'format': 1, 'name': 'bad',
            'modules': [{'name': 'b', 'header': 'h.h',
                         'functions': [{'python': 'f', 'cpp': 'c', 'arity': [2, 1]}],
                         'native': {'include': [], 'lib': {}}}],
        })
        result, _, _ = run(tool, root, {'modules': []},
                           [os.path.join(root, 'bad', 'manifest.json')])
        check('bad_arity', result.returncode != 0 and 'arity' in result.stderr, result.stderr)

        # 8. 重名模块必须报错：链进来哪个取决于链接顺序，不能静默接受
        package(root, 'dup1', [module('same', ['a'])])
        package(root, 'dup2', [module('same', ['b'])])
        result, _, _ = run(tool, root, {'modules': [{'name': 'same'}]},
                           [os.path.join(root, 'dup1', 'manifest.json'),
                            os.path.join(root, 'dup2', 'manifest.json')])
        check('duplicate_module', result.returncode != 0, '应当报错')

    if failures:
        for failure in failures:
            print('FAIL %s' % failure)
        print('extc check: %d 处不符' % len(failures))
        return 1
    print('extc check OK: 8 组断言')
    return 0


def main(argv):
    if not argv:
        print('用法: check_extc.py <pyui_extc 路径>', file=sys.stderr)
        return 2
    tool = argv[0]
    if not os.path.isfile(tool):
        print('check_extc: 找不到 %s' % tool, file=sys.stderr)
        return 2
    return run_all(tool)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
