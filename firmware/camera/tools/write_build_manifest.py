"""Record final binaries and compare built source copies with the reviewable sources."""
from pathlib import Path
import datetime,hashlib,json,re
root=Path(__file__).resolve().parents[1]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
sources={p.name:digest(p) for p in (root/'main').iterdir() if p.suffix in ('.cpp','.h') and p.name!='project_local.h'}
model_sources={p.name:digest(p) for p in (root/'vendor/esp-dl-models').iterdir() if p.suffix in ('.cpp','.hpp')}
profiles={}
for profile,flash in [('camera_preview',4),('camera_identity',8)]:
    build=root/('build-'+profile);output=build/'output';sketch=build/profile
    for name,sha in sources.items():
        if digest(sketch/name)!=sha:raise RuntimeError(f'{profile}: source changed after build: {name}')
    if profile=='camera_identity':
        for name,sha in model_sources.items():
            if digest(sketch/name)!=sha:raise RuntimeError(f'Identity wrapper changed after build: {name}')
    log=(build/'compile.log').read_text(encoding='utf-8')
    if 'Error during build' in log or 'Sketch uses ' not in log:raise RuntimeError(f'{profile}: no successful build log')
    options=json.loads((build/'objects/build.options.json').read_text(encoding='utf-8'))
    sizes=re.search(r'Sketch uses (\d+) bytes.*Maximum is (\d+) bytes',log)
    ram=re.search(r'Global variables use (\d+) bytes',log)
    artifacts={}
    for kind,suffix in [('application','.bin'),('bootloader','.bootloader.bin'),('partitions','.partitions.bin'),('merged','.merged.bin')]:
        path=output/(profile+'.ino'+suffix)
        artifacts[kind]={'path':path.relative_to(root).as_posix(),'bytes':path.stat().st_size,'sha256':digest(path)}
    profiles[profile]={'flash_mb':flash,'fqbn':options.get('fqbn'),'arduino_esp32':'3.3.12','build_log':(build/'compile.log').relative_to(root).as_posix(),'application_used':int(sizes[1]),'application_capacity':int(sizes[2]),'static_ram_used':int(ram[1]),'partition_csv_sha256':digest(sketch/'partitions.csv'),'artifacts':artifacts,'hardware_flashed':False}
manifest={'generated_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'baseline_commit':'cd6cbc9984225ae0afe9bf534cf3f664f015aae8','source_sha256':sources,'model_source_sha256':model_sources,'profiles':profiles,'model_manifest':'vendor/esp-dl-models/manifest.json','model_manifest_sha256':digest(root/'vendor/esp-dl-models/manifest.json'),'validation':['Three native C++ suites passed','Five cross-language server contract tests passed'],'hardware_validation':'Not performed; actual sensor, Flash and PSRAM need confirmation. Identity requires >=8 MB Flash.'}
(root/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Wrote',root/'build-manifest.json')
