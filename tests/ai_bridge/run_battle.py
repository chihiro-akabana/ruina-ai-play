#!/usr/bin/env python3
"""Link observation-only bridge harnesses against a clean native Ninja build.

Usage: python3 tests/ai_bridge/run_battle.py BUILD [--battle-source SOURCE]
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
parser.add_argument('--battle-source', type=Path,
                    help='optional old bridge source for a negative regression run')
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

with tempfile.TemporaryDirectory(prefix='ai-bridge-harness-') as directory:
    work = Path(directory)

    def compile_harness(name, harness, source, baseline=False):
        obj = str(work / (name + '.o'))
        command = [a for a in compile_args if a != '-DNDEBUG']
        command[command.index('-o') + 1] = obj
        command[command.index('-c') + 1] = str(repo / 'tests/ai_bridge' / harness)
        command += ['-I' + str(repo), '-DBRIDGE_SOURCE="' + str(source) + '"']
        if baseline:
            command += ['-DBASELINE']
        subprocess.run(command, cwd=entry['directory'], check=True)
        command = link_args.copy()
        command[command.index('-o') + 1] = str(work / name)
        command.insert(command.index('-o'), obj)
        subprocess.run(command, cwd=build, check=True)

    def run(name, *params, gate=None):
        env = os.environ.copy()
        env.pop('EASYRPG_AI_CAPTURE', None)
        if gate is not None:
            env['EASYRPG_AI_CAPTURE'] = gate
        return subprocess.check_output([str(work / name), *params], env=env, text=True)

    compile_harness('battle', 'battle_result.cpp',
                    args.battle_source.resolve() if args.battle_source else repo / 'src/ai_state.cpp')
    print(run('battle').strip(), flush=True)
