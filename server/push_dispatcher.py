"""Bounded, retryable push delivery outside camera upload and inference requests."""
import logging
import os
import time
import threading
from datetime import timedelta
from pathlib import Path
from store import db
from push_events import schema

class FirebaseSender:
    def __init__(self):
        self.ready=False
        path=os.environ.get('GOOGLE_APPLICATION_CREDENTIALS','')
        if not path or not Path(path).is_file():return
        try:
            import firebase_admin
            from firebase_admin import credentials
            self.app=firebase_admin.initialize_app(credentials.Certificate(path),name='sentinel-push')
            self.ready=True
        except Exception as error:logging.error('Firebase setup failed: %s',type(error).__name__)

    def send(self,token,event,installation,binding):
        from firebase_admin import messaging
        message=messaging.Message(token=token,data={'event_id':event,'installation_id':installation,'binding':binding},
            android=messaging.AndroidConfig(priority='high',ttl=timedelta(minutes=10)))
        return messaging.send(message,app=self.app)

def process_once(accounts,sender):
    if not sender.ready:return 0
    now=time.time()
    with db() as c:
        schema(c)
        events=c.execute('SELECT * FROM push_events WHERE expanded=0 ORDER BY created LIMIT 50').fetchall()
        for event in events:
            with accounts.db() as a:
                phones=a.execute('''SELECT p.* FROM push_installations p JOIN users u ON u.id=p.owner_id
                    WHERE p.owner_id=? AND p.created<=? AND p.expires>? AND u.active=1 AND u.role='viewer' ''',
                    (event['owner_id'],event['created'],now)).fetchall()
            for phone in phones:
                c.execute('INSERT OR IGNORE INTO push_deliveries(event_id,installation_id,binding) VALUES(?,?,?)',
                          (event['id'],phone['id'],phone['binding']))
            c.execute('UPDATE push_events SET expanded=1 WHERE id=?',(event['id'],))
        pending=c.execute('''SELECT d.*,p.owner_id,p.created FROM push_deliveries d JOIN push_events p ON p.id=d.event_id
                             WHERE d.status='pending' AND d.next_try<=? LIMIT 20''',(now,)).fetchall()
    sent=0
    for delivery in pending:
        event,identity,binding=delivery['event_id'],delivery['installation_id'],delivery['binding']
        # A short lease prevents two dashboard processes from sending concurrently.
        with db() as c:
            c.execute('BEGIN IMMEDIATE')
            claimed=c.execute("UPDATE push_deliveries SET next_try=? WHERE event_id=? AND installation_id=? AND binding=? AND status='pending' AND next_try<=?",
                              (now+120,event,identity,binding,now)).rowcount
        if not claimed:continue
        with accounts.db() as a:
            phone=a.execute('''SELECT p.* FROM push_installations p JOIN users u ON u.id=p.owner_id
                WHERE p.id=? AND p.binding=? AND p.owner_id=? AND p.expires>? AND u.active=1 AND u.role='viewer' ''',
                (identity,binding,delivery['owner_id'],time.time())).fetchone()
        status='cancelled';error_name=None;attempts=delivery['attempts']
        if phone and now-delivery['created']<3600:
            attempts+=1
            try:
                sender.send(phone['token'],event,identity,binding)
                status='sent';sent+=1
            except Exception as error:
                error_name=type(error).__name__
                if error_name in ('UnregisteredError','SenderIdMismatchError'):
                    with accounts.db() as a:a.execute('DELETE FROM push_installations WHERE id=? AND binding=?',(identity,binding))
                    status='invalid_token'
                else:status='failed' if attempts>=5 else 'pending'
                logging.warning('Push delivery failure: %s',error_name)
        with db() as c:
            c.execute('UPDATE push_deliveries SET status=?,attempts=?,next_try=?,last_error=? WHERE event_id=? AND installation_id=? AND binding=?',
                      (status,attempts,time.time()+min(600,15*2**attempts),error_name,event,identity,binding))
    return sent

def install_dispatcher(app):
    app.state.push_sender=FirebaseSender()
    stop=threading.Event()
    def run():
        while not stop.is_set():
            try:process_once(app.state.accounts,app.state.push_sender)
            except Exception as error:logging.error('Push dispatcher error: %s',type(error).__name__)
            stop.wait(3)
    @app.on_event('startup')
    def start():
        if app.state.push_sender.ready:
            app.state.push_thread=threading.Thread(target=run,name='sentinel-push',daemon=True)
            app.state.push_thread.start()
    @app.on_event('shutdown')
    def shutdown():stop.set()
