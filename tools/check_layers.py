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

INC = re.compile(r'#include\s+[<"]harmonia/([\w]+)/')
INC_FILE = re.compile(r'#include\s+[<"]harmonia/([\w./-]+)[>"]')
NS_OPEN = re.compile(r'^namespace\s+(harmonia(?:::\w+)*)\s*\{')
FORBIDDEN_INC = re.compile(r'#include\s+[<"](theia|hyperion)/')


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
    file_adj = {}
    for dirpath, _, files in os.walk(SRC):
        for f in sorted(files):
            if not f.endswith(('.hpp', '.cpp')):
                continue
            p = os.path.join(dirpath, f)
            src_layer = layer_of(p)
            me = os.path.relpath(p, SRC).replace(os.sep, '/')
            file_adj.setdefault(me, set())
            opens = set()
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
                m = INC_FILE.match(line)
                if m:
                    file_adj.setdefault(m.group(1), set())
                    if m.group(1) != me:
                        file_adj[me].add(m.group(1))
                m = NS_OPEN.match(line)
                if m:
                    opens.add(m.group(1))
                if FORBIDDEN_INC.match(line):
                    violations.append(f'{p}: renderer include (theia/hyperion) in the shared library')
            # The file must open its own layer's namespace (nested module
            # namespaces like harmonia::utils::Math count for their layer).
            # Forward-declaration blocks of other layers are fine.
            layer_ns = 'harmonia' if src_layer == 'root' else f'harmonia::{src_layer}'
            opens_own = any(o == layer_ns or o.startswith(layer_ns + '::') for o in opens)
            if not opens_own and (f.endswith('.hpp') or opens):
                violations.append(f'{p}: never opens namespace {layer_ns}')
    # Same-layer header cycles: the matrix gates layer pairs, not files inside
    # one layer - detect cycles as file-level strongly-connected components.
    for comp in scc(sorted(file_adj), file_adj):
        if len(comp) > 1:
            violations.append('include cycle: ' + ' <-> '.join(sorted(comp)))
    if report:
        print('measured include edges:')
        for (a, b), n in sorted(edges.items()):
            print(f'  {a:13} -> {b:13} x{n}')
    if violations:
        print(f'\n{len(violations)} layer violation(s):')
        for v in violations:
            print('  ' + v)
        return 1
    print(f'layers clean ({len(edges)} distinct include edges, no cycles)')
    return 0


def scc(nodes, adj):
    """Tarjan, iterative: strongly-connected components over the file graph."""
    idx, low, on, stack, out = {}, {}, set(), [], []
    counter = [0]
    for start in nodes:
        if start in idx:
            continue
        work = [(start, iter(sorted(adj.get(start, ()))))]
        idx[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on.add(start)
        while work:
            v, it = work[-1]
            advanced = False
            for w in it:
                if w not in idx:
                    idx[w] = low[w] = counter[0]
                    counter[0] += 1
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(sorted(adj.get(w, ())))))
                    advanced = True
                    break
                elif w in on:
                    low[v] = min(low[v], idx[w])
            if advanced:
                continue
            work.pop()
            if work:
                pv = work[-1][0]
                low[pv] = min(low[pv], low[v])
            if low[v] == idx[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                out.append(comp)
    return out


def io_lines(p):
    with open(p, encoding='utf-8', errors='replace') as fh:
        return fh.readlines()


if __name__ == '__main__':
    sys.exit(main())
