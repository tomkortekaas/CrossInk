#!/usr/bin/env python3
"""Read-only verification of recorded deployment identity."""
import hashlib, json, pathlib, subprocess, sys

def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()

def check(file):
    state = json.loads(pathlib.Path(file).read_text())
    repo = pathlib.Path(state['repository']).resolve(strict=True)
    active = pathlib.Path(state['active_reference']).resolve(strict=True)
    if active != repo:
        raise ValueError(f'Active reference resolves to {active}, expected {repo}')
    if pathlib.Path(git(repo, 'rev-parse', '--show-toplevel')).resolve() != repo:
        raise ValueError('Recorded repository is not a repository root')
    subprocess.run(['git', '-C', str(repo), 'merge-base', '--is-ancestor', state['installed_source'], 'HEAD'], check=True)
    for relative, commit in state['dependencies'].items():
        if git(repo / relative, 'rev-parse', 'HEAD') != commit:
            raise ValueError(f'Dependency differs: {relative}')
    h = hashlib.sha256()
    with open(state['artifact'], 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    if h.hexdigest() != state['artifact_sha256']:
        raise ValueError('Recorded artifact hash differs')
    if state['status'] != 'installed_verified':
        raise ValueError('No verified installation recorded')
    print('PASS: active source, installed ancestry, dependencies and artifact identity')

if __name__ == '__main__':
    try:
        check(sys.argv[1])
    except (OSError, ValueError, KeyError, IndexError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
