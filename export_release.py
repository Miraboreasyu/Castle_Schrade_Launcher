"""Create the self-contained release archive from a local successful build."""
from pathlib import Path
import csv
import hashlib
import io
import json
import re
import zipfile

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'dist'


def main():
    ini = (ROOT / 'auto_quest_start.ini').read_bytes()
    keys = re.findall(r'^([a-z_]+)\s*=', ini.decode('utf-8-sig'), re.M)
    if keys != ['enabled', 'save_slot', 'quest_id']:
        raise ValueError(f'Unexpected public settings: {keys}')
    quests = (ROOT / 'quest_ids.csv').read_bytes()
    rows = list(csv.reader(io.StringIO(quests.decode('utf-8-sig'))))
    if len(rows) != 513 or len({row[0] for row in rows[1:]}) != 512:
        raise ValueError('Expected 512 unique quests')
    readme = (ROOT / 'CONFIGURATION.md').read_text(encoding='utf-8')
    readme += '\n\n---\n\n' + (ROOT / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
    readme += '\n\n' + (ROOT / 'LICENSE').read_text(encoding='utf-8')
    files = {
        'nativePC/plugins/auto_quest_start.dll': (ROOT / 'auto_quest_start.dll').read_bytes(),
        'nativePC/plugins/auto_quest_start.ini': ini,
        'README.md': readme.encode('utf-8'),
        'quest_ids.csv': quests,
    }
    OUT.mkdir(exist_ok=True)
    archive = OUT / 'Castle_Schrade_Launcher-15.23.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in files.items():
            z.writestr(name, data)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None and z.namelist() == list(files)
        for name, data in files.items():
            assert z.read(name) == data
    manifest = {
        'archive': archive.name,
        'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'files': {n: hashlib.sha256(b).hexdigest() for n, b in files.items()},
        'runtime_regression_test': 'required after source cleanup',
    }
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
