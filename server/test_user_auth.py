"""Isolated integration tests: never opens the production camera data directory."""
import os
import tempfile
import unittest
from pathlib import Path
import cv2
import numpy as np
from fastapi.testclient import TestClient

temp=tempfile.TemporaryDirectory()
os.environ['CAMERA_DATA_DIR']=temp.name
os.environ['UPLOAD_TOKEN']='test-device-token'
import main
from store import db, finish
from user_auth import COOKIE


class LoginTests(unittest.TestCase):
    def setUp(self):
        self.accounts=main.app.state.accounts
        with self.accounts.db() as c:
            c.execute('DELETE FROM push_installations')
            for name in ['sessions','limits','auth_audit','users']:
                c.execute('DELETE FROM '+name)
        with db() as c:
            c.execute('DELETE FROM push_deliveries')
            c.execute('DELETE FROM push_events')
            c.execute('DELETE FROM jobs')
            c.execute('DELETE FROM events')
            c.execute('DELETE FROM devices')
        self.admin=self.accounts.create('admin','Example-test-password!','admin')
        self.viewer=self.accounts.create('viewer','Viewer-test-password!','viewer')
        self.client=TestClient(main.app,base_url='https://testserver')

    def login(self,name='admin',password='Example-test-password!'):
        response=self.client.post('/api/auth/login',headers={'X-Sentinel-Request':'1'},json={'username':name,'password':password})
        self.assertEqual(response.status_code,200,response.text)
        me=self.client.get('/api/auth/me').json()
        self.headers={'X-CSRF-Token':me['csrf'],'Origin':'https://testserver'}
        return response

    def upload(self,event='test1'):
        jpeg=cv2.imencode('.jpg',np.zeros((32,32,3),dtype=np.uint8))[1].tobytes()
        return self.client.post('/api/device/upload',content=jpeg,headers={
            'Content-Type':'image/jpeg','Authorization':'Bearer test-device-token',
            'X-Device-ID':'test-camera','X-Event-ID':event,'X-Photo-Index':'1'})

    def test_private_routes_and_minimal_health(self):
        self.assertEqual(self.client.get('/',follow_redirects=False).status_code,303)
        for path in ['/api/photos','/api/analysis/latest','/api/analysis/history','/api/review','/api/device/status','/docs','/openapi.json','/photos/'+('a'*64)+'.jpg','/api/review/'+('a'*64)+'/photo','/api/admin/users']:
            self.assertEqual(self.client.get(path).status_code,401,path)
        self.assertEqual(self.client.get('/api/health').json(),{'server':'running','login_required':True})
        self.assertEqual(self.client.get('/login').status_code,200)

    def test_login_cookie_and_logout(self):
        response=self.login()
        cookie=response.headers['set-cookie']
        for word in ['Secure','HttpOnly','SameSite=strict']: self.assertIn(word,cookie)
        self.assertEqual(self.client.get('/').status_code,200)
        self.assertEqual(self.client.get('/api/admin/users').status_code,200)
        self.assertEqual(self.client.post('/api/auth/logout',headers=self.headers).status_code,200)
        self.assertEqual(self.client.get('/api/photos').status_code,401)

    def test_login_errors_and_limit(self):
        data={'username':'admin','password':'bad'}
        self.assertEqual(self.client.post('/api/auth/login',json=data).status_code,403)
        for _ in range(10):
            self.assertEqual(self.client.post('/api/auth/login',json=data,headers={'X-Sentinel-Request':'1'}).status_code,401)
        self.assertEqual(self.client.post('/api/auth/login',json=data,headers={'X-Sentinel-Request':'1'}).status_code,429)

    def test_csrf_and_origin(self):
        self.login()
        self.assertEqual(self.client.post('/api/auth/logout').status_code,403)
        bad={**self.headers,'Origin':'https://evil.example'}
        self.assertEqual(self.client.post('/api/auth/logout',headers=bad).status_code,403)
        self.assertEqual(self.client.post('/api/auth/logout',headers={**self.headers,'Sec-Fetch-Site':'cross-site'}).status_code,403)
        self.assertEqual(self.client.post('/api/auth/logout',headers={**self.headers,'X-CSRF-Token':'wrong'}).status_code,403)

    def test_viewer_and_admin_access(self):
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.get('/api/photos').status_code,200)
        self.assertEqual(self.client.get('/api/review').status_code,200)
        for path in ['/admin','/admin.js','/api/admin/users','/api/admin/devices']:
            self.assertEqual(self.client.get(path).status_code,403)
        self.assertEqual(self.client.post('/api/admin/users',headers=self.headers,json={'username':'new-user','password':'Password-long-enough'}).status_code,403)

    def test_create_disable_and_last_admin(self):
        self.login()
        response=self.client.post('/api/admin/users',headers=self.headers,json={'username':'new-user','password':'Password-long-enough'})
        self.assertEqual(response.status_code,201,response.text)
        self.assertEqual(self.client.patch('/api/admin/users/'+str(self.admin),headers=self.headers,json={'active':False}).status_code,409)
        raw=self.accounts.login('viewer','Viewer-test-password!','local')
        self.assertIsNotNone(self.accounts.session(raw))
        response=self.client.patch('/api/admin/users/'+str(self.viewer),headers=self.headers,json={'active':False})
        self.assertEqual(response.status_code,200,response.text)
        self.assertIsNone(self.accounts.session(raw))
        users=self.client.get('/api/admin/users').json()['users']
        self.assertNotIn('password_hash',users[0])

    def test_password_change_revokes_sessions(self):
        self.login()
        old=self.client.cookies.get(COOKIE)
        response=self.client.post('/api/auth/password',headers=self.headers,json={'current_password':'Example-test-password!','new_password':'A-new-test-password!'})
        self.assertEqual(response.status_code,200,response.text)
        self.assertIsNone(self.accounts.session(old))
        self.login(password='A-new-test-password!')

    def test_expired_session(self):
        self.login()
        with self.accounts.db() as c: c.execute('UPDATE sessions SET seen=0')
        self.assertEqual(self.client.get('/api/photos').status_code,401)

    def test_device_upload_dedup_alarm_without_login(self):
        response=self.upload()
        self.assertEqual(response.status_code,201,response.text)
        response=self.upload()
        self.assertTrue(response.json()['duplicate'])
        response=self.client.get('/api/device/event-status',params={'device_id':'test-camera','event_id':'test1'},headers={'Authorization':'Bearer test-device-token'})
        self.assertEqual(response.status_code,200,response.text)
        self.assertEqual(response.text,'pending')
        self.assertEqual(self.client.get('/api/device/event-status',params={'device_id':'test-camera','event_id':'test1'}).status_code,401)

    def test_manual_review_and_gallery_flow(self):
        # A genuine pre-event legacy record must retain its original review URL.
        from store import INCOMING,now
        job='b'*64
        (INCOMING/(job+'.jpg')).write_bytes(cv2.imencode('.jpg',np.zeros((32,32,3),dtype=np.uint8))[1].tobytes())
        with db() as c:
            c.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,error,owner_id) VALUES(?,?,?,?,?,?,?,?,?,?)',
                      (job,'fixture','legacy','manual',1,0,now(),'review','Needs review',self.viewer))
        self.login('viewer','Viewer-test-password!')
        os.environ['REVIEW_TOKEN']='old-review-token'
        try:
            self.assertEqual(self.client.get('/api/review').status_code,200)
            self.assertEqual(self.client.get('/api/review/'+job+'/photo').status_code,200)
            response=self.client.post('/api/review/'+job,headers=self.headers,json={'action':'keep','description':'Test human-reviewed event'})
            self.assertEqual(response.status_code,200,response.text)
            self.assertEqual(len(self.client.get('/api/photos').json()),1)
            self.assertEqual(self.client.get('/photos/'+job+'.jpg').status_code,200)
            self.assertEqual(len(self.client.get('/api/analysis/history').json()['items']),1)
        finally: os.environ.pop('REVIEW_TOKEN',None)


if __name__=='__main__': unittest.main(verbosity=2)
