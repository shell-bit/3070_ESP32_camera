"""Private event WAV recordings and short-lived, account-bound speaker audio."""
import hashlib
import io
import re
import secrets
import time
import wave
from pathlib import Path
from contextlib import contextmanager
from fastapi import HTTPException, Request
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field
from store import DATA, ROOT, db, now
from ownership import current_owner, device_owner
from user_auth import COOKIE, IDLE

AUDIO = DATA / 'audio'
AUDIO.mkdir(parents=True, exist_ok=True)
RETENTION = 7 * 86400
MAX_WAV = 16000 * 2 * 5 + 1024
READY_AGE = 8


@contextmanager
def audio_db():
    with db() as c:
        c.execute('''CREATE TABLE IF NOT EXISTS event_audio (
            id TEXT PRIMARY KEY,device TEXT,event TEXT,owner_id INTEGER,digest TEXT,
            received TEXT,created REAL,duration REAL,UNIQUE(device,event))''')
        c.execute('''CREATE TABLE IF NOT EXISTS audio_presence (
            device TEXT PRIMARY KEY,owner_id INTEGER,seen REAL,ready INTEGER)''')
        c.execute('''CREATE TABLE IF NOT EXISTS talk_sessions (
            id TEXT PRIMARY KEY,device TEXT,owner_id INTEGER,session_digest TEXT,
            token_digest TEXT,created REAL,updated REAL,active INTEGER)''')
        c.execute('''CREATE TABLE IF NOT EXISTS talk_chunks (
            id TEXT PRIMARY KEY,session_id TEXT,sequence INTEGER,created REAL,
            state TEXT,payload BLOB,UNIQUE(session_id,sequence))''')
        yield c


def cleanup(c):
    clock = time.time()
    c.execute('DELETE FROM talk_chunks WHERE created<?', (clock-30,))
    c.execute('DELETE FROM talk_sessions WHERE updated<?', (clock-300,))
    rows = c.execute('SELECT id FROM event_audio WHERE created<?', (clock-RETENTION,)).fetchall()
    for row in rows:
        (AUDIO/(row['id']+'.wav')).unlink(missing_ok=True)
    c.execute('DELETE FROM event_audio WHERE created<?', (clock-RETENTION,))


async def read_limited(request, maximum):
    data = bytearray()
    async for chunk in request.stream():
        if len(data)+len(chunk)>maximum: raise HTTPException(413,'Audio is too large.')
        data.extend(chunk)
    return bytes(data)


def validate_wav(data):
    try:
        with wave.open(io.BytesIO(data),'rb') as w:
            frames=w.getnframes()
            if (w.getnchannels(),w.getsampwidth(),w.getframerate(),w.getcomptype())!=(1,2,16000,'NONE'):
                raise ValueError()
            if not 1600<=frames<=80000 or len(w.readframes(frames))!=frames*2:raise ValueError()
            return frames/16000
    except (wave.Error,EOFError,ValueError):
        raise HTTPException(422,'Expected complete mono 16-bit PCM WAV, 16000 Hz, up to 5 seconds.')


def registered(request, device, c):
    owner = device_owner(request,device,c)
    row=c.execute('SELECT * FROM devices WHERE device=?',(device,)).fetchone()
    if not row or owner is None: raise HTTPException(403,'Assign this camera to a user and configure its token first.')
    return row


def own_camera(request, device, c):
    owner=current_owner(request)
    row=c.execute('SELECT * FROM devices WHERE device=? AND owner_id=?',(device,owner)).fetchone()
    if not row:raise HTTPException(404,'Camera unavailable for this account.')
    return row


