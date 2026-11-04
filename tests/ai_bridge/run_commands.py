#!/usr/bin/env python3
"""Link observation-only bridge harnesses against a clean native Ninja build.

Usage: python3 tests/ai_bridge/run_commands.py BUILD [--baseline cdba88b]
Requires CMAKE_EXPORT_COMPILE_COMMANDS=ON and an already built easyrpg-player.
Artifacts go in a temporary directory. No test-only production hooks are used.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('--baseline', default='cdba88b')
parser.add_argument('--allow-last-seq', action='store_true',
                    help='normalize only the intended uniform state heartbeat addition')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
build = args.build.resolve()
entry = next(c for c in json.loads((build / 'compile_commands.json').read_text())
             if c['file'].endswith('/src/ai_state.cpp'))
compile_args = shlex.split(entry['command'])
link_args = shlex.split(subprocess.check_output(
    ['ninja', '-C', str(build), '-t', 'commands', 'easyrpg-player'], text=True
).splitlines()[-1])
link_args = link_args[link_args.index('&&') + 1:]
link_args = link_args[:link_args.index('&&')]
link_args = [a for a in link_args if not a.endswith('/src/ai_state.cpp.o')
             and not a.endswith('/src/platform/sdl/main.cpp.o')]

def without_heartbeat(value):
    if isinstance(value, list):
        return [without_heartbeat(v) for v in value]
    if isinstance(value, dict):
        result = {}
        for key, child in value.items():
            if key in ('state.json', 'states.jsonl'):
                lines = []
                for line in child.splitlines():
                    state = json.loads(line)
                    assert type(state.get('last_seq')) is int and state['last_seq'] >= 0, 'missing heartbeat'
                    del state['last_seq']
                    lines.append(json.dumps(state, sort_keys=True, ensure_ascii=False, separators=(',', ':')) + '\n')
                result[key] = ''.join(lines)
            else:
                result[key] = without_heartbeat(child)
        return result
    return value

with tempfile.TemporaryDirectory(prefix='ai-bridge-harness-') as directory:
    work = Path(directory)

    def compile_harness(name, harness, source, baseline=False):
        obj = str(work / (name + '.o'))
        command = [a for a in compile_args if a != '-DNDEBUG']
        command[command.index('-o') + 1] = obj
        command[command.index('-c') + 1] = str(repo / 'tests/ai_bridge' / harness)
        command += ['-O0', '-I' + str(repo), '-DBRIDGE_SOURCE="' + str(source) + '"']
        if baseline:
            command += ['-DBASELINE']
            if 'void UpdateShop(' not in source.read_text():
                command += ['-DBASELINE_NO_SHOP']
            if 'int PollKeyCommand(' not in source.read_text():
                command += ['-DBASELINE_NO_KEY']
        subprocess.run(command, cwd=entry['directory'], check=True)
        command = link_args.copy()
        command[command.index('-o') + 1] = str(work / name)
        command.insert(command.index('-o'), obj)
        subprocess.run(command, cwd=build, check=True)

    def run(name, *params, gate=None):
        env = os.environ.copy()
        env.pop('EASYRPG_AI_CAPTURE', None)
        env.pop('EASYRPG_AI_KEY', None)
        if gate is not None:
            env['EASYRPG_AI_CAPTURE'] = gate
        return subprocess.check_output([str(work / name), *params], env=env, text=True)

    baseline_source = work / 'baseline.cpp'
    baseline_source.write_bytes(subprocess.check_output(
        ['git', 'show', args.baseline + ':src/ai_state.cpp'], cwd=repo))
    compile_harness('baseline', 'commands.cpp', baseline_source, baseline=True)
    compile_harness('current', 'commands.cpp', repo / 'src/ai_state.cpp')
    for gate in [None, '0', '1']:
        baseline = json.loads(run('baseline', 'compat', gate=gate))
        current = json.loads(run('current', 'compat', gate=gate))
        if args.allow_last_seq:
            current = without_heartbeat(current)
        assert baseline == current, f'compatibility mismatch with capture={gate!r}'
        label = 'state last_seq addition only; otherwise byte-identical' if args.allow_last_seq else 'exact differential'
        print(f'PASS {label} vs {args.baseline}: capture={gate!r}, '
              f'100 known-command scenarios, {current["checks"]} checks', flush=True)
    for gate in [None, '0', 'true', '01', '1']:
        print(run('current', 'gate', '1' if gate == '1' else '0', gate=gate).strip(), flush=True)
    for mode in ['disabled', 'capture', 'unknown']:
        print(run('current', mode, gate='1').strip(), flush=True)
