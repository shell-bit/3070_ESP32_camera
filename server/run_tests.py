"""Run independent test modules in separate processes, using only temporary data."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parent
CHILD="""import importlib,sys,unittest
module=importlib.import_module(sys.argv[1])
suite=unittest.TestSuite()
for value in vars(module).values():
    if isinstance(value,type) and issubclass(value,unittest.TestCase) and value.__module__==module.__name__:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(value))
result=unittest.TextTestRunner(verbosity=2).run(suite)
print('TEST_COUNT='+str(result.testsRun))
sys.exit(not result.wasSuccessful())
"""

def run(log_dir):
    log_dir=Path(log_dir)
    log_dir.mkdir(parents=True,exist_ok=True)
    results=[]
    for path in sorted(ROOT.glob('test_*.py')):
        started=time.monotonic()
        with tempfile.TemporaryDirectory(prefix='five-frame-suite-') as data:
            env=os.environ.copy()
            env['CAMERA_DATA_DIR']=data
            env.pop('GOOGLE_APPLICATION_CREDENTIALS',None)
            env.pop('DASHBOARD_PUBLIC_URL',None)
            try:
                completed=subprocess.run([sys.executable,'-c',CHILD,path.stem],cwd=ROOT,
                    env=env,capture_output=True,text=True,timeout=240)
                output=completed.stdout+completed.stderr
                count=next((int(line.split('=',1)[1]) for line in completed.stdout.splitlines() if line.startswith('TEST_COUNT=')),0)
                item={'suite':path.stem,'tests':count,'returncode':completed.returncode}
            except subprocess.TimeoutExpired as error:
                output=((error.stdout or b'')+(error.stderr or b'')).decode(errors='replace')
                item={'suite':path.stem,'tests':0,'returncode':'timeout'}
        (log_dir/(path.stem+'.log')).write_text(output,encoding='utf-8')
        item['seconds']=round(time.monotonic()-started,2)
        results.append(item)
        (log_dir/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
        print(json.dumps(item),flush=True)
    return 0 if all(item['returncode']==0 and item['tests']>0 for item in results) else 1

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir',required=True,help='Directory for per-suite logs')
    sys.exit(run(parser.parse_args().log_dir))
