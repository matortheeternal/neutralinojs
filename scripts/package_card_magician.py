"""Package the fork's supported targets and record their exact source revision."""
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile

source = Path(sys.argv[1])
targets = [
    'linux_x64', 'linux_arm64', 'linux_armhf', 'win_x64.exe',
    'mac_x64', 'mac_arm64', 'mac_universal',
]
metadata = {
    'repository': 'matortheeternal/neutralinojs',
    'tag': os.environ['RUNTIME_TAG'],
    'revision': os.environ['RUNTIME_COMMIT'],
    'binaries': {},
}
for target in targets:
    name = f'neutralino-{target}'
    metadata['binaries'][name] = hashlib.sha256((source / name).read_bytes()).hexdigest()

output = Path('dist-cm')
output.mkdir(exist_ok=True)
manifest = json.dumps(metadata, indent=2) + '\n'
(output / 'runtime.json').write_text(manifest)
archive = output / f'neutralinojs-{metadata["tag"]}.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
    for name in metadata['binaries']:
        info = zipfile.ZipInfo(name)
        info.external_attr = 0o100755 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        bundle.writestr(info, (source / name).read_bytes())
    bundle.writestr('runtime.json', manifest)
checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
(output / 'SHA256SUMS').write_text(f'{checksum}  {archive.name}\n')
