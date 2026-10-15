# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""An installed build's files in the XDG Base Directory folders: the game's settings in the config folder, its log and
threads.txt in the state folder, the pipeline cache and the extractions in the cache folder, and what an older build
left in the data folder moved there. A game in a folder of its own keeps everything beside it. No GPU: the game stops
before it, its ExeFS taken away."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from nx_runtime_smoke import make_nro


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    driver = str(args.driver.resolve())
    failures = []

    with tempfile.TemporaryDirectory(prefix='scarletnx-xdg-') as temporary:
        root = Path(temporary)
        env = {k: v for k, v in os.environ.items() if k not in ('SNX_GAME', 'APPIMAGE', 'APPDIR')}
        homes = {kind: root / kind.lower() for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE')}
        for kind, path in homes.items():
            env[f'XDG_{kind}_HOME'] = str(path)
        env['HOME'] = str(root / 'home')
        data, config, state, cache = (homes[kind] / 'scarletnx' for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE'))
        games = data / 'games'

        def run(command, cwd=root):
            p = subprocess.run(
                command, check=False, cwd=cwd, env=env, capture_output=True, text=True, timeout=args.timeout
            )
            return p.returncode, p.stdout + p.stderr

        def check(condition, what, output=''):
            if not condition:
                failures.append(f'{what}\n' + ''.join(f'    {line}\n' for line in output.splitlines()[-30:]))

        # what an older build left beside the game and in work/
        (games / 'shader_cache').mkdir(parents=True)
        (games / 'Smoke.ini').write_text('[Display]\nlanguage=1\n; kept from the older build\n')
        (games / 'Smoke.log').write_text('older log\n')
        (games / 'threads.txt').write_text('older threads\n')
        (games / 'shader_cache/vk_pipelines.bin').write_bytes(b'pipelines')
        (data / 'work/Old/exefs').mkdir(parents=True)
        (data / 'work/Old/exefs/main').write_bytes(b'code')
        nro = root / 'smoke.nro'
        nro.write_bytes(make_nro())

        # recompiled into the data folder (--repo names it, as the menu does): the settings in the config folder
        status, out = run([driver, '--repo', str(data), '-r', str(nro), 'Smoke', '--runtime', driver, '--jobs', '2'])
        check(status == 0 and (games / 'Smoke').is_file(), 'the recompile did not put Smoke in games/', out)
        ini = config / 'games/Smoke.ini'
        check(
            ini.is_file() and 'kept from the older build' in ini.read_text() and not (games / 'Smoke.ini').exists(),
            f'the older Smoke.ini did not move to {ini}',
            out,
        )
        check(f'Moved {games / "Smoke.ini"} to {ini}' in out, 'the recompile did not say it moved Smoke.ini', out)

        # the game, started in games/ as ./Smoke, logs in the state folder and stops before the GPU without its ExeFS
        shutil.rmtree(games / 'Smoke_data/exefs')
        status, out = run(['./Smoke', '-H'], cwd=games)
        log = state / 'games/Smoke.log'
        text = log.read_text(errors='replace') if log.is_file() else ''
        check(
            status != 0 and 'ScarletNX - Smoke' in text and 'Could not load the game executables' in text,
            f'the game did not log to {log} and stop before the GPU (exit {status})',
            out,
        )
        check(f'moved {games / "Smoke.log"} to {log}' in text, 'the log does not say the older one moved', text)
        check(not (games / 'Smoke.log').exists(), 'a log is left in games/', out)
        threads = state / 'games/threads.txt'
        check(
            threads.is_file() and threads.read_text() == 'older threads\n' and not (games / 'threads.txt').exists(),
            f'threads.txt did not move to {threads}',
            text,
        )
        pipelines = cache / 'games/shader_cache/vk_pipelines.bin'
        check(
            pipelines.is_file() and pipelines.read_bytes() == b'pipelines' and not (games / 'shader_cache').exists(),
            f'the pipeline cache did not move to {pipelines}',
            text,
        )
        check(ini.is_file() and not (games / 'Smoke.ini').exists(), 'the game wrote its settings in games/', text)
        check(
            all((games / name).exists() for name in ('Smoke.abi', 'Smoke_code.so', 'Smoke_data')),
            'the game itself left games/',
        )

        # an extraction: the older build's work/ moved to the cache folder. hactool finds no program in an empty PFS0
        (root / 'empty.nsp').write_bytes(b'PFS0' + bytes(12))
        status, out = run([driver, '--repo', str(data), '-x', str(root / 'empty.nsp')])
        moved = cache / 'work/Old/exefs/main'
        check(
            moved.is_file() and moved.read_bytes() == b'code' and not (data / 'work').exists(),
            f'work/ did not move to {cache / "work"}',
            out,
        )
        check(f'Moved {data / "work/Old"} to {cache / "work/Old"}' in out, 'the extraction did not say what moved', out)

        # a folder of its own (a checkout's or a portable copy's games/, --outdir): everything beside the game
        beside = root / 'portable/games'
        status, out = run(
            [driver, '--repo', str(data), '-r', str(nro), 'Beside', '--runtime', driver, '--jobs', '2']
            + ['--outdir', str(beside)]
        )
        check(status == 0 and (beside / 'Beside.ini').is_file(), 'the --outdir recompile did not keep Beside.ini', out)
        shutil.rmtree(beside / 'Beside_data/exefs')
        status, out = run(['./Beside', '-H'], cwd=beside)
        check((beside / 'Beside.log').is_file(), 'the game in a folder of its own did not log beside it', out)
        check(
            not (config / 'games/Beside.ini').exists() and not (state / 'games/Beside.log').exists(),
            'the game in a folder of its own wrote in the XDG folders',
        )
        check(not (root / 'home').exists(), 'something was written in HOME')

    if failures:
        print('XDG layout:\n' + ''.join(failures), file=sys.stderr)
        return 1
    print(
        'XDG layout: settings in config, log and threads.txt in state, pipeline cache and work/ in cache, older '
        'files moved; a folder of its own keeps them beside the game'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
