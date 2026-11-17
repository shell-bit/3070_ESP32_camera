"""Synthetic protocol/policy integration; this is NOT recognition accuracy evidence."""
import json
import os
import unittest
from unittest.mock import patch
from test_identity_ingest import IdentityIngestTests as Fixtures, main, store
import face_registry
import worker


class ActorModels:
    def __init__(self,actor='p1',broken=False):self.actor,self.broken=actor,broken
    def has_person(self,path):return True
    def detect_people(self,path):return [[0,0,16,24],[17,0,32,24]]
    def assess_event(self,paths,frames):return 'suspicious','Synthetic visible action','violence',[1]
    def assess_actors(self,paths,frames,tracks):
        if self.broken:raise ValueError('Simulated malformed actor output')
        return {'coverage':'complete','actions':[{'verdict':'suspicious','actor_track_id':self.actor,
            'target_track_ids':['p1'] if self.actor=='p2' else [],'evidence_frame_indices':[1]}]}


class FamilyPipelineTests(Fixtures):
    def setUp(self):
        super().setUp()
        self.config=patch.dict(os.environ,{'FAMILY_EXEMPTION_DEVICES':'camera','FACE_RECOGNIZER_VERSIONS':'test-v1',
                                          'FAMILY_EXEMPTION_RETENTION_DAYS':'7'})
        self.config.start();self.addCleanup(self.config.stop)
        with store.db() as c:
            for table in ('family_members','member_devices','device_commands','family_device_state','alarm_decisions','push_events','push_deliveries'):
                c.execute('DELETE FROM '+table)
            member=face_registry.create_member(c,self.owner,'Family member')
            self.member=member['id']
            command=face_registry.create_enrollment(c,self.owner,self.member,'camera','enrollment')
            face_registry.ack_command(c,self.owner,'camera',command['id'],command['member_version'],
                'committed',database_version='db-v1',recognizer_version='test-v1')

    def full_family(self):
        result=None
        for i in range(1,6):
            m=self.metadata(i);m['faces']=[{'face_id':'f1','bbox':[4,1,8,5],'status':'matched',
                'member_id':self.member,'member_version':1,'match_score':0.9}]
            response=self.upload(m);self.assertEqual(response.status_code,201,response.text)
            result=response.json()
        return result['event_key']

    def state(self,key):
        with store.db() as c:return dict(c.execute('SELECT * FROM events WHERE id=?',(key,)).fetchone())

    def test_family_actor_private_media_no_push_and_device_terminal(self):
        key=self.full_family();self.assertTrue(worker.process_one(ActorModels()))
        self.assertEqual(self.state(key)['state'],'exempted')
        with store.db() as c:
            self.assertEqual(c.execute('SELECT count(*) FROM push_events').fetchone()[0],0)
            self.assertEqual(c.execute('SELECT count(*) FROM alarm_decisions').fetchone()[0],1)
        response=self.client.get('/api/device/event-status',params={'device_id':'camera','event_id':'event'},
            headers={'Authorization':'Bearer camera-token'})
        self.assertEqual(response.text,'exempted')
        self.assertEqual(response.headers['content-length'],'8')
        self.client.post('/api/auth/login',headers={'X-Sentinel-Request':'1'},
            json={'username':'identity-owner','password':'Identity-owner-password!'})
        records=self.client.get('/api/family/events').json()['items']
        self.assertEqual(len(records),1)
        self.assertEqual(records[0]['model_verdict'],'suspicious')
        url=records[0]['frames'][0]['photo_url']
        self.assertEqual(self.client.get(url).content,self.jpeg)
        self.assertEqual(self.client.get('/api/analysis/history').json()['items'],[])
        csrf=self.client.get('/api/auth/me').json()['csrf']
        self.assertEqual(self.client.delete('/api/family/events/'+key,
            headers={'X-CSRF-Token':csrf,'Origin':'https://testserver'}).status_code,200)
        self.assertEqual(self.client.get(url).status_code,404)
        self.client.post('/api/auth/logout',headers={'X-CSRF-Token':csrf,'Origin':'https://testserver'})

    def test_family_target_does_not_exempt_unknown_actor(self):
        key=self.full_family();worker.process_one(ActorModels(actor='p2'))
        self.assertEqual(self.state(key)['state'],'suspicious')
        with store.db() as c:self.assertEqual(c.execute('SELECT count(*) FROM push_events').fetchone()[0],1)

    def test_revocation_between_upload_and_decision_prevents_exemption(self):
        key=self.full_family()
        with store.db() as c:face_registry.revoke_member(c,self.owner,self.member)
        worker.process_one(ActorModels())
        self.assertEqual(self.state(key)['state'],'suspicious')

    def test_broken_actor_extension_preserves_original_suspicious(self):
        key=self.full_family();worker.process_one(ActorModels(broken=True))
        self.assertEqual(self.state(key)['state'],'suspicious')

    def test_record_only_does_not_enable_exemption(self):
        key=self.full_family()
        with patch.dict(os.environ,{'FAMILY_EXEMPTION_DEVICES':''}):worker.process_one(ActorModels())
        self.assertEqual(self.state(key)['state'],'suspicious')


if __name__=='__main__':unittest.main()
