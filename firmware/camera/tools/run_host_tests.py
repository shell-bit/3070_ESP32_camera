from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parents[1];out=root/'build-host-tests';out.mkdir(exist_ok=True)
flags=['-std=c++17','-Wall','-Wextra','-Werror']
if os.name=='nt':flags+=['-static','-static-libgcc','-static-libstdc++']
for name,source,extra in [('core','project_core.cpp',[]),('camera_manager','project_camera_manager.cpp',['-I'+str(root/'tests/fakes')])]:
    executable=out/(name+('.exe' if os.name=='nt' else ''))
    subprocess.check_call(['g++',*flags,*extra,str(root/'tests'/('test_'+name+'.cpp')),str(root/'main'/source),'-o',str(executable)])
    subprocess.check_call([str(executable)])
executable=out/('identity_json.exe' if os.name=='nt' else 'identity_json')
subprocess.check_call(['g++',*flags,'-I'+str(root/'vendor/cjson'),str(root/'tests/test_identity_json.cpp'),str(root/'main/project_identity_json.cpp'),str(root/'vendor/cjson/cJSON.c'),'-o',str(executable)])
result=subprocess.check_output([str(executable)],text=True)
assert len(result.splitlines())==6
print('identity serializer: unknown, matched, timeout, error, not_run and no_faces checks passed')
