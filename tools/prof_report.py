"""Report for a TOTK_PROFILE sampling profile (runtime/prof_win.c, runtime/platform/prof_orbis.c): top functions and the
guest/runtime/outside split.

usage: prof_report.py <profile> <totk.map | ocarina.elf> [--top 40] [--chains 30] [--timeline 80]

Windows: the map is the lld one from the link (build/<version>/totk.map); its addresses are RVAs, the profile header
carries the runtime image base, so ASLR does not matter. Symbols in the map are labels without sizes: a Rip belongs to
the nearest symbol at or below it.

PS4: pass the non-stripped ELF of the build that ran (build/<version>-orbis/ocarina.elf). Its symbols come from llvm-nm
(cached under ~/.cache/prof_report); the profile header has text_start/text_end, where the loader put the executable,
and the base is text_start minus the ELF's __text_start. A size mismatch between the two ranges means the ELF is not
the one that ran. Rips outside the executable are named after the system module (libkernel and the like) that holds
them; libkernel counts as "blocked" the way ntdll does on Windows.

A profile taken with TOTK_PROFILE_RENDER=1 or TOTK_PROFILE_CHAINS=1 (and every PS4 profile) also has call chains: the
report then adds the inclusive time per function (a function counted once per chain), and for each module outside the
exe the exe functions that call into it and the longest common chains down to it. On the PS4 the chains come from a
stack scan (return addresses checked against the call instruction before them), so a stale return address can show up
as an extra caller.

A TOTK_PROFILE_SLOW dump (<profile>.f<frame>) also has `tstack <tid> <ms> <chain>` lines: the report then prints, per
thread, a timeline of the slow interval (consecutive samples with the same top frames merged into one span), the
gaps where a thread took no sample, and the `tstall` lines (a sample that waited over 2 ms for the thread to stop).
"""
import argparse
import bisect
import collections
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

SYM = re.compile(r'^([0-9a-f]{8}) +[0-9a-f]{8} +\d+ {2,}(\S+)$')   # In column empty: object lines have a path:(section) there
NM = os.environ.get('LLVM_NM', 'llvm-nm')


def load_map(path):
    syms = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYM.match(line.rstrip('\n'))
            if m and '(' not in m.group(2):
                syms.setdefault(int(m.group(1), 16), m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs], {}


def load_elf(path):
    """Text symbols of an ELF (link addresses) and the few markers the PS4 header needs, through llvm-nm, cached."""
    st = os.stat(path)
    key = hashlib.sha1(f'{os.path.realpath(path)}:{st.st_size}:{st.st_mtime_ns}'.encode()).hexdigest()[:16]
    cache = Path.home() / '.cache' / 'prof_report' / f'{Path(path).name}.{key}.syms'
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        out = subprocess.run([NM, '-n', '--defined-only', '-C', path], check=True, capture_output=True, text=True).stdout
        tmp = cache.with_suffix('.tmp')
        tmp.write_text(out)
        tmp.replace(cache)
    syms, marks = {}, {}
    with open(cache, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split(' ', 2)
            if len(p) < 3:
                continue
            if p[2] in ('__text_start', '__eh_frame_start'):
                marks[p[2]] = int(p[0], 16)
            if p[1] in 'tTwW':
                syms.setdefault(int(p[0], 16), p[2])
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs], marks


def load_profile(path):
    prof = {'rips': [], 'modules': [], 'threads': [], 'stacks': [], 'samples': 0, 'base': 0, 'tstacks': [],
            'tstalls': [], 'roles': {}, 'text': None, 'frame': None, 'ring_from': None}
    text_start = text_end = None
    for line in Path(path).read_text().splitlines():
        p = line.split(None, 3)
        if not p:
            continue
        f = line.split()
        if p[0] == 'base':
            prof['base'] = int(p[1], 16)
        elif p[0] == 'samples':
            prof['samples'] = int(p[1])
        elif p[0] == 'thread':
            prof['threads'].append((int(f[1]), int(f[2]), int(f[3]), f[4] if len(f) > 4 else ''))
        elif p[0] == 'module':
            prof['modules'].append((int(p[1], 16), int(p[2], 16), p[3]))
        elif p[0] == 'rip':
            prof['rips'].append((int(p[1], 16), int(p[2])))
        elif p[0] == 'stack':
            prof['stacks'].append((int(f[1]), [int(x, 16) for x in f[2:]]))
        elif p[0] == 'tstack':
            prof['tstacks'].append((int(f[1]), float(f[2]), [int(x, 16) for x in f[3:]]))
        elif p[0] == 'tstall':
            prof['tstalls'].append((int(f[1]), float(f[2]), float(f[3]), [int(x, 16) for x in f[4:]]))
        elif p[0] == 'text_start':
            text_start = int(p[1], 16)
        elif p[0] == 'text_end':
            text_end = int(p[1], 16)
        elif p[0] == 'main' and len(f) >= 6:   # main <tid> render <tid> replay <tid>
            for k in range(0, len(f) - 1, 2):
                if f[k + 1] != '0':
                    prof['roles'][int(f[k + 1])] = f[k]
        elif p[0] == 'frame':
            prof['frame'] = line
        elif p[0] == 'ring_from_ms':
            prof['ring_from'] = float(p[1])
    if text_start is not None and text_end is not None:
        prof['text'] = (text_start, text_end)
    for tid, _, _, role in prof['threads']:
        if role:
            prof['roles'].setdefault(tid, role)
    return prof


