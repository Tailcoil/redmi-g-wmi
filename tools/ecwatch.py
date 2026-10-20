"""只读监视 EC RAM（ec_sys），按 DSDT 字段名打印变化。需要 root。

用法：ecwatch.py <DSDT.dsl> [日志文件]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecmap import parse  # noqa: E402

EC_IO = '/sys/kernel/debug/ec/ec0/io'
# 持续变化的字段，只做周期汇总
NOISY = re.compile(r'^(TSR\d|F[12](HI|LO)|B1\w\w|BTPT|BRSC|SM\w\w|SDA\w|SDAT|BS5\d|RTC1|BCYC|BALM|BATM)$')


def field_value(buf, off, bits):
    raw = int.from_bytes(buf[off // 8:(off + bits + 7) // 8], 'little')
    return (raw >> (off % 8)) & ((1 << bits) - 1)


def main():
    fields = parse(sys.argv[1], 'ECF2')
    log = open(sys.argv[2], 'a') if len(sys.argv) > 2 else sys.stdout
    covered = set()
    for off, bits, _ in fields:
        covered.update(range(off // 8, (off + bits + 7) // 8))

    def out(msg):
        log.write(f'{time.strftime("%T")} {msg}\n')
        log.flush()
        os.fsync(log.fileno())

    prev = open(EC_IO, 'rb').read()
    out('ec 开始监视')
    last_summary = 0
    while True:
        time.sleep(0.1)
        cur = open(EC_IO, 'rb').read()
        changes = []
        for off, bits, name in fields:
            if NOISY.match(name):
                continue
            a, b = field_value(prev, off, bits), field_value(cur, off, bits)
            if a != b:
                changes.append(f'{name} {a}->{b}')
        # ACPI 里没命名的字节也报告
        for i in range(256):
            if i not in covered and prev[i] != cur[i]:
                changes.append(f'[0x{i:02X}] {prev[i]}->{cur[i]}')
        if changes:
            out('ec ' + '  '.join(changes))
        if time.time() - last_summary > 10:
            t = ' '.join(str(cur[i]) for i in range(0x17, 0x20))
            out(f'ec 汇总 风扇={cur[0x9B] << 8 | cur[0x9C]}/{cur[0x9D] << 8 | cur[0x9E]} 温度={t} ITSM={cur[0xE4]}')
            last_summary = time.time()
        prev = cur


if __name__ == '__main__':
    main()
