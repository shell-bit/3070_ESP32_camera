import hashlib
import io
import time
import wave
from fastapi.testclient import TestClient
from test_user_auth import LoginTests, main
from store import db, now
from audio_api import audio_db, AUDIO


def wav_bytes(seconds=4):
    out=io.BytesIO()
    with wave.open(out,'wb') as w:
        w.setnchannels(1);w.setsampwidth(2);w.setframerate(16000)
        w.writeframes(b'\0\0'*int(16000*seconds))
    return out.getvalue()


class AudioTests(LoginTests):
    def setUp(self):
        super().setUp()
        with audio_db() as c:
            for table in ['talk_chunks','talk_sessions','audio_presence','event_audio']:c.execute('DELETE FROM '+table)
        self.other=self.accounts.create('other','Other-test-password!','viewer')
        self.token='camera-a-token'
        with db() as c:
            c.execute('INSERT INTO devices VALUES(?,?,?)',('camera-a',self.viewer,hashlib.sha256(self.token.encode()).hexdigest()))
            c.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id) VALUES(?,?,?,?,?,?,?,?,?)',
                ('a'*64,'digest','camera-a','event-a',1,0,now(),'review',self.viewer))
        self.device=TestClient(main.app,base_url='https://testserver')
        self.dh={'Authorization':'Bearer '+self.token,'X-Device-ID':'camera-a'}

    def poll(self,ready=True):
        return self.device.get('/api/device/talk',params={'device_id':'camera-a','ready':str(ready).lower()},headers=self.dh)

    def start(self):
        self.assertEqual(self.poll().status_code,204)
        self.login('viewer','Viewer-test-password!')
        r=self.client.post('/api/audio/talk/start',json={'device_id':'camera-a'},headers=self.headers)
        self.assertEqual(r.status_code,200,r.text)
        return r.json()['session_id']

    def send(self,sid,n=0):
        return self.client.post('/api/audio/talk/'+sid,content=b'\x00\x10'*16000,
            headers={**self.headers,'X-Audio-Sequence':str(n),'Content-Type':'application/octet-stream'})

    def test_audio_upload_validation_and_private_playback(self):
        h={**self.dh,'X-Event-ID':'event-a','Content-Type':'audio/wav'}
        self.assertEqual(self.device.post('/api/device/audio',content=b'bad',headers=h).status_code,422)
        self.assertEqual(self.device.post('/api/device/audio',content=wav_bytes(),headers={**h,'Authorization':'Bearer bad'}).status_code,401)
        self.assertEqual(self.device.post('/api/device/audio',content=wav_bytes(),headers={**h,'X-Event-ID':'missing'}).status_code,409)
        r=self.device.post('/api/device/audio',content=wav_bytes(),headers=h)
        self.assertEqual(r.status_code,201,r.text);key=r.json()['id']
        self.assertTrue(self.device.post('/api/device/audio',content=wav_bytes(),headers=h).json()['duplicate'])
        self.assertEqual(self.device.post('/api/device/audio',content=wav_bytes(3),headers=h).status_code,409)
        self.assertEqual(self.device.get('/api/audio/recordings/'+key).status_code,401)
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.get('/api/audio/recordings').json()['items'][0]['event_id'],'event-a')
        self.assertEqual(self.client.get('/api/audio/recordings/'+key).content,wav_bytes())
        self.login('other','Other-test-password!')
        self.assertEqual(self.client.get('/api/audio/recordings').json()['items'],[])
        self.assertEqual(self.client.get('/api/audio/recordings/'+key).status_code,404)
        with audio_db() as c:c.execute('UPDATE event_audio SET created=?',(time.time()-8*86400,))
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.get('/api/audio/recordings').json()['items'],[])
        self.assertFalse((AUDIO/(key+'.wav')).exists())

    def test_talk_delivery_ack_and_no_replay(self):
        sid=self.start();self.assertEqual(self.send(sid).status_code,200)
        self.assertTrue(self.send(sid).json()['duplicate'])
        r=self.poll();self.assertEqual(r.status_code,200);self.assertEqual(len(r.content),32000)
        key=r.headers['x-talk-id'];self.assertEqual(self.poll().status_code,204)
        self.assertEqual(self.device.post('/api/device/talk/ack',headers={**self.dh,'X-Talk-ID':key,'X-Playback-OK':'true'}).status_code,200)
        self.assertEqual(self.client.get('/api/audio/talk/'+sid+'/status').json()['chunks']['played'],1)
        self.assertEqual(self.send(sid,1).status_code,200)
        self.assertEqual(self.client.post('/api/audio/talk/'+sid+'/stop',headers=self.headers).status_code,200)
        self.assertEqual(self.poll().status_code,204)

    def test_sleep_csrf_cross_account_and_expiry(self):
        self.login('viewer','Viewer-test-password!')
        self.assertEqual(self.client.post('/api/audio/talk/start',headers=self.headers,json={'device_id':'camera-a'}).status_code,409)
        self.poll()
        self.assertEqual(self.client.post('/api/audio/talk/start',json={'device_id':'camera-a'}).status_code,403)
        sid=self.start()
        self.login('other','Other-test-password!')
        self.assertEqual(self.client.post('/api/audio/talk/start',headers=self.headers,json={'device_id':'camera-a'}).status_code,404)
        self.assertEqual(self.send(sid).status_code,404)
        self.assertEqual(self.client.get('/api/audio/talk/'+sid+'/status').status_code,404)
        self.assertEqual(self.device.get('/api/device/talk?device_id=camera-a',headers={'Authorization':'Bearer bad'}).status_code,401)
        self.login('viewer','Viewer-test-password!')
        # A different login session cannot send into the previous session.
        self.assertEqual(self.send(sid).status_code,409)

    def test_queue_bounded_and_logout_cancels_playback(self):
        sid=self.start()
        for n in range(3):self.assertEqual(self.send(sid,n).status_code,200)
        self.assertEqual(self.send(sid,3).status_code,429)
        self.client.post('/api/auth/logout',headers=self.headers)
        self.assertEqual(self.poll().status_code,204)

    def test_expired_speech_and_reassigned_camera_never_play(self):
        sid=self.start();self.send(sid)
        with audio_db() as c:c.execute('UPDATE talk_chunks SET created=?',(time.time()-9,))
        self.assertEqual(self.poll().status_code,204)
        self.assertEqual(self.send(sid,1).status_code,200)
        with db() as c:c.execute('UPDATE devices SET owner_id=?',(self.other,))
        self.assertEqual(self.poll().status_code,204)

    def test_sleep_cancels_session_and_oversized_audio_rejected(self):
        sid=self.start();self.send(sid)
        self.assertEqual(self.poll(False).status_code,204)
        self.assertEqual(self.poll().status_code,204)
        self.assertEqual(self.send(sid,1).status_code,409)
        h={**self.dh,'X-Event-ID':'event-a','Content-Type':'audio/wav'}
        self.assertEqual(self.device.post('/api/device/audio',content=wav_bytes(6),headers=h).status_code,413)


if __name__=='__main__':
    import unittest
    unittest.main()
