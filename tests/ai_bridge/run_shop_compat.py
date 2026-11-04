#!/usr/bin/env python3
"""Compare native shop gameplay with no bridge commands: BASE_BUILD BUILD."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ai-shop-compat-') as directory:
    traces = []
    for n, name in enumerate(sys.argv[1:]):
        build = Path(name).resolve()
        entry = next(c for c in json.loads((build / 'compile_commands.json').read_text())
                     if c['file'].endswith('/src/ai_state.cpp'))
        obj = str(Path(directory) / f'{n}.o')
        exe = str(Path(directory) / f'{n}')
        command = shlex.split(entry['command'])
        command[command.index('-o') + 1] = obj
        command[command.index('-c') + 1] = str(repo / 'tests/ai_bridge/shop_compat.cpp')
        # Build include paths precede repo so each version uses its own scene layout.
        command += ['-O0', '-I' + str(repo)]
        subprocess.run(command, cwd=entry['directory'], check=True)
        command = shlex.split(subprocess.check_output(
            ['ninja','-C',str(build),'-t','commands','easyrpg-player'], text=True).splitlines()[-1])
        command = command[command.index('&&') + 1:]
        command = command[:command.index('&&')]
        command = [a for a in command if not a.endswith('/src/platform/sdl/main.cpp.o')]
        command[command.index('-o') + 1] = exe
        command.insert(command.index('-o'), obj)
        subprocess.run(command, cwd=build, check=True)
        traces.append(subprocess.check_output([exe]))
    assert len(traces) == 2 and traces[0] == traces[1], 'native shop trace mismatch'
    print('PASS byte-identical native shop gameplay trace (no bridge commands); '
          f'{len(json.loads(traces[0])["trace"])} shop frames, '
          f'{len(json.loads(traces[0])["input_trace"])} input frames; new shop states/last_seq are intentional')
