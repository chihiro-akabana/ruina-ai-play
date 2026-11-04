#!/usr/bin/env python3
"""Check gated logical key pulses and common state heartbeat: BUILD."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
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
with tempfile.TemporaryDirectory(prefix='ai-menu-shop-key-harness-') as directory:
    obj = str(Path(directory) / 'harness.o')
    exe = str(Path(directory) / 'harness')
    command = [a for a in compile_args if a != '-DNDEBUG']
    command[command.index('-o') + 1] = obj
    command[command.index('-c') + 1] = str(repo / 'tests/ai_bridge/key.cpp')
    command += ['-O0', '-I' + str(repo), '-DBRIDGE_SOURCE="' + str(repo / 'src/ai_state.cpp') + '"']
    subprocess.run(command, cwd=entry['directory'], check=True)
    command = link_args.copy()
    command[command.index('-o') + 1] = exe
    command.insert(command.index('-o'), obj)
    subprocess.run(command, cwd=build, check=True)
    for gate in [None, '0', 'true', '01', '1']:
        env = os.environ.copy()
        env.pop('EASYRPG_AI_KEY', None)
        if gate is not None:
            env['EASYRPG_AI_KEY'] = gate
        subprocess.run([exe, 'gate', '1' if gate == '1' else '0'], env=env, check=True)
    env = os.environ.copy()
    env['EASYRPG_AI_KEY'] = '1'
    for mode in ['pulse', 'disabled']:
        subprocess.run([exe, mode], env=env, check=True)
