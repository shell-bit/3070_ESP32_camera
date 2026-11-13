"""Build the preview profile with installed Arduino ESP32 3.3.12, without global changes."""
from pathlib import Path
import argparse,shutil,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--cli',default=r'E:/application/Arduino_IDE/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe');p.add_argument('--fqbn',default='esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app');p.add_argument('--flash-size',type=int,choices=[4,8,16]);p.add_argument('--config-file');p.add_argument('--identity',action='store_true');a=p.parse_args()
flash=a.flash_size or (8 if a.identity else 4)
if a.identity and flash<8:p.error('The identity/model profile requires at least 8 MB Flash; select preview for a 4 MB board.')
if 'FlashSize=' in a.fqbn:p.error('Set flash capacity only through --flash-size.')
fqbn=a.fqbn+(',' if a.fqbn.count(':')>=3 else ':')+f'FlashSize={flash}M'
root=Path(__file__).resolve().parents[1];profile='camera_identity' if a.identity else 'camera_preview';build=root/('build-'+profile);sketch=build/profile;sketch.mkdir(parents=True,exist_ok=True)
for source in (root/'main').iterdir():
    if source.suffix in ('.h','.cpp'):shutil.copy2(source,sketch/source.name)
(sketch/(profile+'.ino')).write_text('// setup/loop are in main.cpp\n')
flags='-std=gnu++17'
partition='partitions.csv' if flash==16 else 'partitions-8mb-identity.csv' if flash==8 else 'partitions-4mb-preview.csv'
shutil.copy2(root/partition,sketch/'partitions.csv')
maximum={4:0x300000,8:0x600000,16:0xb00000}[flash]
if a.identity:
    vendor=root/'vendor'/'esp-dl-models'
    if not (vendor/'manifest.json').exists():subprocess.check_call([sys.executable,str(root/'tools'/'prepare_face_models.py')])
    for source in vendor.iterdir():
        if source.suffix in ('.hpp','.cpp'):shutil.copy2(source,sketch/source.name)
    assembly=['.section .rodata.face_models,"a"']
    for name in ['human_face_detect','human_face_feat']:
        symbol='_binary_'+name+'_espdl_start'
        assembly.extend(['.balign 16','.global '+symbol,symbol+':','.incbin "'+(vendor/(name+'.espdl')).as_posix()+'"'])
    (sketch/'face_models.S').write_text('\n'.join(assembly)+'\n')
    flags+=' -DPROJECT_FACE_ENABLED=1 -DPROJECT_UPLOAD_V2=1 -DCONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL=0 -DCONFIG_DEFAULT_HUMAN_FACE_FEAT_MODEL=0 -DCONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA=1 -DCONFIG_HUMAN_FACE_FEAT_MODEL_IN_FLASH_RODATA=1 -DCONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION=0 -DCONFIG_HUMAN_FACE_FEAT_MODEL_LOCATION=0 -DCONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1=1 -DCONFIG_FLASH_HUMAN_FACE_FEAT_MFN_S8_V1=1'
cmd=[a.cli]+(['--config-file',a.config_file] if a.config_file else [])+['compile','--fqbn',fqbn,'--build-path',str(build/'objects'),'--output-dir',str(build/'output'),'--build-property','compiler.cpp.extra_flags='+flags,'--build-property',f'upload.maximum_size={maximum}',str(sketch)]
result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
(build/'compile.log').write_text(result.stdout,encoding='utf-8')
print(result.stdout,end='')
sys.exit(result.returncode)
