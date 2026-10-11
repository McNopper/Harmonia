#!/usr/bin/env python3
"""Layer/architecture rules for harmonia (the code-quality-matrix "arch rules" cell).

Checks, over src/harmonia/**:
  1. #include edges between harmonia layers are in the allowed matrix (no upward
     or sideways-in cycles - the DAG is the design contract);
  2. every file opens the namespace matching its layer (`harmonia::<layer>`).

Exit codes: 0 = clean, 1 = violations, 77 = skipped (surface not found).
Usage: python tools/check_layers.py [--report]
"""
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'harmonia')

# The allowed layer->layer include matrix (self + downward only). `root` is the
# shared vocabulary (DeviceContext, GpuTypes) that any layer may include.
ALLOWED = {
    'root':         {'root', 'core', 'utils'},
    'utils':        {'utils', 'root'},
    'core':         {'core', 'utils', 'root'},
    'vulkan_init':  {'vulkan_init', 'core', 'utils', 'root'},
    'rt':           {'rt', 'core', 'utils', 'root'},
    'scene':        {'scene', 'core', 'utils', 'rt', 'root'},
    'renderer':     {'renderer', 'core', 'utils', 'rt', 'scene', 'root'},
    'pipeline':     {'pipeline', 'core', 'utils', 'root'},
    'presentation': {'presentation', 'core', 'utils', 'root'},
    'app':          {'app', 'core', 'utils', 'rt', 'scene', 'renderer', 'pipeline',
                     'presentation', 'vulkan_init', 'root'},
}

INC = re.compile(r'#include\s+"harmonia/([\w]+)/')
NS = re.compile(r'^namespace harmonia(?::(\w+))?\s*\{')


def layer_of(path):
    rel = os.path.relpath(path, SRC)
    return rel.split(os.sep)[0] if os.sep in rel else 'root'


def main():
    report = '--report' in sys.argv
    if not os.path.isdir(SRC):
        print(f'skip: {SRC} not found')
        return 77
    violations = []
    edges = {}
    for dirpath, _, files in os.walk(SRC):
        for f in sorted(files):
            if not f.endswith(('.hpp', '.cpp')):
                continue
            p = os.path.join(dirpath, f)
            src_layer = layer_of(p)
            ns_seen = None
            for line in io_lines(p):
                m = INC.match(line)
                if m:
                    dst = m.group(1)
                    if dst not in ALLOWED:
                        continue  # not a harmonia layer (or unknown include root)
                    edges.setdefault((src_layer, dst), 0)
                    edges[(src_layer, dst)] += 1
                    if dst not in ALLOWED[src_layer]:
                        violations.append(f'{p}: layer {src_layer} must not include harmonia/{dst}/')
                m = NS.match(line)
                if m and ns_seen is None:
                    ns_seen = m.group(1) or 'root'
            if ns_seen is not None and ns_seen != src_layer:
                violations.append(f'{p}: namespace harmonia::{ns_seen} does not match layer {src_layer}')
    if report:
        print('measured include edges:')
        for (a, b), n in sorted(edges.items()):
            print(f'  {a:13} -> {b:13} x{n}')
    if violations:
        print(f'\n{len(violations)} layer violation(s):')
        for v in violations:
            print('  ' + v)
        return 1
    print(f'layers clean ({len(edges)} distinct include edges)')
    return 0


def io_lines(p):
    with open(p, encoding='utf-8', errors='replace') as fh:
        return fh.readlines()


if __name__ == '__main__':
    sys.exit(main())
