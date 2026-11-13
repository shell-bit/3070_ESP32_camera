"""HTTP integration uses real auth, temporary accounts and a temporary SQLite queue."""
import hashlib
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from fastapi import FastAPI
from fastapi.testclient import TestClient

_data=tempfile.TemporaryDirectory(prefix='family-api-test-')
os.environ['CAMERA_DATA_DIR']=_data.name
os.environ['UPLOAD_TOKEN']='legacy-test-token'


class MemberAPITests(unittest.TestCase):
    def setUp(self):
        for name in ('member_api','device_commands'):
            self.assertIsNotNone(importlib.util.find_spec(name),name+' implementation missing')
        from user_auth import install_user_auth
        from ownership import install_ownership
        from member_api import install_member_api
        from device_commands import install_device_commands
        from store import db, ROOT
        from face_registry import schema
        self.db=db
        self.app=FastAPI()
        account_dir=tempfile.TemporaryDirectory(dir=_data.name)
        self.addCleanup(account_dir.cleanup)
        self.accounts=install_user_auth(self.app,ROOT,account_dir.name)
        self.owner=self.accounts.create('owner','Owner-password-long!','viewer')
        self.other=self.accounts.create('other','Other-password-long!','viewer')
        self.admin=self.accounts.create('admin','Admin-password-long!','admin')
        install_ownership(self.app); install_member_api(self.app); install_device_commands(self.app)
        self.client=TestClient(self.app,base_url='https://testserver')
        with db() as c:
            schema(c)
            for table in ('family_members','member_devices','device_commands','family_device_state','devices'):
                c.execute('DELETE FROM '+table)
            c.executemany('INSERT INTO devices VALUES(?,?,?)',[(device,owner,hashlib.sha256(token.encode()).hexdigest())
                for device,owner,token in [('a',self.owner,'token-a'),('b',self.owner,'token-b'),('other',self.other,'token-other'),('unowned',None,'token-unowned')]])
        self.login()

    def login(self,name='owner',password='Owner-password-long!'):
        result=self.client.post('/api/auth/login',headers={'X-Sentinel-Request':'1'},json={'username':name,'password':password})
        self.assertEqual(result.status_code,200,result.text)
        self.headers={'Origin':'https://testserver','X-CSRF-Token':self.client.get('/api/auth/me').json()['csrf']}

    def create(self,name='Family'):
        response=self.client.post('/api/family/members',headers=self.headers,json={'display_name':name})
        self.assertEqual(response.status_code,201,response.text)
        return response.json()['member']

    def enroll(self,member,device='a',request='request-1'):
        return self.client.post('/api/family/members/'+member['id']+'/enrollments',headers=self.headers,json={'device_id':device,'request_id':request})

    def camera(self,device='a',token='token-a'):
        return self.client.get('/api/device/commands',params={'device_id':device},headers={'Authorization':'Bearer '+token})

    def ack(self,command,result='committed',**overrides):
        body={'device_id':'a','member_version':command['member_version'],'result':result,
              'recognizer_version':'model-1','database_version':'db-1',**overrides}
        return self.client.post('/api/device/commands/'+command['id']+'/ack',headers={'Authorization':'Bearer token-a'},json=body)

    def test_http_enrollment_waiting_sampling_commit_and_revoke(self):
        member=self.create(); response=self.enroll(member)
        self.assertEqual(response.status_code,201,response.text); command=response.json()['command']
        listed=self.client.get('/api/family/members').json()
        registration=listed['members'][0]['devices'][0]
        self.assertEqual(registration['state'],'pending'); self.assertFalse(registration['online'])
        self.assertFalse(listed['exemptions_enabled']); self.assertEqual(listed['identity_mode'],'record_only')
        self.assertEqual(self.camera().json()['commands'][0]['id'],command['id'])
        sampling=self.ack(command,'sampling',database_version=None,recognizer_version=None)
        self.assertEqual(sampling.status_code,200,sampling.text)
        self.assertEqual(self.client.get('/api/family/members').json()['members'][0]['devices'][0]['command_status'],'sampling')
        self.assertEqual(self.ack(command).json()['registration']['state'],'active')
        self.assertEqual(self.camera().json()['commands'],[])
        response=self.client.patch('/api/family/members/'+member['id'],headers=self.headers,json={'enabled':False})
        self.assertEqual(response.status_code,200,response.text)
        self.assertEqual(self.ack(command).status_code,409)
        self.assertEqual(self.camera().json()['commands'][0]['operation'],'revoke')

    def test_device_credentials_are_strict_and_do_not_accept_legacy_anonymous(self):
        self.client.cookies.clear()
        for device,token in [('unknown','legacy-test-token'),('unknown',''),('unowned','token-unowned'),('a','token-b')]:
            self.assertIn(self.camera(device,token).status_code,(401,403,404))
        self.assertEqual(self.camera().status_code,200)

    def test_cross_owner_member_and_cross_camera_ack_denied(self):
        member=self.create(); command=self.enroll(member).json()['command']
        wrong=self.client.post('/api/device/commands/'+command['id']+'/ack',headers={'Authorization':'Bearer token-b'},
             json={'device_id':'b','member_version':1,'result':'committed','database_version':'db-1','recognizer_version':'model-1'})
        self.assertEqual(wrong.status_code,404,wrong.text)
        self.assertEqual(self.enroll(member,'other','wrong').status_code,404)
        self.login('other','Other-password-long!')
        self.assertEqual(self.client.get('/api/family/members').json()['members'],[])
        self.assertEqual(self.enroll(member,'other').status_code,404)
        self.assertEqual(self.client.delete('/api/family/members/'+member['id'],headers=self.headers).status_code,404)

    def test_members_mutations_require_session_origin_and_csrf(self):
        for headers in [{},{**self.headers,'Origin':'https://evil.example'},{**self.headers,'Sec-Fetch-Site':'cross-site'},
                        {**self.headers,'X-CSRF-Token':'wrong'}]:
            self.assertEqual(self.client.post('/api/family/members',headers=headers,json={'display_name':'bad'}).status_code,403)
        self.client.cookies.clear()
        self.assertEqual(self.client.get('/api/family/members').status_code,401)
        self.assertEqual(self.client.get('/members',follow_redirects=False).status_code,303)

    def test_rename_reenable_delete_tombstone_and_invalid_name(self):
        self.assertEqual(self.client.post('/api/family/members',headers=self.headers,json={'display_name':'   '}).status_code,422)
        member=self.create('<img onerror=alert(1)>')
        path='/api/family/members/'+member['id']
        self.assertEqual(self.client.patch(path,headers=self.headers,json={'display_name':'Renamed'}).json()['member']['display_name'],'Renamed')
        self.assertEqual(self.client.delete(path,headers=self.headers).status_code,200)
        self.assertEqual(self.client.delete(path,headers=self.headers).status_code,200)
        self.assertEqual(self.client.get('/api/family/members').json()['members'],[])
        self.assertEqual(self.enroll(member).status_code,404)
        with self.db() as c:self.assertIsNotNone(c.execute('SELECT deleted_at FROM family_members WHERE id=?',(member['id'],)).fetchone()[0])

    def test_reset_report_revokes_active_and_missing_commit_versions_rejected(self):
        member=self.create(); command=self.enroll(member).json()['command']
        self.assertEqual(self.ack(command,database_version=None).status_code,422)
        self.assertEqual(self.ack(command).status_code,200)
        response=self.client.post('/api/device/identity-state',headers={'Authorization':'Bearer token-a'},
             json={'device_id':'a','recognizer_version':'model-1','database_version':'empty','reset':True})
        self.assertEqual(response.status_code,200,response.text)
        self.assertTrue(response.json()['requires_enrollment'])
        self.assertEqual(self.client.get('/api/family/members').json()['members'][0]['devices'][0]['state'],'revoked')

    def test_mode_requires_owned_allowlisted_device_model_and_retention(self):
        values={'FAMILY_EXEMPTION_DEVICES':'other','FACE_RECOGNIZER_VERSIONS':'model-1','FAMILY_EXEMPTION_RETENTION_DAYS':'7'}
        old={key:os.environ.get(key) for key in values}
        try:
            os.environ.update(values)
            self.assertFalse(self.client.get('/api/family/members').json()['exemptions_enabled'])
            os.environ['FAMILY_EXEMPTION_DEVICES']='a'
            self.assertTrue(self.client.get('/api/family/members').json()['exemptions_enabled'])
            os.environ['FAMILY_EXEMPTION_RETENTION_DAYS']='0'
            self.assertFalse(self.client.get('/api/family/members').json()['exemptions_enabled'])
        finally:
            for key,value in old.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value


if __name__=='__main__': unittest.main(verbosity=2)