def live_session(request, c, session, camera):
    clock=time.time()
    if not session or not session['active'] or session['updated']<clock-15 or session['created']<clock-120:
        return False
    if session['owner_id']!=camera['owner_id'] or session['token_digest']!=camera['token_digest']:
        return False
    with request.app.state.accounts.db() as a:
        return bool(a.execute('''SELECT 1 FROM sessions s JOIN users u ON u.id=s.user_id
            WHERE s.digest=? AND s.user_id=? AND s.expires>? AND s.seen>?
            AND u.active=1 AND u.role='viewer' ''',
            (session['session_digest'],session['owner_id'],clock,clock-IDLE)).fetchone())


class StartTalk(BaseModel):
    device_id:str=Field(pattern=r'^[A-Za-z0-9_-]{1,40}$')


def install_audio_api(app):
    with audio_db():pass

    @app.get('/audio.js')
    def script():return FileResponse(ROOT/'audio.js',media_type='text/javascript')

    @app.get('/audio-worklet.js')
    def worklet():return FileResponse(ROOT/'audio-worklet.js',media_type='text/javascript')

    @app.post('/api/device/audio',status_code=201)
    async def upload_audio(request:Request):
        device=request.headers.get('x-device-id','');event=request.headers.get('x-event-id','')
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,48}',event):raise HTTPException(400,'Invalid event ID.')
        with audio_db() as c:registered(request,device,c)
        if request.headers.get('content-type','').split(';')[0]!='audio/wav':raise HTTPException(415,'Send audio/wav.')
        data=await read_limited(request,MAX_WAV);duration=validate_wav(data)
        digest=hashlib.sha256(data).hexdigest();key=hashlib.sha256((device+'\0'+event).encode()).hexdigest()
        with audio_db() as c:
            c.execute('BEGIN IMMEDIATE');camera=registered(request,device,c);cleanup(c)
            jobs=c.execute('SELECT owner_id FROM jobs WHERE device=? AND event=?',(device,event)).fetchall()
            if not jobs or any(j['owner_id']!=camera['owner_id'] for j in jobs):
                raise HTTPException(409,'Upload event photos first; do not reuse events after reassignment.')
            old=c.execute('SELECT * FROM event_audio WHERE id=?',(key,)).fetchone()
            if old:
                if old['owner_id']!=camera['owner_id'] or old['digest']!=digest:raise HTTPException(409,'Event audio differs.')
                return {'saved':True,'duplicate':True,'id':key}
            if c.execute('SELECT count(*) FROM event_audio').fetchone()[0]>=2000:
                raise HTTPException(503,'Audio storage is full. Remove older recordings before retrying.')
            path=AUDIO/(key+'.wav');temp=path.with_suffix('.tmp')
            temp.write_bytes(data);temp.replace(path)
            c.execute('INSERT INTO event_audio VALUES(?,?,?,?,?,?,?,?)',
                      (key,device,event,camera['owner_id'],digest,now(),time.time(),duration))
        return {'saved':True,'id':key,'duration':duration}

    @app.get('/api/audio/recordings')
    def recordings(request:Request,before:float=1e20):
        owner=current_owner(request)
        with audio_db() as c:
            cleanup(c)
            rows=c.execute('SELECT * FROM event_audio WHERE owner_id=? AND created<? ORDER BY created DESC LIMIT 51',
                           (owner,before)).fetchall()
            items=[]
            for r in rows[:50]:
                jobs=c.execute('SELECT id,event_key,state FROM jobs WHERE owner_id=? AND device=? AND event=? ORDER BY seq',
                               (owner,r['device'],r['event'])).fetchall()
                items.append({'id':r['id'],'device_id':r['device'],'event_id':r['event'],
                    'timestamp':r['received'],'duration':r['duration'],'url':'/api/audio/recordings/'+r['id'],
                    'report_ids':list(dict.fromkeys(j['event_key'] or j['id'] for j in jobs if j['state']=='suspicious')),
                    'review_ids':list(dict.fromkeys(j['event_key'] or j['id'] for j in jobs if j['state']=='review'))})
            return {'items':items,'next_cursor':rows[49]['created'] if len(rows)>50 else None,'retention_days':7}

    @app.get('/api/audio/recordings/{key}')
    def recording(key:str,request:Request):
        owner=current_owner(request)
        if not re.fullmatch('[a-f0-9]{64}',key):raise HTTPException(404)
        with audio_db() as c:
            row=c.execute('SELECT * FROM event_audio WHERE id=? AND owner_id=? AND created>?',
                          (key,owner,time.time()-RETENTION)).fetchone()
        if not row or not (AUDIO/(key+'.wav')).is_file():raise HTTPException(404)
        return FileResponse(AUDIO/(key+'.wav'),media_type='audio/wav',headers={'Cache-Control':'no-store'})

    @app.get('/api/audio/devices')
    def devices(request:Request):
        owner=current_owner(request)
        with audio_db() as c:
            rows=c.execute('''SELECT d.device,p.seen,p.ready,p.owner_id FROM devices d
                LEFT JOIN audio_presence p ON d.device=p.device WHERE d.owner_id=? ORDER BY d.device''',(owner,)).fetchall()
            return {'devices':[{'id':r['device'],'available':bool(r['ready'] and r['owner_id']==owner and
                r['seen'] and r['seen']>time.time()-READY_AGE)} for r in rows]}

    @app.post('/api/audio/talk/start')
    def start(body:StartTalk,request:Request):
        with audio_db() as c:
            c.execute('BEGIN IMMEDIATE');cleanup(c);camera=own_camera(request,body.device_id,c)
            p=c.execute('SELECT * FROM audio_presence WHERE device=?',(body.device_id,)).fetchone()
            if not p or not p['ready'] or p['owner_id']!=camera['owner_id'] or p['seen']<time.time()-READY_AGE:
                raise HTTPException(409,'Camera is asleep, busy or offline. Trigger the PIR and wait for Ready.')
            for old in c.execute('SELECT * FROM talk_sessions WHERE device=? AND active=1',(body.device_id,)):
                if live_session(request,c,old,camera):raise HTTPException(409,'This camera is already in a talk session.')
            sid=secrets.token_hex(16)
            c.execute('INSERT INTO talk_sessions VALUES(?,?,?,?,?,?,?,1)',(sid,body.device_id,camera['owner_id'],
                request.app.state.accounts.digest(request.cookies[COOKIE]),camera['token_digest'],time.time(),time.time()))
        return {'session_id':sid,'sample_rate':16000,'max_seconds':120}

    @app.post('/api/audio/talk/{sid}')
    async def send(sid:str,request:Request):
        owner=current_owner(request)
        sequence=request.headers.get('x-audio-sequence','')
        if not re.fullmatch(r'[0-9]{1,4}',sequence):raise HTTPException(400,'Invalid audio sequence.')
        data=await read_limited(request,32000)
        if not data or len(data)%2:raise HTTPException(422,'Send signed 16-bit little-endian mono PCM, 16000 Hz.')
        with audio_db() as c:
            c.execute('BEGIN IMMEDIATE');cleanup(c)
            s=c.execute('SELECT * FROM talk_sessions WHERE id=? AND owner_id=?',(sid,owner)).fetchone()
            if not s:raise HTTPException(404)
            camera=own_camera(request,s['device'],c)
            if s['session_digest']!=request.app.state.accounts.digest(request.cookies[COOKIE]) or not live_session(request,c,s,camera):
                raise HTTPException(409,'Talk session expired. Start again.')
            p=c.execute('SELECT * FROM audio_presence WHERE device=?',(s['device'],)).fetchone()
            if not p or not p['ready'] or p['seen']<time.time()-READY_AGE:raise HTTPException(409,'Camera is unavailable.')
            old=c.execute('SELECT * FROM talk_chunks WHERE session_id=? AND sequence=?',(sid,int(sequence))).fetchone()
            if old:return {'queued':True,'duplicate':True}
            if c.execute("SELECT count(*) FROM talk_chunks WHERE session_id=? AND state='queued' AND created>?",(sid,time.time()-8)).fetchone()[0]>=3:
                raise HTTPException(429,'Speaker connection is too slow. Stop and try again.')
            key=secrets.token_hex(16)
            c.execute('INSERT INTO talk_chunks VALUES(?,?,?,?,?,?)',(key,sid,int(sequence),time.time(),'queued',data))
            c.execute('UPDATE talk_sessions SET updated=? WHERE id=?',(time.time(),sid))
        return {'queued':True}

    @app.post('/api/audio/talk/{sid}/stop')
    def stop(sid:str,request:Request):
        owner=current_owner(request)
        with audio_db() as c:
            s=c.execute('SELECT * FROM talk_sessions WHERE id=? AND owner_id=?',(sid,owner)).fetchone()
            if not s:raise HTTPException(404)
            if s['session_digest']!=request.app.state.accounts.digest(request.cookies[COOKIE]):raise HTTPException(403)
            c.execute('UPDATE talk_sessions SET active=0 WHERE id=?',(sid,))
            c.execute('DELETE FROM talk_chunks WHERE session_id=?',(sid,))
        return {'stopped':True}

    @app.get('/api/audio/talk/{sid}/status')
    def talk_status(sid:str,request:Request):
        owner=current_owner(request)
        with audio_db() as c:
            s=c.execute('SELECT * FROM talk_sessions WHERE id=? AND owner_id=?',(sid,owner)).fetchone()
            if not s:raise HTTPException(404)
            counts={r['state']:r['n'] for r in c.execute('SELECT state,count(*) n FROM talk_chunks WHERE session_id=? GROUP BY state',(sid,))}
        return {'active':bool(s['active']),'chunks':counts}

    @app.get('/api/device/talk')
    def poll(request:Request,device_id:str,ready:bool=True):
        with audio_db() as c:
            c.execute('BEGIN IMMEDIATE');camera=registered(request,device_id,c);cleanup(c)
            c.execute('INSERT OR REPLACE INTO audio_presence VALUES(?,?,?,?)',(device_id,camera['owner_id'],time.time(),int(ready)))
            if not ready:
                c.execute('DELETE FROM talk_chunks WHERE session_id IN (SELECT id FROM talk_sessions WHERE device=?)',(device_id,))
                c.execute('UPDATE talk_sessions SET active=0 WHERE device=?',(device_id,))
            if ready:
                rows=c.execute("""SELECT t.* FROM talk_chunks t JOIN talk_sessions s ON s.id=t.session_id
                    WHERE s.device=? AND s.active=1 AND t.state='queued' AND t.created>?
                    ORDER BY t.created,t.sequence""",(device_id,time.time()-8)).fetchall()
                for chunk in rows:
                    s=c.execute('SELECT * FROM talk_sessions WHERE id=?',(chunk['session_id'],)).fetchone()
                    if not live_session(request,c,s,camera):continue
                    # At-most-once delivery: a lost response does not replay a warning later.
                    c.execute("UPDATE talk_chunks SET state='delivered',payload=NULL WHERE id=?",(chunk['id'],))
                    return Response(bytes(chunk['payload']),media_type='application/octet-stream',
                                    headers={'X-Talk-ID':chunk['id'],'Cache-Control':'no-store'})
        return Response(status_code=204,headers={'Cache-Control':'no-store'})

    @app.post('/api/device/talk/ack')
    def ack(request:Request):
        with audio_db() as c:
            camera=registered(request,request.headers.get('x-device-id',''),c)
            key=request.headers.get('x-talk-id','')
            c.execute('''UPDATE talk_chunks SET state=? WHERE id=? AND state='delivered' AND session_id IN
                (SELECT id FROM talk_sessions WHERE device=? AND owner_id=? AND token_digest=?)''',
                ('played' if request.headers.get('x-playback-ok')=='true' else 'failed',key,camera['device'],camera['owner_id'],camera['token_digest']))
        return {'ok':True}
