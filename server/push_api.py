"""Authenticated Android subscriptions; data-only FCM never carries photo contents."""
import hashlib
import hmac
import os
import secrets
import time
from pathlib import Path
from fastapi import HTTPException, Request
from pydantic import BaseModel, Field
from ownership import current_owner
from store import db, report_for
from user_auth import COOKIE, IDLE

MAX_AGE=90*86400

def schema(c):
    c.execute('''CREATE TABLE IF NOT EXISTS push_installations (
        id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE,
        secret_digest TEXT NOT NULL, binding TEXT NOT NULL, session_digest TEXT NOT NULL,
        created REAL NOT NULL, updated REAL NOT NULL, expires REAL NOT NULL)''')

def revoke(c, uid=None, session=None):
    if not c.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='push_installations'").fetchone():return
    if uid is not None:c.execute('DELETE FROM push_installations WHERE owner_id=?',(uid,))
    elif session is not None:c.execute('DELETE FROM push_installations WHERE session_digest=?',(session,))

class Registration(BaseModel):
    installation_id:str=Field(pattern=r'^[a-f0-9]{32}$')
    token:str=Field(min_length=20,max_length=4096)
    previous_secret:str=Field(default='',max_length=128)

class Refresh(BaseModel):
    token:str=Field(min_length=20,max_length=4096)

def authenticate_installation(request):
    identity=request.headers.get('x-installation-id','')
    authorization=request.headers.get('authorization','')
    raw=authorization[7:] if authorization.startswith('Bearer ') else ''
    if len(identity)!=32 or not 20<=len(raw)<=128:raise HTTPException(401,'Notification registration expired. Enable alerts again.')
    with request.app.state.accounts.db() as c:
        row=c.execute('''SELECT p.*,u.active,u.role FROM push_installations p JOIN users u ON u.id=p.owner_id
                      WHERE p.id=?''',(identity,)).fetchone()
    if not row or not row['active'] or row['role']!='viewer' or row['expires']<=time.time() or not hmac.compare_digest(row['secret_digest'],hashlib.sha256(raw.encode()).hexdigest()):
        raise HTTPException(401,'Notification registration expired. Enable alerts again.')
    return dict(row)

def install_push_api(app):
    with app.state.accounts.db() as c:schema(c)

    @app.get('/api/push/status')
    def status(request:Request):
        uid=current_owner(request)
        with app.state.accounts.db() as c:
            count=c.execute('SELECT count(*) FROM push_installations WHERE owner_id=? AND expires>?',(uid,time.time())).fetchone()[0]
        return {'configured':app.state.push_sender.ready,'registered_phones':count}

    @app.post('/api/push/register')
    def register(body:Registration,request:Request):
        uid=current_owner(request)
        if not app.state.push_sender.ready:raise HTTPException(503,'Firebase is not configured on the server yet.')
        raw=secrets.token_urlsafe(32);binding=secrets.token_hex(16);now=time.time()
        with app.state.accounts.db() as c:
            c.execute('BEGIN IMMEDIATE')
            session_digest=app.state.accounts.digest(request.cookies[COOKIE])
            valid=c.execute('''SELECT 1 FROM sessions s JOIN users u ON u.id=s.user_id
                WHERE s.digest=? AND s.user_id=? AND s.expires>? AND s.seen>?
                AND u.active=1 AND u.role='viewer' ''',(session_digest,uid,now,now-IDLE)).fetchone()
            if not valid:raise HTTPException(401,'Sign in again before enabling alerts.')
            old=c.execute('SELECT * FROM push_installations WHERE id=?',(body.installation_id,)).fetchone()
            if old and not hmac.compare_digest(old['secret_digest'],hashlib.sha256(body.previous_secret.encode()).hexdigest()):
                raise HTTPException(409,'This phone registration must be disabled before replacing it.')
            collision=c.execute('SELECT id FROM push_installations WHERE token=? AND id!=?',(body.token,body.installation_id)).fetchone()
            if collision:raise HTTPException(409,'This push token is already registered. Disable the previous phone registration first.')
            c.execute('INSERT OR REPLACE INTO push_installations VALUES(?,?,?,?,?,?,?,?,?)',
                      (body.installation_id,uid,body.token,hashlib.sha256(raw.encode()).hexdigest(),binding,
                       session_digest,now,now,now+MAX_AGE))
        return {'installation_id':body.installation_id,'secret':raw,'binding':binding,'username':request.state.user['username'],'user_id':uid}

    @app.post('/api/push/refresh')
    def refresh(body:Refresh,request:Request):
        phone=authenticate_installation(request)
        with app.state.accounts.db() as c:
            c.execute('BEGIN IMMEDIATE')
            if c.execute('SELECT 1 FROM push_installations WHERE token=? AND id!=?',(body.token,phone['id'])).fetchone():
                raise HTTPException(409,'Push token conflict')
            count=c.execute('UPDATE push_installations SET token=?,updated=? WHERE id=? AND binding=?',
                            (body.token,time.time(),phone['id'],phone['binding'])).rowcount
            if not count:raise HTTPException(401,'Registration revoked')
        return {'ok':True}

    @app.post('/api/push/unregister')
    def unregister(request:Request):
        phone=authenticate_installation(request)
        with app.state.accounts.db() as c:
            c.execute('DELETE FROM push_installations WHERE id=? AND binding=?',(phone['id'],phone['binding']))
        return {'ok':True}

    @app.get('/api/push/event/{event_id}')
    def event(event_id:str,request:Request):
        phone=authenticate_installation(request)
        if len(event_id)!=64:raise HTTPException(404,'Report not found')
        with db() as c:
            row=c.execute('''SELECT e.* FROM events e JOIN push_events p ON p.id=e.id
                WHERE e.id=? AND e.owner_id=? AND e.state='suspicious'
                AND p.owner_id=? AND p.created>=?''',(event_id,phone['owner_id'],phone['owner_id'],phone['created'])).fetchone()
            if row is None:
                row=c.execute('''SELECT j.* FROM jobs j JOIN push_events p ON p.id=j.id
                WHERE j.id=? AND j.owner_id=? AND j.state='suspicious'
                AND j.event_key IS NULL AND p.owner_id=? AND p.created>=?''',(event_id,phone['owner_id'],phone['owner_id'],phone['created'])).fetchone()
        if not row:raise HTTPException(404,'Report not found')
        # Recheck after reading the report, so a completed logout/reset cannot
        # authorize a subsequent request with the old installation secret.
        authenticate_installation(request)
        report=report_for(row)
        return {'report_id':row['id'],'timestamp':row['received'],'description':report.get('description','Activity requires review.')[:1000]}

    @app.get('/api/analysis/report/{event_id}')
    def report(event_id:str,request:Request):
        uid=current_owner(request)
        with db() as c:
            row=c.execute("SELECT * FROM events WHERE id=? AND owner_id=? AND state='suspicious'",(event_id,uid)).fetchone()
            if row is None:
                row=c.execute("SELECT * FROM jobs WHERE id=? AND owner_id=? AND state='suspicious' AND event_key IS NULL",(event_id,uid)).fetchone()
            if row is None:
                # Frame IDs returned by uploads can resolve to their one joint report.
                row=c.execute('''SELECT e.* FROM events e JOIN jobs j ON j.event_key=e.id
                    WHERE j.id=? AND j.owner_id=? AND e.owner_id=? AND e.state='suspicious' ''',
                    (event_id,uid,uid)).fetchone()
        if not row:raise HTTPException(404,'Report not found')
        return report_for(row)
