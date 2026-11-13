import time
from test_ownership import OwnershipTests, main
from store import db
from push_dispatcher import process_once

class Sender:
    ready=True
    def __init__(self): self.sent=[]; self.error=None
    def send(self,*args):
        if self.error:raise self.error
        self.sent.append(args)

class PushTests(OwnershipTests):
    def setUp(self):
        super().setUp()
        self.sender=Sender();main.app.state.push_sender=self.sender

    def register(self,name='viewer',identity='a'*32,token='t'*40):
        self.login(name, 'Viewer-test-password!' if name=='viewer' else 'Other-test-password!')
        r=self.client.post('/api/push/register',headers=self.headers,json={'installation_id':identity,'token':token,'owner_id':self.other})
        self.assertEqual(r.status_code,200,r.text)
        phone=r.json()
        return phone,{'X-Installation-ID':identity,'Authorization':'Bearer '+phone['secret']}

    def test_push_isolation_and_once_per_burst(self):
        phone,h=self.register()
        self.register('other','b'*32,'u'*40)
        a=self.seed(self.viewer,'cam-a','event-a')
        b=self.seed(self.other,'cam-b','event-b')
        self.seed(None,'legacy','event-c')
        self.assertEqual(process_once(self.accounts,self.sender),2)
        self.assertEqual(process_once(self.accounts,self.sender),0)
        self.assertEqual({(s[0],s[1]) for s in self.sender.sent},{('t'*40,a),('u'*40,b)})
        self.assertEqual(self.client.get('/api/push/event/'+a,headers=h).status_code,200)
        self.assertEqual(self.client.get('/api/push/event/'+b,headers=h).status_code,404)
        with db() as c:
            from push_events import enqueue
            enqueue(c,a)
            c.execute('''INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id)
                SELECT ?,digest,device,event,2,night,received,state,owner_id FROM jobs WHERE id=?''',('f'*64,a))
            enqueue(c,'f'*64)
            self.assertEqual(c.execute('SELECT count(*) FROM push_events WHERE owner_id=?',(self.viewer,)).fetchone()[0],1)
        self.assertEqual(process_once(self.accounts,self.sender),0)

    def test_push_logout_and_disable_revokes(self):
        phone,h=self.register()
        a=self.seed(self.viewer,'cam','first')
        self.assertEqual(self.client.post('/api/auth/logout',headers=self.headers).status_code,200)
        self.assertEqual(self.client.get('/api/push/event/'+a,headers=h).status_code,401)
        self.assertEqual(process_once(self.accounts,self.sender),0)
        phone,h=self.register()
        self.login()
        self.client.patch('/api/admin/users/'+str(self.viewer),headers=self.headers,json={'active':False})
        self.assertEqual(self.client.post('/api/push/refresh',headers=h,json={'token':'z'*40}).status_code,401)

    def test_push_registration_proof_csrf_and_admin(self):
        phone,h=self.register()
        body={'installation_id':'a'*32,'token':'x'*40}
        self.assertEqual(self.client.post('/api/push/register',json=body).status_code,403)
        self.assertEqual(self.client.post('/api/push/register',headers=self.headers,json=body).status_code,409)
        body['previous_secret']=phone['secret']
        self.assertEqual(self.client.post('/api/push/register',headers=self.headers,json=body).status_code,200)
        self.assertEqual(self.client.post('/api/push/refresh',headers=h,json={'token':'z'*40}).status_code,401)
        self.login()
        self.assertEqual(self.client.post('/api/push/register',headers=self.headers,json=body).status_code,403)

    def test_push_no_historical_backfill_and_retry(self):
        self.seed(self.viewer,'cam','old')
        phone,h=self.register()
        self.assertEqual(process_once(self.accounts,self.sender),0)
        a=self.seed(self.viewer,'cam','new')
        self.sender.error=RuntimeError('fake provider unavailable')
        self.assertEqual(process_once(self.accounts,self.sender),0)
        with db() as c:
            row=c.execute('SELECT * FROM push_deliveries WHERE event_id=?',(a,)).fetchone()
            self.assertEqual(row['attempts'],1);self.assertEqual(row['status'],'pending')
            c.execute('UPDATE push_deliveries SET next_try=0')
        self.sender.error=None
        self.assertEqual(process_once(self.accounts,self.sender),1)

    def test_push_refresh_unregister_and_unconfigured(self):
        phone,h=self.register()
        self.assertEqual(self.client.post('/api/push/refresh',headers=h,json={'token':'z'*40}).status_code,200)
        self.seed(self.viewer,'cam','new')
        process_once(self.accounts,self.sender)
        self.assertEqual(self.sender.sent[0][0],'z'*40)
        self.assertEqual(self.client.post('/api/push/unregister',headers=h).status_code,200)
        self.assertEqual(self.client.post('/api/push/refresh',headers=h,json={'token':'q'*40}).status_code,401)
        self.sender.ready=False
        self.assertEqual(self.client.post('/api/push/register',headers=self.headers,json={'installation_id':'b'*32,'token':'t'*40}).status_code,503)

    def test_push_registration_cannot_race_completed_logout(self):
        from unittest.mock import patch
        from ownership import current_owner
        self.login('viewer','Viewer-test-password!')
        def revoke_during_request(request):
            uid=current_owner(request)
            with self.accounts.db() as c:c.execute('DELETE FROM sessions WHERE user_id=?',(uid,))
            return uid
        with patch('push_api.current_owner',side_effect=revoke_during_request):
            r=self.client.post('/api/push/register',headers=self.headers,json={'installation_id':'a'*32,'token':'t'*40})
        self.assertEqual(r.status_code,401,r.text)
        with self.accounts.db() as c:self.assertEqual(c.execute('SELECT count(*) FROM push_installations').fetchone()[0],0)

    def test_push_manual_review_enqueue(self):
        phone,h=self.register()
        key=self.seed(self.viewer,'cam','review','review')
        r=self.client.post('/api/review/'+key,headers=self.headers,json={'action':'keep','description':'Observed damage, please review.'})
        self.assertEqual(r.status_code,200,r.text)
        self.assertEqual(process_once(self.accounts,self.sender),1)
        report=self.client.get('/api/push/event/'+key,headers=h).json()
        self.assertEqual(report['description'],'Observed damage, please review.')
