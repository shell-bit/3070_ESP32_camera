import os
import tempfile
import unittest
from pathlib import Path
import cv2
import numpy as np
from fastapi.testclient import TestClient

# Isolated test data: never open or delete deployment photos.
temporary = tempfile.TemporaryDirectory(prefix='ai-test-',dir=Path(__file__).resolve().parent)
os.environ['CAMERA_DATA_DIR']=temporary.name
os.environ['UPLOAD_TOKEN']=''
import store
import main
import worker

class FakeModels:
    def __init__(self,person=True,verdict='suspicious',fail=False):
        self.person=person; self.verdict=verdict; self.fail=fail; self.calls=0
    def has_person(self,path):
        assert path.is_file()
        return self.person
    def assess(self,path):
        self.calls+=1
        if self.fail: raise RuntimeError('Simulated model failure')
        return self.verdict,'Visible test action'

class PipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.viewer=main.app.state.accounts.create('pipeline-viewer','Pipeline-test-password!','viewer')

    def setUp(self):
        import hashlib
        with store.db() as db:
            for table in ('jobs','events','push_events','push_deliveries','devices'):db.execute('DELETE FROM '+table)
            db.execute('INSERT INTO devices VALUES(?,?,?)',('test',self.viewer,hashlib.sha256(b'pipeline-device-token').hexdigest()))
        for folder in (store.INCOMING,store.PHOTOS):
            for p in folder.iterdir(): p.unlink()
        self.client=TestClient(main.app,base_url='https://testserver')
        response=self.client.post('/api/auth/login',headers={'X-Sentinel-Request':'1'},json={'username':'pipeline-viewer','password':'Pipeline-test-password!'})
        self.assertEqual(response.status_code,200,response.text)
        self.data=cv2.imencode('.jpg',np.zeros((24,32,3),dtype=np.uint8))[1].tobytes()
        self.headers={'Content-Type':'image/jpeg','Authorization':'Bearer pipeline-device-token','X-Device-ID':'test','X-Event-ID':'burst-1','X-Photo-Index':'1','X-Expected-Photos':'1'}
    def upload(self,index='1'):
        r=self.client.post('/api/device/upload',headers={**self.headers,'X-Event-ID':'burst-'+index},content=self.data)
        self.assertEqual(r.status_code,201,r.text)
        return r.json()['job_id']
    def test_no_person_retained_for_review_and_retry_deduplicated(self):
        key=self.upload(); model=FakeModels(person=False)
        self.assertTrue(worker.process_one(model)); self.assertEqual(model.calls,0)
        self.assertTrue((store.INCOMING/(key+'.jpg')).exists())
        self.assertEqual(self.upload(),key)
        self.assertFalse(worker.process_one(model))
        self.assertEqual(self.client.get('/api/photos').json(),[])
    def test_normal_deleted(self):
        key=self.upload(); worker.process_one(FakeModels(verdict='normal'))
        self.assertFalse((store.INCOMING/(key+'.jpg')).exists())
        self.assertEqual(self.client.get('/api/analysis/history').json()['items'],[])
    def test_alert_saved_and_served_with_report(self):
        key=self.upload(); worker.process_one(FakeModels())
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(report['status'],'suspicious')
        self.assertEqual(report['description'],'Visible test action')
        self.assertTrue(report['timestamp'])
        self.assertEqual(self.client.get(report['photo_url']).content,self.data)
        self.assertEqual(len(self.client.get('/api/analysis/history').json()['items']),1)
        self.assertEqual(len(self.client.get('/api/photos').json()),1)
    def test_uncertain_and_error_retained_privately(self):
        for index,model in [('1',FakeModels(verdict='uncertain')),('2',FakeModels(fail=True))]:
            key=self.upload(index); worker.process_one(model)
            self.assertTrue((store.INCOMING/(key+'.jpg')).is_file())
            self.assertEqual(self.client.get('/photos/'+key+'.jpg').status_code,404)
        self.assertEqual(self.client.get('/api/health').json()['jobs']['review'],2)
    def test_restart_and_pagination(self):
        self.upload()
        with store.db() as db:
            db.execute("UPDATE jobs SET state='processing'")
            db.execute("UPDATE events SET state='processing'")
        store.recover(); worker.process_one(FakeModels())
        self.upload('2'); worker.process_one(FakeModels())
        first=self.client.get('/api/analysis/history?limit=1').json()
        second=self.client.get('/api/analysis/history?limit=1&before='+first['next_cursor']).json()
        self.assertEqual(len(second['items']),1)
        self.assertNotEqual(first['items'][0]['id'],second['items'][0]['id'])
    def test_invalid_input_and_conflicting_retry(self):
        self.assertEqual(self.client.post('/api/device/upload',headers=self.headers,content=b'bad').status_code,422)
        self.upload()
        other=cv2.imencode('.jpg',np.full((24,32,3),255,dtype=np.uint8))[1].tobytes()
        self.assertEqual(self.client.post('/api/device/upload',headers=self.headers,content=other).status_code,409)
    def test_strict_qwen_output(self):
        self.assertEqual(worker.parse_answer('{"verdict":"normal","description":"Walking"}')[0],'normal')
        for answer in ['Normal', '{"verdict":"safe","description":"ok"}', '{"verdict":false}']:
            with self.assertRaises((ValueError,TypeError)): worker.parse_answer(answer)

if __name__=='__main__':
    try: unittest.main()
    finally: temporary.cleanup()