def report_chains(prof, name_of, in_exe, top):
    stacks = prof['stacks']
    total = sum(n for n, _ in stacks) or 1
    print(f'\ncall chains: {total} samples in {len(stacks)} chains (render thread, or every thread with TOTK_PROFILE_CHAINS)')
    incl = collections.Counter()
    for n, chain in stacks:
        for f in set(name_of(r) for r in chain):
            incl[f] += n
    print(f'\ninclusive, top {top} (% of chain samples)')
    for f, n in incl.most_common(top):
        print(f'  {100 * n / total:6.2f}%  {n:8d}  {f}')
    # Per outside module: the innermost exe frame of each chain whose leaf is in that module
    by_mod = collections.defaultdict(collections.Counter)
    paths = collections.defaultdict(collections.Counter)
    for n, chain in stacks:
        if in_exe(chain[0]):
            continue
        mod = name_of(chain[0]).split('!')[0]
        i = next((k for k, r in enumerate(chain) if in_exe(r)), None)
        if i is None:
            by_mod[mod]['(no exe frame in the chain)'] += n
            continue
        by_mod[mod][name_of(chain[i])] += n
        paths[mod][' < '.join(name_of(r) for r in chain[i:i + 4])] += n
    for mod, c in sorted(by_mod.items(), key=lambda kv: -sum(kv[1].values())):
        m = sum(c.values())
        print(f'\n{mod}: {100 * m / total:.2f}% of chain samples; exe callers')
        for f, n in c.most_common(12):
            print(f'  {100 * n / total:6.2f}%  {n:8d}  {f}')
        for pth, n in paths[mod].most_common(6):
            print(f'      {100 * n / total:6.2f}%  {pth}')


def short(name, width=70):
    return name if len(name) <= width else name[:width - 3] + '...'


