#!/usr/bin/env python3
"""Strip and package the cross-built Windows DLL and standalone apps.
Reject unexpected third-party DLL dependencies rather than ship a broken bundle.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parent.parent
build = Path(sys.argv[1]).resolve()
prefix = Path(sys.argv[2]).resolve()
out = repo / 'dist/lh-windows-x64'
out.mkdir(parents=True, exist_ok=True)
(out / 'apps').mkdir(exist_ok=True)
files = {build / 'openvr/lighthouse/bin/win64/driver_lighthouse.dll': out / 'driver_lighthouse.dll'}
files.update({build / name: out / 'apps' / name for name in ['liblh.dll', 'lhviz.exe', 'lhtool.exe', 'lhcapture.exe']})
system = {'advapi32.dll', 'kernel32.dll', 'shlwapi.dll', 'user32.dll', 'gdi32.dll',
          'shell32.dll', 'winmm.dll', 'ole32.dll', 'comdlg32.dll', 'msvcrt.dll',
          'ucrtbase.dll', 'ntdll.dll', 'imm32.dll', 'version.dll', 'oleaut32.dll', 'setupapi.dll', 'opengl32.dll', 'hid.dll'}
artifacts = []
for source, target in files.items():
    if not source.is_file():
        sys.exit(f'Missing build artifact: {source}')
    subprocess.run(['x86_64-w64-mingw32-strip', '--strip-unneeded', '-o', str(target), str(source)], check=True)
    pe = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(target)], text=True)
    imports = re.findall(r'DLL Name:\s*(\S+)', pe)
    for name in imports:
        if name.lower() not in system and not name.lower().startswith('api-ms-win-') and not (target.parent / name).is_file() and name.lower() != 'liblh.dll':
            sys.exit(f'Unbundled Windows dependency in {target}: {name}')
        if name.lower() == 'liblh.dll' and target.parent != out / 'apps':
            sys.exit('Driver must not depend on liblh.dll')
    artifacts.append({'file': str(target.relative_to(out)), 'imports': imports,
                      'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
licenses = out / 'licenses'
licenses.mkdir(exist_ok=True)
for copyright in (prefix / 'share').glob('*/copyright'):
    shutil.copy2(copyright, licenses / (copyright.parent.name + '.txt'))
shutil.copy2(repo / 'third_party/imgui/LICENSE.txt', licenses / 'imgui.txt')
for package in ['mingw-w64-crt', 'mingw-w64-headers', 'mingw-w64-winpthreads', 'gcc']:
    notice = Path('/usr/share/licenses') / package
    if notice.is_dir():
        shutil.copytree(notice, licenses / package, dirs_exist_ok=True)
gpl = Path('/usr/share/licenses/spdx/GPL-3.0-only.txt')
if gpl.is_file():
    shutil.copy2(gpl, licenses / 'GCC-GPL-3.0.txt')
shutil.copy2(repo / 'scripts/windows-driver.ps1', out / 'Install-Driver.ps1')
(out / 'BUILD.json').write_text(json.dumps({
    'source_commit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
    'source_dirty': bool(subprocess.check_output(['git', '-C', str(repo), 'diff', '--name-only'], text=True).strip()),
    'compiler': subprocess.check_output(['x86_64-w64-mingw32-g++', '-dumpfullversion'], text=True).strip(),
    'vcpkg_baseline': json.loads((repo / 'vcpkg.json').read_text())['builtin-baseline'],
    'artifacts': artifacts,
}, indent=2) + '\n')
(out / 'README.txt').write_text('''lh Windows x64 — lighthouse tracking; headsets displayed through SteamVR's own driver

Headsets (Vive, Vive Pro, Index, Bigscreen Beyond): our driver tracks them; their
display comes from SteamVR's original driver, which the installer keeps as
driver_lighthouse.dll.stock next to ours and which ours loads at start-up.
Nothing of Valve's is in this package. Switches (environment variables, NAME=1):
  LH_STOCK_HMD=1      always load the stock display part (headset not detected)
  LH_NO_STOCK_HMD=1   never load it (tracked devices only, no headset)
  LH_NO_REBASE=1      old behaviour when the headset's main base goes out of view
  LH_STOCK_DRIVER=... path of the stock DLL, if not next to ours
  LH_STOCK_ALL_DEVICES=1  let the stock part see every lighthouse device (by default it
                      only sees the headset: otherwise wireless trackers stutter)
  LH_DARK_BY_LIGHT_TIME=1 / LH_ROOM_STEPS=1  older tracking behaviour, for comparison
Environment variables reach SteamVR only after Steam restarts. Recording a session for
a bug report: add "capture" : "C:\\\\lh-capture\\\\run1" under "driver_lighthouse" in
Steam\\config\\steamvr.vrsettings (or set LH_CAPTURE), play until it goes wrong, close
SteamVR, and send that folder plus Steam\\logs\\vrserver.txt (our lines start with "lh:").

Close SteamVR before installing. Extract the ZIP completely, then in PowerShell:
  powershell -ExecutionPolicy Bypass -File .\\Install-Driver.ps1 Install
For another Steam library add: -SteamVRPath "D:\\SteamLibrary\\steamapps\\common\\SteamVR"
Restore the original driver:
  powershell -ExecutionPolicy Bypass -File .\\Install-Driver.ps1 Restore
The installer retains driver_lighthouse.dll.stock (needed for headsets; after a
SteamVR update, install again: the new stock DLL becomes the backup).
Alternatively back up the stock DLL yourself and replace:
  SteamVR\\drivers\\lighthouse\\bin\\win64\\driver_lighthouse.dll
Keep the existing SteamVR manifest, input profiles, models and hand assets.

Standalone applications (close SteamVR before accessing USB):
  apps\\lhviz.exe
  apps\\lhtool.exe session live --rate 30
  apps\\lhcapture.exe
Keep liblh.dll beside the EXEs. The SteamVR driver itself is self-contained;
there are no Ceres, hidapi, MinGW runtime or liblh DLLs to install beside it.
Windows 10/11 x64 with the Universal CRT is the intended target.
Settings/room map: %APPDATA%\\lh\\settings.json and roommap.json.
Steam location comes from HKCU\\Software\\Valve\\Steam; driver assets can also
be located relative to the installed DLL (handles other Steam libraries).
Overrides: LH_STEAM_ROOT, LH_STEAM_CONFIG_DIR, STEAMVR_PATH, LH_HAND_POSES,
LH_REFERENCE_DB, LH_ROOM_MAP, LH_STEAM_DB. No host room/chaperone files are modified.

Validation: Windows binaries tested via Wine (unit/replay/mock host/viewer),
NOT inside native Windows SteamVR or against Windows USB hardware. The installer
has not been executed on Windows. The headset (stock display) path is untested
on Windows: it works on Linux SteamVR and Monado. Tracking, haptics and Identify (tone + LED)
are verified with the same code on Linux hardware only.
Do not infer native Windows tracking or stock-driver parity from Wine tests.
''')
archive = shutil.make_archive(str(repo / 'dist/lh-windows-x64'), 'zip', out.parent, out.name)
print(f'Packaged {archive}')
for item in artifacts:
    print(f"  {item['file']}: {(out / item['file']).stat().st_size / 1048576:.1f} MiB")
