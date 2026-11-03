"""Generate the English GitHub README from the public support catalogue."""
from pathlib import Path
import argparse
import json
import re

ROOT = Path(__file__).resolve().parents[2]


def read_catalogue(root=ROOT):
    data = json.loads((root / 'SUPPORTED_BUILDS.json').read_text(encoding='utf-8'))
    profiles = json.loads((root / 'source/profiles.json').read_text(encoding='utf-8'))
    assert data['hash_algorithm'] == 'SHA-256'
    assert data['unique_profiles'] == len(profiles)
    assert len({p['sha256'] for p in profiles}) == len(profiles)
    assert {row['sha256'] for row in data['editions']} == {p['sha256'] for p in profiles}
    identities = set()
    for row in data['editions']:
        identity = (row['game'], row['store'], row['architecture'], row['renderer'],
                    row['original_filename'], row['sha256'])
        assert identity not in identities
        identities.add(identity)
        assert row['architecture'] in ('x86', 'x64')
        assert row['status'] in ('tested', 'base_game_issue')
        assert re.fullmatch('[0-9a-f]{64}', row['sha256'])
        for value in (row['game'], row['store'], row['original_filename']):
            assert value and not any(c in value for c in ('|', '`', '\n', '\r'))
        assert not any(c in (row['renderer'] or '') for c in ('|', '`', '\n', '\r'))
    return data


def render(root=ROOT):
    data = read_catalogue(root)
    table = ['| Game / edition | Original filename | SHA-256 | Status |', '|---|---|---|---|']
    for row in data['editions']:
        bits = '32-bit' if row['architecture'] == 'x86' else '64-bit'
        edition = f'{row["store"]}, {bits}'
        if row['renderer']:
            edition += ', ' + row['renderer']
        label = f'{row["game"]} ({edition})'
        status = 'Tested' if row['status'] == 'tested' else 'Does not work'
        table.append(f'| {label} | `{row["original_filename"]}` | `{row["sha256"]}` | {status} |')
    template = (root / 'source/docs/README.md.in').read_text(encoding='utf-8')
    assert template.count('@VERSION@') == 1 and template.count('@BUILD_CATALOGUE@') == 1
    return template.replace('@VERSION@', data['module_version']).replace('@BUILD_CATALOGUE@', '\n'.join(table)).encode('utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    expected = render()
    destination = ROOT / 'README.md'
    if args.check:
        assert destination.read_bytes() == expected, 'Regenerate README.md'
    else:
        destination.write_bytes(expected)
    print(f'English README: {len(read_catalogue()["editions"])} editions with original filenames and full hashes.')
