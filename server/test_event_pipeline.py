"""Server event pipeline tests: synthetic JPEGs, isolated DB, no GPU/network sender."""
import hashlib
import json
import logging
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import cv2
import numpy as np
from fastapi.testclient import TestClient

temporary = tempfile.TemporaryDirectory(prefix='event-api-tests-')
os.environ['CAMERA_DATA_DIR'] = temporary.name
os.environ['EVENT_WAIT_SECONDS'] = '30'
os.environ.pop('GOOGLE_APPLICATION_CREDENTIALS', None)
import main
import store
import worker
logging.disable(logging.CRITICAL)

class FakeModels:
    def __init__(self, verdict='suspicious', subtype='forced_entry', people=None, evidence=None):
        self.verdict = verdict
        self.subtype = subtype if verdict == 'suspicious' else None
        self.people = people
        self.evidence = [2, 3] if evidence is None else evidence
        self.calls = []
        self.detections = []
    def has_person(self, path):
        self.detections.append(path)
        return True if self.people is None else self.people[len(self.detections)-1]
    def assess_event(self, paths, frames):
        self.calls.append((paths, frames))
        return self.verdict, 'Visible actions across the event.', self.subtype, self.evidence
    def assess(self, path):
        raise AssertionError('Five-frame events must not use single-image inference')

class EventPipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.accounts = main.app.state.accounts
        cls.viewer = cls.accounts.create('event-viewer', 'Event-viewer-password!', 'viewer')
        cls.other = cls.accounts.create('event-other', 'Event-other-password!', 'viewer')
        cls.admin = cls.accounts.create('event-admin', 'Event-admin-password!', 'admin')
        cls.client = TestClient(main.app, base_url='https://testserver')
        cls.other_client = TestClient(main.app, base_url='https://testserver')
        cls.admin_client = TestClient(main.app, base_url='https://testserver')
        for client, name, password in [
            (cls.client,'event-viewer','Event-viewer-password!'),
            (cls.other_client,'event-other','Event-other-password!'),
            (cls.admin_client,'event-admin','Event-admin-password!')]:
            r=client.post('/api/auth/login',headers={'X-Sentinel-Request':'1'},json={'username':name,'password':password})
            assert r.status_code == 200, r.text
        cls.csrf = {'X-CSRF-Token':cls.client.get('/api/auth/me').json()['csrf'],'Origin':'https://testserver'}
        cls.admin_csrf = {'X-CSRF-Token':cls.admin_client.get('/api/auth/me').json()['csrf'],'Origin':'https://testserver'}
        cls.jpeg = cv2.imencode('.jpg', np.full((24,32,3),90,dtype=np.uint8))[1].tobytes()
        cls.token = 'event-test-device-token'
    def setUp(self):
        with store.db() as c:
            for name in ('push_deliveries','push_events','jobs','events','devices','manual_decisions'):
                if c.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",(name,)).fetchone():
                    c.execute('DELETE FROM '+name)
            c.execute('INSERT INTO devices VALUES(?,?,?)',('camera',self.viewer,hashlib.sha256(self.token.encode()).hexdigest()))
        for folder in (store.INCOMING,store.PHOTOS):
            for path in folder.glob('*.jpg'): path.unlink()
        with self.accounts.db() as c:c.execute('DELETE FROM push_installations')
    def upload(self,index,event='burst',expected=None,extra=None,data=None):
        headers={'Content-Type':'image/jpeg','Authorization':'Bearer '+self.token,
                 'X-Device-ID':'camera','X-Event-ID':event,'X-Photo-Index':str(index)}
        if expected is not None:headers['X-Expected-Photos']=str(expected)
        headers.update(extra or {})
        return self.client.post('/api/device/upload',headers=headers,content=self.jpeg if data is None else data)
    def full(self,event='burst',order=(1,2,3,4,5)):
        items={}
        for index in order:
            r=self.upload(index,event)
            self.assertEqual(r.status_code,201,r.text)
            items[index]=r.json()
        return items
    def alarm(self,event='burst',**params):
        return self.client.get('/api/device/event-status',params={'device_id':'camera','event_id':event,**params},
                               headers={'Authorization':'Bearer '+self.token})
    def test_collects_without_early_analysis_and_orders_one_joint_call(self):
        uploaded={}
        for index in (5,2,1,4):
            r=self.upload(index)
            self.assertEqual(r.status_code,201,r.text)
            self.assertEqual(r.json()['analysis_status'],'collecting')
            uploaded[index]=r.json()
        model=FakeModels(people=[False,False,True,False,False])
        self.assertFalse(worker.process_one(model))
        self.assertEqual(model.detections,[])
        self.assertEqual(self.alarm().text,'pending')
        uploaded[3]=self.upload(3).json()
        self.assertEqual(uploaded[3]['analysis_status'],'queued')
        self.assertTrue(worker.process_one(model))
        self.assertEqual(len(model.calls),1)
        self.assertEqual(len(model.detections),5)
        paths,frames=model.calls[0]
        self.assertEqual([p.stem for p in paths],[uploaded[i]['job_id'] for i in range(1,6)])
        self.assertEqual([f['photo_index'] for f in frames],[1,2,3,4,5])
        self.assertFalse(worker.process_one(model))
        self.assertEqual(self.alarm().text,'suspicious')
        history=self.client.get('/api/analysis/history').json()['items']
        self.assertEqual(len(history),1)
        report=history[0]
        self.assertEqual(report['frame_count'],5)
        self.assertEqual(report['activity_type'],'forced_entry')
        self.assertEqual(report['evidence_frame_indices'],[2,3])
        self.assertTrue(report['input_complete'])
        self.assertEqual(self.client.get('/api/analysis/latest').json()['id'],report['id'])
        self.assertEqual(self.client.get('/api/analysis/report/'+report['id']).json(),report)
        for frame in report['frames']:
            self.assertEqual(self.client.get(frame['photo_url']).content,self.jpeg)
        with store.db() as c:self.assertEqual(c.execute('SELECT count(*) FROM push_events').fetchone()[0],1)
    def test_duplicate_conflict_metadata_and_closed_late_frames(self):
        first=self.upload(1).json()
        self.assertTrue(self.upload(1).json()['duplicate'])
        other=cv2.imencode('.jpg',np.zeros((24,32,3),dtype=np.uint8))[1].tobytes()
        self.assertEqual(self.upload(1,data=other).status_code,409)
        self.assertEqual(self.upload(1,expected=1).status_code,409)
        self.assertEqual(self.upload(2,expected=4).status_code,409)
        with store.db() as c:c.execute('UPDATE events SET deadline=0')
        worker.process_one(FakeModels())
        self.assertEqual(self.upload(2).status_code,409)
        self.assertTrue(self.upload(1).json()['duplicate'])
        self.assertEqual(self.alarm().text,'review')
    def test_missing_frames_private_review_and_cannot_retry_incomplete(self):
        one=self.upload(1).json()
        with store.db() as c:c.execute('UPDATE events SET deadline=0')
        model=FakeModels()
        worker.process_one(model)
        self.assertEqual(model.calls,[])
        items=self.client.get('/api/review').json()['items']
        self.assertEqual(len(items),1)
        event=items[0]
        self.assertEqual(event['missing_indices'],[2,3,4,5])
        self.assertFalse(event['input_complete'])
        self.assertEqual(self.client.get(event['frames'][0]['photo_url']).content,self.jpeg)
        self.assertEqual(self.client.get('/photos/'+one['job_id']+'.jpg').status_code,404)
        self.assertEqual(self.client.post('/api/review/'+event['id'],headers=self.csrf,
            json={'action':'retry','description':'Retry'}).status_code,409)
        keep=self.client.post('/api/review/'+event['id'],headers=self.csrf,
            json={'action':'keep','description':'Visible damage confirmed'})
        self.assertEqual(keep.status_code,200,keep.text)
        self.assertEqual(self.alarm().text,'suspicious')
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(report['activity_type'],'suspicious')
        self.assertFalse(report['input_complete'])
    def test_normal_deletes_all_and_generic_suspicious_still_alerts(self):
        self.full()
        worker.process_one(FakeModels('normal',evidence=[]))
        self.assertEqual(self.alarm().text,'normal')
        self.assertEqual(list(store.INCOMING.glob('*.jpg')),[])
        self.assertEqual(list(store.PHOTOS.glob('*.jpg')),[])
        self.assertEqual(self.client.get('/api/analysis/history').json()['items'],[])
        self.full('second')
        worker.process_one(FakeModels(subtype='property_damage'))
        self.assertEqual(self.client.get('/api/analysis/latest').json()['activity_type'],'suspicious')
    def test_no_yolo_or_uncertain_retains_whole_group(self):
        for event,model in [('no-person',FakeModels(people=[False]*5)),('uncertain',FakeModels('uncertain',evidence=[]))]:
            self.full(event)
            worker.process_one(model)
            self.assertEqual(self.alarm(event).text,'review')
        self.assertEqual(len(list(store.INCOMING.glob('*.jpg'))),10)
        self.assertEqual(len(self.client.get('/api/review').json()['items']),2)
        self.assertEqual(self.client.get('/api/photos').json(),[])
    def test_cross_owner_event_report_photos_review_and_device_state(self):
        self.full()
        worker.process_one(FakeModels())
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(self.other_client.get('/api/analysis/report/'+report['id']).status_code,404)
        self.assertEqual(self.other_client.get(report['frames'][0]['photo_url']).status_code,404)
        self.assertEqual(self.other_client.get('/api/analysis/history').json()['items'],[])
        self.full('review')
        worker.process_one(FakeModels('uncertain',evidence=[]))
        review=self.client.get('/api/review').json()['items'][0]
        self.assertEqual(self.other_client.get(review['frames'][0]['photo_url']).status_code,404)
        self.assertEqual(self.other_client.get('/api/review').json()['items'],[])
        wrong=self.other_client.get('/api/auth/me').json()['csrf']
        self.assertEqual(self.other_client.post('/api/review/'+review['id'],
            headers={'X-CSRF-Token':wrong,'Origin':'https://testserver'},
            json={'action':'dismiss','description':'not mine'}).status_code,404)
    def test_expected_count_and_capture_time_validation(self):
        for value in ('0','6','bad'):
            self.assertEqual(self.upload(1,expected=value).status_code,400)
        self.assertEqual(self.upload(2,expected=1).status_code,400)
        self.assertEqual(self.upload(1,extra={'X-Captured-At':'not-a-time'}).status_code,400)
        self.assertEqual(self.upload(1,extra={'X-Captured-At':'2026-11-09T10:00:00'}).status_code,400)
        r=self.upload(1,expected=1,extra={'X-Captured-At':'2026-11-09T10:00:00+00:00'})
        self.assertEqual(r.status_code,201,r.text)
        worker.process_one(FakeModels(evidence=[1]))
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(report['frame_count'],1)
        self.assertEqual(report['frames'][0]['captured_at'],'2026-11-09T10:00:00+00:00')
        self.assertEqual(self.alarm().text,'suspicious')
    def test_reassignment_cannot_join_old_event(self):
        self.upload(1)
        r=self.admin_client.post('/api/admin/devices',headers=self.admin_csrf,json={'device_id':'camera','owner_id':self.other})
        self.assertEqual(r.status_code,200,r.text)
        previous=self.token
        self.token=r.json()['upload_token']
        self.assertEqual(self.upload(2).status_code,409)
        self.assertEqual(self.alarm().text,'not_found')
        self.assertEqual(self.upload(1,'new-owner-event').status_code,201)
        self.token=previous
    def test_invalid_evidence_or_model_error_preserves_frames(self):
        self.full()
        worker.process_one(FakeModels(evidence=[6]))
        self.assertEqual(self.alarm().text,'review')
        self.assertEqual(len(list(store.INCOMING.glob('*.jpg'))),5)
        self.assertEqual(self.client.get('/api/photos').json(),[])
    def test_retry_reanalyzes_complete_event_and_dismiss_deletes_all(self):
        self.full()
        worker.process_one(FakeModels('uncertain',evidence=[]))
        item=self.client.get('/api/review').json()['items'][0]
        r=self.client.post('/api/review/'+item['id'],headers=self.csrf,json={'action':'retry'})
        self.assertEqual(r.status_code,200,r.text)
        worker.process_one(FakeModels())
        self.assertEqual(self.alarm().text,'suspicious')
        self.full('dismiss')
        worker.process_one(FakeModels('uncertain',evidence=[]))
        item=self.client.get('/api/review').json()['items'][0]
        r=self.client.post('/api/review/'+item['id'],headers=self.csrf,json={'action':'dismiss'})
        self.assertEqual(r.status_code,200,r.text)
        self.assertEqual(self.alarm('dismiss').text,'normal')
        self.assertEqual(len(list(store.INCOMING.glob('*.jpg'))),0)

    def test_expiry_during_upload_rolls_back_new_file_and_event(self):
        from event_queue import EventConflict
        with patch('main.register_frame',side_effect=EventConflict('deadline crossed')):
            self.assertEqual(self.upload(1).status_code,409)
        self.assertEqual(list(store.INCOMING.glob('*.jpg')),[])
        with store.db() as c:
            self.assertEqual(c.execute('SELECT count(*) FROM jobs').fetchone()[0],0)
            self.assertEqual(c.execute('SELECT count(*) FROM events').fetchone()[0],0)

    def test_event_push_dispatch_and_private_mobile_report(self):
        from push_dispatcher import process_once
        class Sender:
            ready=True
            def __init__(self):self.sent=[]
            def send(self,*args):self.sent.append(args)
        sender=Sender()
        main.app.state.push_sender=sender
        r=self.client.post('/api/push/register',headers=self.csrf,
            json={'installation_id':'a'*32,'token':'token-for-event-push-'*3})
        self.assertEqual(r.status_code,200,r.text)
        headers={'X-Installation-ID':'a'*32,'Authorization':'Bearer '+r.json()['secret']}
        self.full()
        worker.process_one(FakeModels())
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(process_once(self.accounts,sender),1)
        self.assertEqual(process_once(self.accounts,sender),0)
        self.assertEqual(sender.sent[0][1],report['id'])
        self.assertEqual(self.client.get('/api/push/event/'+report['id'],headers=headers).json()['report_id'],report['id'])
        self.assertEqual(self.client.get('/api/push/event/'+'f'*64,headers=headers).status_code,404)

    def test_history_pagination_combines_legacy_and_single_event_reports(self):
        def old(label):
            key=hashlib.sha256(label.encode()).hexdigest()
            (store.INCOMING/(key+'.jpg')).write_bytes(self.jpeg)
            with store.db() as c:
                c.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id) VALUES(?,?,?,?,?,?,?,?,?)',
                          (key,key,'old-camera',label,1,0,store.now(),'queued',self.viewer))
                row=dict(c.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone())
            store.finish(row,'suspicious',label)
            return key
        first=old('legacy-first')
        self.full()
        worker.process_one(FakeModels())
        event=self.client.get('/api/analysis/latest').json()['id']
        last=old('legacy-last')
        found=[];cursor=None
        for _ in range(3):
            params={'limit':1}
            if cursor is not None:params['before']=cursor
            result=self.client.get('/api/analysis/history',params=params).json()
            found.append(result['items'][0]['id']);cursor=result['next_cursor']
        self.assertEqual(found,[last,event,first])
        self.assertIsNone(cursor)
        self.assertEqual(len(self.client.get('/api/analysis/history').json()['items']),3)

    def test_import_unassigned_event_updates_frame_and_event_owners(self):
        with store.db() as c:c.execute('UPDATE devices SET owner_id=NULL')
        self.full()
        worker.process_one(FakeModels())
        self.assertEqual(self.client.get('/api/analysis/history').json()['items'],[])
        r=self.admin_client.post('/api/admin/devices',headers=self.admin_csrf,
            json={'device_id':'camera','owner_id':self.viewer,'include_unassigned':True})
        self.assertEqual(r.status_code,200,r.text)
        report=self.client.get('/api/analysis/latest').json()
        self.assertEqual(report['frame_count'],5)
        with store.db() as c:
            self.assertEqual(c.execute('SELECT owner_id FROM events').fetchone()[0],self.viewer)
            self.assertEqual(c.execute('SELECT count(*) FROM jobs WHERE owner_id=?',(self.viewer,)).fetchone()[0],5)

    def test_event_audio_links_to_one_accessible_joint_report(self):
        import io,wave
        uploaded=self.full('audio-event')
        stream=io.BytesIO()
        with wave.open(stream,'wb') as audio:
            audio.setnchannels(1);audio.setsampwidth(2);audio.setframerate(16000)
            audio.writeframes(b'\x00\x00'*16000*4)
        r=self.client.post('/api/device/audio',content=stream.getvalue(),headers={
            'Authorization':'Bearer '+self.token,'X-Device-ID':'camera','X-Event-ID':'audio-event','Content-Type':'audio/wav'})
        self.assertEqual(r.status_code,201,r.text)
        worker.process_one(FakeModels())
        report=self.client.get('/api/analysis/latest').json()
        recording=self.client.get('/api/audio/recordings').json()['items'][0]
        self.assertEqual(recording['report_ids'],[report['id']])
        self.assertEqual(self.client.get('/api/analysis/report/'+recording['report_ids'][0]).status_code,200)
        self.assertEqual(self.client.get('/api/analysis/report/'+uploaded[1]['job_id']).json()['id'],report['id'])

if __name__=='__main__':
    try:unittest.main(verbosity=2)
    finally:temporary.cleanup()
