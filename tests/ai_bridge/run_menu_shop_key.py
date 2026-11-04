#!/usr/bin/env python3
"""Compile native scene/bridge checks against an existing CMake Ninja build."""
import json
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
    command[command.index('-c') + 1] = str(repo / 'tests/ai_bridge/menu_shop_key.cpp')
    command += ['-O0', '-I' + str(repo), '-DBRIDGE_SOURCE="' + str(repo / 'src/ai_state.cpp') + '"']
    subprocess.run(command, cwd=entry['directory'], check=True)
    command = link_args.copy()
    command[command.index('-o') + 1] = exe
    command.insert(command.index('-o'), obj)
    subprocess.run(command, cwd=build, check=True)
    subprocess.run([exe, *sys.argv[2:]], check=True)
