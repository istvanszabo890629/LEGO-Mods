"""Build loader-free source/x86/x64 archives and verify every manifest entry.

Only reads ASI binaries as PE data. Does not load or execute them.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import sys
import zipfile

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE / 'tools'))
from generate_profiles import render as render_profiles
from generate_readme import render as render_readme, read_catalogue
from inventory_executables import PEReader

ARCHITECTURES = {'x86': 0x14c, 'x64': 0x8664}
ROOT_FILES = {'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'SUPPORTED_BUILDS.json',
              '.gitignore', '.gitattributes'}
SOURCE_FILES = {'source/build.ps1', 'source/package.py', 'source/profiles.json'}
INI_FILES = {'LegoMods.ini', 'ENGLISH_PLUS_LB1.ini', 'ENGLISH_PLUS_IJ1.ini',
             'ENGLISH_PLUS_IJ2.ini', 'ENGLISH_PLUS_TCS.ini'}
FORBIDDEN_SOURCE_SUFFIXES = {'.exe', '.dll', '.asi', '.bin', '.log', '.dmp', '.zip',
                             '.obj', '.exp', '.pdb', '.ilk', '.lib', '.pyc'}
SKIP_DIRECTORIES = {'.git', '__pycache__', '.vs', '.vscode', '.idea'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def record(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def source_files(root=ROOT):
    """Fail on unexpected public files rather than silently shipping them."""
    result = {}
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        parts = relative.parts
        if any(part in SKIP_DIRECTORIES for part in parts):
            continue
        if parts[:2] == ('source', 'build') or parts[:1] == ('dist',):
            continue
        if not path.is_file():
            continue
        require(not path.is_symlink(), f'Symlink is not a public source file: {relative}')
        require(path.suffix.lower() not in FORBIDDEN_SOURCE_SUFFIXES, f'Binary or local artifact in source tree: {relative}')
        name = relative.as_posix()
        allowed = name in ROOT_FILES | SOURCE_FILES or name == 'licenses/MinHook_LICENSE.txt'
        allowed |= name == '.github/workflows/build.yml'
        allowed |= len(parts) == 3 and parts[:2] == ('source', 'src') and path.suffix in {'.cpp', '.h', '.def', '.inc'}
        allowed |= len(parts) == 3 and parts[:2] == ('source', 'tests') and path.suffix == '.cpp'
        allowed |= len(parts) == 3 and parts[:2] == ('source', 'tools') and path.suffix == '.py'
        allowed |= len(parts) == 3 and parts[:2] == ('source', 'defaults') and path.name in INI_FILES
        allowed |= name == 'source/docs/README.md.in'
        allowed |= parts[:3] == ('source', 'third_party', 'minhook')
        require(allowed, f'Unexpected file in public tree: {relative}')
        data = path.read_bytes()
        text = data.decode('utf-8-sig')
        for private in (r'[A-Z]:\\+Users\\+[^\\\s/]+\\+', r'[A-Z]:\\+gametools\\+', r'Documents\\+Codex'):
            require(not re.search(private, text, re.IGNORECASE), f'Local workspace path in public file: {relative}')
        result[name] = data
    require(ROOT_FILES | SOURCE_FILES <= set(result), 'Missing repository entry points')
    return result


def verify_asi(content, architecture, profiles, version):
    reader = PEReader(content)
    metadata = reader.parse()
    require(metadata['machine'] == ARCHITECTURES[architecture], 'ASI architecture mismatch')
    require(metadata['pe_format'] == ('PE32+' if architecture == 'x64' else 'PE32'), 'ASI format mismatch')
    optional = struct.unpack_from('<I', content, 0x3c)[0] + 24
    protections = struct.unpack_from('<H', content, optional + 70)[0]
    require(protections & 0x40 and protections & 0x100, 'ASLR/NX protections missing')
    offset = 112 if architecture == 'x64' else 96
    export_rva, export_size = reader.unpack('<II', optional + offset)
    require(export_rva and export_size, 'ASI exports missing')
    at = reader.rva_offset(export_rva, 40)
    function_count, name_count, functions, names, ordinals = reader.unpack('<IIIII', at + 20)
    exported = []
    for index in range(name_count):
        name = reader.cstring(reader.unpack('<I', reader.rva_offset(names + 4 * index, 4))[0])
        ordinal = reader.unpack('<H', reader.rva_offset(ordinals + 2 * index, 2))[0]
        require(ordinal < function_count, 'Invalid export ordinal')
        target = reader.unpack('<I', reader.rva_offset(functions + 4 * ordinal, 4))[0]
        require(not export_rva <= target < export_rva + export_size, 'Forwarded ASI export')
        require(any(s['executable'] and s['rva'] <= target < s['rva'] + s['raw_size'] for s in reader.sections), 'Non-executable ASI export')
        exported.append(name)
    require(exported == ['InitializeASI'], 'Unexpected ASI exports')
    require(('LEGO Mods ' + version).encode('utf-16le') in content, 'ASI version mismatch')
    require('LegoMods.ini'.encode('utf-16le') in content, 'ASI settings support missing')
    for profile in profiles:
        require(bytes.fromhex(profile['sha256']) in content, 'Missing compiled native profile: ' + profile['label'])
    if architecture == 'x86':
        for name in sorted(INI_FILES - {'LegoMods.ini'}):
            require(name.encode('utf-16le') in content, 'Missing compiled text configuration: ' + name)
    return dict(machine=metadata['machine'], pe_format=metadata['pe_format'],
                aslr=True, nx_compatible=True, exports=exported, **record(content))


def manifest(contents, version, kind, architecture=None, module=None):
    result = dict(schema_version=1, module_version=version, package_type=kind,
                  external_loader_included=False,
                  files={name: record(data) for name, data in sorted(contents.items())})
    if architecture:
        result['architecture'] = architecture
        result['module'] = module
    return (json.dumps(result, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def write_zip(destination, contents):
    require('BUILD_STATUS.json' in contents, 'Missing manifest')
    for name in contents:
        parts = Path(name).parts
        require(name and not name.startswith(('/', '\\')) and '\\' not in name and '..' not in parts and ':' not in name, 'Unsafe archive member')
        require(not name.lower().endswith(('.dll', '.exe', '.bin', '.log', '.dmp')), 'External loader/game/diagnostic artifact in release')
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(contents.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    with zipfile.ZipFile(destination) as archive:
        require(archive.testzip() is None and len(archive.namelist()) == len(contents), 'ZIP verification failed')
        require(set(archive.namelist()) == set(contents), 'ZIP members differ from planned files')
        metadata = json.loads(archive.read('BUILD_STATUS.json'))
        require(set(metadata['files']) == set(contents) - {'BUILD_STATUS.json'}, 'Manifest members differ')
        for name, expected in metadata['files'].items():
            require(record(archive.read(name)) == expected, 'Manifest mismatch: ' + name)
    return dict(file=destination.name, files=len(contents), **record(destination.read_bytes()))


def package(output_dir, binary_dir=None):
    catalogue = read_catalogue(ROOT)
    version = catalogue['module_version']
    profiles = json.loads((SOURCE / 'profiles.json').read_text(encoding='utf-8'))
    require((SOURCE / 'src/generated_profiles.inc').read_text(encoding='utf-8') == render_profiles(profiles), 'Regenerate native profiles')
    require((ROOT / 'README.md').read_bytes() == render_readme(ROOT), 'Regenerate README.md')
    public = source_files()
    output_dir = output_dir.resolve()
    require(output_dir != ROOT and not SOURCE.is_relative_to(output_dir), 'Output must not replace the source tree')
    require(not output_dir.is_relative_to(ROOT) or output_dir == ROOT / 'dist', 'Use dist for in-repository archives')
    output_dir.mkdir(parents=True, exist_ok=True)
    common = {name: public[name] for name in ('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'SUPPORTED_BUILDS.json', 'licenses/MinHook_LICENSE.txt')}
    binary_dir = (binary_dir or SOURCE / 'build').resolve()
    results = []
    for architecture, machine in ARCHITECTURES.items():
        selected = [p for p in profiles if p['machine'] == machine]
        require(bool(selected), 'No native profiles for ' + architecture)
        content = (binary_dir / architecture / 'LegoMods.asi').read_bytes()
        details = verify_asi(content, architecture, selected, version)
        contents = dict(common)
        contents['LegoMods.asi'] = content
        contents['LegoMods.ini'] = public['source/defaults/LegoMods.ini']
        require('Enabled=0' in contents['LegoMods.ini'].decode('utf-8'), 'Default logging must be disabled')
        if architecture == 'x86':
            for name in sorted(INI_FILES - {'LegoMods.ini'}):
                contents['mods/' + name] = public['source/defaults/' + name]
        contents['BUILD_STATUS.json'] = manifest(contents, version, 'module', architecture, details)
        results.append(write_zip(output_dir / f'LEGO_Mods_{version}_{architecture}.zip', contents))
    public['BUILD_STATUS.json'] = manifest(public, version, 'source')
    results.append(write_zip(output_dir / f'LEGO_Mods_{version}_source.zip', public))
    checksums = ''.join(f'{item["sha256"]}  {item["file"]}\n' for item in results)
    (output_dir / 'SHA256SUMS.txt').write_text(checksums, encoding='ascii')
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist')
    parser.add_argument('--binary-dir', type=Path, help='Optional directory containing x86/x64/LegoMods.asi; defaults to source/build')
    args = parser.parse_args()
    print(json.dumps(package(args.output_dir, args.binary_dir), indent=2))
