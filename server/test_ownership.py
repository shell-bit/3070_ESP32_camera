import unittest
import hashlib
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path
from fastapi.testclient import TestClient
from test_user_auth import LoginTests, main
from store import db, INCOMING, finish, now
import cv2
import numpy as np


class OwnershipTests(LoginTests):
    def setUp(self):
        super().setUp()
        self.other=self.accounts.create('other','Other-test-password!','viewer')

    def assign(self,device,owner,history=False):
        r=self.client.post('/api/admin/devices',headers=self.headers,json={'device_id':device,'owner_id':owner,'include_unassigned':history})
        self.assertEqual(r.status_code,200,r.text)
        return r.json()['upload_token']

    def camera_upload(self,device,token,event='fresh',index='1',extra=None):
        jpeg=cv2.imencode('.jpg',np.full((32,32,3),90,dtype=np.uint8))[1].tobytes()
        return self.client.post('/api/device/upload',content=jpeg,headers={
            'Content-Type':'image/jpeg','Authorization':'Bearer '+token,'X-Device-ID':device,'X-Event-ID':event,'X-Photo-Index':index,**(extra or {})})

    def seed(self,uid,device,event,state='suspicious'):
        key=hashlib.sha256((device+event).encode()).hexdigest()
        cv2.imwrite(str(INCOMING/(key+'.jpg')),np.full((32,32,3),90,dtype=np.uint8))
        with db() as c:
            c.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id) VALUES(?,?,?,?,?,?,?,?,?)',
                      (key,'fixture',device,event,1,0,now(),'review',uid))
            row=c.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone()
        if state=='suspicious':finish(row,'suspicious','Report for '+device)
        return key

    def test_cross_user_reports_photos_counters_and_direct_urls(self):
        a=self.seed(self.viewer,'camera-a','a')
        b=self.seed(self.other,'camera-b','b')
        unassigned=self.seed(None,'legacy','c')
        review_b=self.seed(self.other,'camera-b','review','review')
        self.login('viewer','Viewer-test-password!')
        photos=self.client.get('/api/photos').json()
        self.assertEqual([p['name'] for p in photos],[a+'.jpg'])
        self.assertEqual(self.client.get('/api/analysis/latest').json()['id'],a)
        self.assertEqual([r['id'] for r in self.client.get('/api/analysis/history').json()['items']],[a])
        self.assertEqual(self.client.get('/api/health').json()['jobs'],{'suspicious':1})
        self.assertEqual(self.client.get('/api/device/status').json()['device_id'],'camera-a')
        for key in [b,unassigned]:
            self.assertEqual(self.client.get('/photos/'+key+'.jpg').status_code,404)
            self.assertEqual(self.client.get('/photos/'+key+'.jpg?download=true').status_code,404)
        self.assertEqual(self.client.get('/api/review').json()['items'],[])
        self.assertEqual(self.client.get('/api/review/'+review_b+'/photo').status_code,404)
        for action in ['keep','dismiss','retry']:
            r=self.client.post('/api/review/'+review_b,headers=self.headers,json={'action':action,'description':'Not mine'})
            self.assertEqual(r.status_code,404,r.text)
        with db() as c:self.assertEqual(c.execute('SELECT state FROM jobs WHERE id=?',(review_b,)).fetchone()['state'],'review')

    def test_device_identity_cannot_be_spoofed(self):
        self.login()
        a=self.assign('camera-a',self.viewer)
        b=self.assign('camera-b',self.other)
        self.assertEqual(self.camera_upload('camera-a',b).status_code,401)
        self.assertEqual(self.camera_upload('camera-a','test-device-token').status_code,401)
        r=self.camera_upload('camera-a',a,extra={'X-User-ID':str(self.other)})
        self.assertEqual(r.status_code,201,r.text)
        key=r.json()['job_id']
        with db() as c:self.assertEqual(c.execute('SELECT owner_id FROM jobs WHERE id=?',(key,)).fetchone()['owner_id'],self.viewer)
        self.assertTrue(self.camera_upload('camera-a',a).json()['duplicate'])
        r=self.client.get('/api/device/event-status',params={'device_id':'camera-a','event_id':'fresh'},headers={'Authorization':'Bearer '+b})
        self.assertEqual(r.status_code,401)

    def test_reassignment_preserves_prior_reports_and_rotates_token(self):
        self.login()
        first=self.assign('camera-a',self.viewer)
        key=self.camera_upload('camera-a',first).json()['job_id']
        with db() as c:
            row=c.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone()
            event_key=row['event_key']
            c.execute("UPDATE events SET state='review',error='Missing input' WHERE id=?",(event_key,))
            c.execute("UPDATE jobs SET state='review' WHERE event_key=?",(event_key,))
        from manual_review import decide
        decide(event_key,'keep','First owner event',owner_id=self.viewer)
        second=self.assign('camera-a',self.other,True)
        self.assertNotEqual(first,second)
        self.assertEqual(self.camera_upload('camera-a',first,'new').status_code,401)
        self.assertEqual(self.camera_upload('camera-a',second).status_code,409)
        self.assertEqual(self.camera_upload('camera-a',second,'new').status_code,201)
        r=self.client.get('/api/device/event-status',params={'device_id':'camera-a','event_id':'fresh'},headers={'Authorization':'Bearer '+second})
        self.assertEqual(r.text,'not_found')
        self.login('other','Other-test-password!')
        self.assertEqual(self.client.get('/photos/'+key+'.jpg').status_code,404)
        self.assertEqual(self.client.get('/api/my/devices').json()['devices'],['camera-a'])
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.get('/photos/'+key+'.jpg').status_code,200)
        self.assertEqual(self.client.get('/api/my/devices').json()['devices'],[])

    def test_unassigned_history_is_opt_in(self):
        a=self.seed(None,'legacy','unassigned')
        b=self.seed(self.other,'legacy','already-owned')
        self.login()
        self.assign('legacy',self.viewer)
        with db() as c:self.assertIsNone(c.execute('SELECT owner_id FROM jobs WHERE id=?',(a,)).fetchone()['owner_id'])
        self.assign('legacy',self.viewer,True)
        with db() as c:
            self.assertEqual(c.execute('SELECT owner_id FROM jobs WHERE id=?',(a,)).fetchone()['owner_id'],self.viewer)
            self.assertEqual(c.execute('SELECT owner_id FROM jobs WHERE id=?',(b,)).fetchone()['owner_id'],self.other)

    def test_admin_only_management_and_no_reports(self):
        self.seed(self.viewer,'camera-a','test')
        self.login()
        self.assertEqual(self.client.get('/',follow_redirects=False).headers['location'],'/admin')
        self.assertEqual(self.client.get('/admin').status_code,200)
        for route in ['/api/photos','/api/analysis/history','/api/analysis/latest','/api/device/status','/api/review']:
            self.assertEqual(self.client.get(route).status_code,403,route)
        self.assertNotIn('jobs',self.client.get('/api/health').json())
        devices=self.client.get('/api/admin/devices').json()['devices']
        self.assertNotIn('token_digest',devices[0])
        self.assertNotIn('description',devices[0])
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.post('/api/admin/devices',headers=self.headers,json={'device_id':'stolen','owner_id':self.viewer}).status_code,403)

    def test_disabled_owner_cannot_receive_camera_uploads(self):
        self.login()
        token=self.assign('camera-a',self.viewer)
        self.accounts.change_user(self.viewer,None,False,None,self.admin)
        self.assertEqual(self.camera_upload('camera-a',token).status_code,403)

    def test_cannot_assign_camera_to_admin_or_invalid_user(self):
        self.login()
        for uid in [self.admin,99999]:
            r=self.client.post('/api/admin/devices',headers=self.headers,json={'device_id':'camera-a','owner_id':uid})
            self.assertEqual(r.status_code,422)

    def test_legacy_upload_cannot_choose_owner(self):
        r=self.camera_upload('legacy','test-device-token',extra={'X-User-ID':str(self.viewer)})
        self.assertEqual(r.status_code,201)
        with db() as c:self.assertIsNone(c.execute('SELECT owner_id FROM jobs WHERE id=?',(r.json()['job_id'],)).fetchone()['owner_id'])

    def test_additive_migration_preserves_old_records(self):
        with tempfile.TemporaryDirectory() as folder:
            c=sqlite3.connect(Path(folder)/'queue.sqlite3')
            c.execute('CREATE TABLE jobs(seq INTEGER PRIMARY KEY, id TEXT UNIQUE, digest TEXT, device TEXT,event TEXT,photo_index INTEGER,night INTEGER,received TEXT,state TEXT,report TEXT,error TEXT)')
            c.execute("INSERT INTO jobs VALUES(1,'original','hash','legacy','event',1,0,'timestamp','review',NULL,'keep me')")
            c.commit();c.close()
            code="from store import db\nwith db() as c:\n r=c.execute('SELECT * FROM jobs').fetchone()\n assert r['id']=='original' and r['error']=='keep me' and r['owner_id'] is None\n assert c.execute('SELECT COUNT(*) FROM devices').fetchone()[0]==0\nprint('migration passed')"
            result=subprocess.run([sys.executable,'-c',code],env={**os.environ,'CAMERA_DATA_DIR':folder},capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__':
    # Inherited login tests run once as part of this comprehensive suite.
    unittest.main(verbosity=2,defaultTest='OwnershipTests')
