"""从反编译的 DSDT 解析 EC RAM 字段（Field 定义），输出 [(bit_offset, bits, name)]。"""
import re
import sys


def parse(dsl_path, region):
    text = open(dsl_path).read()
    m = re.search(r'Field \(' + region + r',[^)]*\)\s*\{(.*?)\n\s*\}', text, re.S)
    if not m:
        raise SystemExit(f'找不到 Field ({region}, ...)')
    pos = 0
    fields = []
    for tok in re.finditer(r'Offset \((0x[0-9A-F]+)\)|(\w*),\s*(\d+)', m.group(1)):
        if tok.group(1):
            pos = int(tok.group(1), 16) * 8
            continue
        name, bits = tok.group(2), int(tok.group(3))
        if name:
            fields.append((pos, bits, name))
        pos += bits
    return fields


if __name__ == '__main__':
    for off, bits, name in parse(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'ECF2'):
        print(f'0x{off // 8:02X}.{off % 8} {bits:>3} {name}')
