"""Fetch pinned Espressif wrappers/models; preserve source and checksums, pack PDL2."""
from pathlib import Path
import hashlib,json,subprocess,sys,urllib.request
COMMIT='512ed0b0dc5f5320f3d9ca5a5aea189803f87f31'  # ESP-DL v3.3.15
BASE=f'https://raw.githubusercontent.com/espressif/esp-dl/{COMMIT}/'
root=Path(__file__).resolve().parents[1];dest=root/'vendor'/'esp-dl-models';dest.mkdir(parents=True,exist_ok=True)
paths=['LICENSE','esp-dl/fbs_loader/pack_espdl_models.py']
for name in ['human_face_detect','human_face_recognition']:
    paths.extend([f'models/{name}/{name}.hpp',f'models/{name}/{name}.cpp'])
paths.extend(['models/human_face_detect/models/s3/human_face_detect_msr_s8_v1.espdl','models/human_face_detect/models/s3/human_face_detect_mnp_s8_v1.espdl','models/human_face_recognition/models/s3/human_face_feat_mfn_s8_v1.espdl'])
manifest=[]
for path in paths:
    target=dest/Path(path).name
    if not target.exists():urllib.request.urlretrieve(BASE+path,target)
    manifest.append({'path':target.name,'source':BASE+path,'sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
(dest/'manifest.json').write_text(json.dumps({'commit':COMMIT,'files':manifest},indent=2)+'\n')
for target,sources in [('human_face_detect',['human_face_detect_msr_s8_v1.espdl','human_face_detect_mnp_s8_v1.espdl']),('human_face_feat',['human_face_feat_mfn_s8_v1.espdl'])]:
    subprocess.check_call([sys.executable,str(dest/'pack_espdl_models.py'),'-f','PDL2','-m',*[str(dest/s) for s in sources],'-o',str(dest/(target+'.espdl'))])
print('Pinned face model sources and 1.5 MB of model data prepared:',dest)