def report_timeline(prof, name_of, in_exe, depth, max_lines):
    """Per thread, the slow interval as spans of consecutive samples whose top `depth` frames match."""
    if prof['frame']:
        print(f'\n{prof["frame"]}')
    if prof['ring_from'] is not None:
        print(f'note: the sample ring starts {prof["ring_from"]:.1f} ms into the window; earlier samples were overwritten')
    by_tid = collections.defaultdict(list)
    for tid, t, chain in prof['tstacks']:
        by_tid[tid].append((t, chain))
    order = sorted(by_tid, key=lambda tid: (tid not in prof['roles'], -len(by_tid[tid])))

    def key_of(chain):
        # the leaf, then the first exe frames above it: a leaf in libkernel says little without its caller
        names = [name_of(chain[0])]
        names += [name_of(r) for r in chain[1:] if in_exe(r)][:depth - 1]
        return ' < '.join(short(n, 60) for n in names)

    for tid in order:
        smp = sorted(by_tid[tid])
        role = prof['roles'].get(tid, '')
        print(f'\nthread {tid} {role}: {len(smp)} samples, {smp[0][0]:.1f} to {smp[-1][0]:.1f} ms')
        tot = collections.Counter()
        incl = collections.Counter()
        spans = []
        for t, chain in smp:
            k = key_of(chain)
            tot[k] += 1
            for f in set(name_of(r) for r in chain):
                incl[f] += 1
            if spans and spans[-1][2] == k and t - spans[-1][1] <= 5.0:
                spans[-1][1] = t
                spans[-1][3] += 1
            else:
                spans.append([t, t, k, 1])
        print('  most samples (top frames):')
        for k, n in tot.most_common(8):
            print(f'    {100 * n / len(smp):5.1f}%  {n:6d}  {k}')
        print('  inclusive (any frame of the chain):')
        for f, n in incl.most_common(12):
            print(f'    {100 * n / len(smp):5.1f}%  {n:6d}  {short(f, 100)}')
        print(f'  timeline (spans of the same top frames; a gap over 5 ms is shown):')
        prev_end = None
        shown = 0
        # long spans first matter most, but in time order: keep the max_lines longest, print them sorted by time
        keep = set(id(s) for s in sorted(spans, key=lambda s: -(s[1] - s[0] + 1))[:max_lines])
        for s in spans:
            if prev_end is not None and s[0] - prev_end > 5.0:
                print(f'    {prev_end:7.1f} - {s[0]:7.1f} ms  ({s[0] - prev_end:.1f} ms with no sample)')
            prev_end = s[1]
            if id(s) not in keep:
                continue
            shown += 1
            print(f'    {s[0]:7.1f} - {s[1]:7.1f} ms  {s[3]:4d}  {s[2]}')
        if shown < len(spans):
            print(f'    ({len(spans) - shown} shorter spans not shown; --timeline N shows more)')
    if prof['tstalls']:
        print(f'\nstalls (the thread took over 2 ms to take the signal; ms into the window, ms waited, where it was):')
        for tid, t, w, chain in sorted(prof['tstalls'], key=lambda s: s[1]):
            role = prof['roles'].get(tid, str(tid))
            print(f'  {t:7.1f} ms  {w:7.1f} ms  {role:6s}  {" < ".join(short(name_of(r), 50) for r in chain[:5])}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('map', help='totk.map (Windows) or the non-stripped ocarina.elf (PS4)')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--chains', type=int, default=30, help='inclusive functions listed from the call chains')
    ap.add_argument('--timeline', type=int, default=80, help='spans listed per thread for a slow-frame dump')
    ap.add_argument('--depth', type=int, default=3, help='frames that tell spans of the timeline apart')
    a = ap.parse_args()
    prof = load_profile(a.profile)
    with open(a.map, 'rb') as f:
        is_elf = f.read(4) == b'\x7fELF'
    addrs, names, marks = load_elf(a.map) if is_elf else load_map(a.map)
    if prof['text']:   # PS4: the executable is the text range, the base comes from the ELF's __text_start
        lo, hi = prof['text']
        base = lo - marks.get('__text_start', 0)
        if '__eh_frame_start' in marks and '__text_start' in marks and \
                marks['__eh_frame_start'] - marks['__text_start'] != hi - lo:
            print(f'WARNING: the ELF text range ({marks["__eh_frame_start"] - marks["__text_start"]:#x} bytes) is not the '
                  f'one that ran ({hi - lo:#x}): symbols will be wrong', file=sys.stderr)
        exe_lo, exe_end = lo, hi
    else:
        base = prof['base']
        exe = next((m for m in prof['modules'] if m[0] == base), None)
        exe_lo, exe_end = base, base + exe[1] if exe else base

    def in_exe(rip):
        return exe_lo <= rip < exe_end

    def name_of(rip):
        if in_exe(rip):
            i = bisect.bisect_right(addrs, rip - base) - 1
            return names[i] if i >= 0 else '?'
        m = next((m for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        return f'{Path(m[2]).name}!+{rip - m[0]:#x}' if m else f'unknown!{rip:#x}'
    rips = prof['rips']
    if not rips:   # a slow-frame dump keeps chains only: the leaf of each sample is its rip
        rips = list(collections.Counter(chain[0] for n, chain in prof['stacks'] for _ in range(n)).items())
    total = sum(n for _, n in rips) or 1
    funcs = collections.Counter()
    cats = collections.Counter()
    for rip, n in rips:
        if in_exe(rip):
            i = bisect.bisect_right(addrs, rip - base) - 1
            name = names[i] if i >= 0 else '?'
            funcs[name] += n
            cats['guest (f_*)' if name.startswith('f_') else 'runtime host (exe)'] += n
            continue
        mod = next((Path(m[2]).name for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        cats['outside: ' + (mod or 'unknown')] += n
        funcs[(mod or 'unknown') + '!+' + hex(rip - next((m[0] for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), 0))] += n
    # waits and message loops: threads not running (Windows), libkernel's system calls (PS4)
    blocked = ('outside: ntdll', 'outside: win32u', 'outside: KERNELBASE', 'outside: libkernel')
    idle = sum(n for k, n in cats.items() if k.startswith(blocked))
    act = (total - idle) or 1
    print(f'samples {prof["samples"]} (Rips kept {total}), 1 ms nominal period; ntdll/win32u/KERNELBASE/libkernel (blocked threads) {100 * idle / total:.1f}%')
    print('\ncategory (% of all samples, % of running samples = all minus the blocked modules)')
    for k, n in cats.most_common():
        print(f'  {100 * n / total:6.2f}%  {"" if k.startswith(blocked) else format(100 * n / act, "6.2f") + "%":>7}  {n:8d}  {k}')
    print(f'\ntop {a.top} functions')
    for k, n in funcs.most_common(a.top):
        print(f'  {100 * n / total:6.2f}%  {n:8d}  {k}')
    print('\nthreads (samples, % of them inside the exe)')
    for tid, n, ie, role in sorted(prof['threads'], key=lambda t: -t[1])[:12]:
        print(f'  tid {tid:6d} {n:8d}  {100 * ie / max(n, 1):5.1f}% in exe  {role}')
    if prof['stacks']:
        report_chains(prof, name_of, in_exe, a.chains)
    if prof['tstacks']:
        report_timeline(prof, name_of, in_exe, a.depth, a.timeline)


if __name__ == '__main__':
    sys.exit(main())
