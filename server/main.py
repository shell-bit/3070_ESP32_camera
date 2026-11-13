import asyncio
import hashlib
import os
import re
import secrets
import json
import cv2
import numpy as np
import uvicorn
from fastapi import FastAPI, Request, HTTPException, Query
from fastapi.responses import FileResponse, RedirectResponse
from pydantic import BaseModel, Field
from typing import Literal
from datetime import datetime
from urllib.parse import urlsplit
from manual_review import decide, ReviewConflict
from store import ROOT, INCOMING, PHOTOS, db, now, report_for
from ownership import current_owner, device_owner, install_ownership
from event_queue import ensure_event, register_frame, frames_for, expire_events, EventConflict

app = FastAPI(title='Surveillance Dashboard')
UPLOAD_LOCK = asyncio.Lock()
UPLOAD_TOKEN = os.environ.get('UPLOAD_TOKEN', '')
MAX_PENDING = int(os.environ.get('MAX_PENDING', '200'))

def validate(data):
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        raise HTTPException(422, 'Incomplete JPEG')
    image = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None or image.shape[0] * image.shape[1] > 12000000:
        raise HTTPException(422, 'Invalid image or dimensions too large')

@app.post('/api/device/upload', status_code=201)
async def upload(request: Request):
    if request.headers.get('content-type', '').split(';')[0].lower() != 'image/jpeg':
        raise HTTPException(415, 'Send image/jpeg')
    device=request.headers.get('x-device-id','')
    event=request.headers.get('x-event-id','')
    index=request.headers.get('x-photo-index','')
    expected=request.headers.get('x-expected-photos','5')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',device) or not re.fullmatch(r'[A-Za-z0-9_-]{1,48}',event) or index not in ('1','2','3','4','5'):
        raise HTTPException(400,'Missing/invalid device, event or photo index headers')
    if expected not in ('1','2','3','4','5') or int(index)>int(expected):
        raise HTTPException(400,'Expected photos must be 1-5 and include this frame index')
    captured_at=request.headers.get('x-captured-at')
    if captured_at is not None:
        try:
            if len(captured_at)>64:raise ValueError()
            captured=datetime.fromisoformat(captured_at.replace('Z','+00:00'))
            if captured.utcoffset() is None:raise ValueError()
            captured_at=captured.isoformat()
        except ValueError:
            raise HTTPException(400,'X-Captured-At must be an ISO 8601 timestamp with timezone')
    with db() as connection:
        device_owner(request,device,connection)
    data=bytearray()
    async for chunk in request.stream():
        if len(data)+len(chunk)>4*1024*1024:
            raise HTTPException(413,'Maximum 4 MiB per photo')
        data.extend(chunk)
    await asyncio.to_thread(validate,data)
    key=hashlib.sha256(json.dumps([device,event,index]).encode()).hexdigest()
    digest=hashlib.sha256(data).hexdigest()
    expire_events()
    async with UPLOAD_LOCK:
        with db() as connection:
            connection.execute('BEGIN IMMEDIATE')
            owner=device_owner(request,device,connection)
            if connection.execute('SELECT 1 FROM event_identity_protocol p JOIN events e ON e.id=p.event_key WHERE e.device=? AND e.event=?',(device,event)).fetchone():
                raise HTTPException(409,'V2 event requires immutable image and identity upload')
            existing=connection.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone()
            if existing:
                if existing['owner_id']!=owner:
                    raise HTTPException(409,'Use a new event ID after camera reassignment.')
                if existing['digest']!=digest:
                    raise HTTPException(409,'Event/index reused for different photo')
                if existing['captured_at']!=captured_at:
                    raise HTTPException(409,'Frame capture timestamp changed on retry')
                old_event=connection.execute('SELECT * FROM events WHERE id=?',(existing['event_key'],)).fetchone()
                if old_event and old_event['expected_frames']!=int(expected):
                    raise HTTPException(409,'Expected frame count changed on retry')
                return {'saved':True,'job_id':key,'event_key':existing['event_key'],
                        'analysis_status':old_event['state'] if old_event else existing['state'],'duplicate':True}
            count=connection.execute("SELECT COUNT(*) FROM jobs WHERE state IN ('collecting','queued','processing','review')").fetchone()[0]
            if count>=MAX_PENDING:
                raise HTTPException(503,'Analysis/review queue full; inspect worker and review folder')
            try:
                event_row=ensure_event(connection,device,event,owner,expected_frames=int(expected))
            except EventConflict as error:
                raise HTTPException(409,str(error))
            path=INCOMING/(key+'.jpg')
            temp=path.with_suffix('.tmp')
            with temp.open('wb') as f:
                f.write(data); f.flush(); os.fsync(f.fileno())
            temp.replace(path)
            connection.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,owner_id,event_key,captured_at,state) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                               (key,digest,device,event,int(index),int(request.headers.get('x-night-capture')=='true'),now(),owner,event_row['id'],captured_at,'collecting'))
            try:
                state=register_frame(connection,event_row['id'])
            except EventConflict as error:
                # The deadline can elapse during fsync; roll back this new frame.
                path.unlink(missing_ok=True)
                raise HTTPException(409,str(error))
    return {'saved':True,'job_id':key,'event_key':event_row['id'],'analysis_status':state}

@app.get('/')
def dashboard(request:Request):
    if request.state.user['role']=='admin': return RedirectResponse('/admin',303)
    return FileResponse(ROOT/'index.html')

@app.get('/review.js')
def review_script():
    return FileResponse(ROOT/'review.js',media_type='text/javascript')

@app.get('/api/device/status')
def device_status(request:Request):
    uid=current_owner(request)
    with db() as connection:
        row=connection.execute('SELECT * FROM jobs WHERE owner_id=? ORDER BY seq DESC LIMIT 1',(uid,)).fetchone()
    return {'last_received':row['received'] if row else None,'device_id':row['device'] if row else None,
            'photo_index':row['photo_index'] if row else None,'night':bool(row['night']) if row else False}

@app.get('/api/health')
def health(request:Request):
    if request.state.user['role']=='admin': return {'server':'running','role':'admin'}
    uid=current_owner(request)
    expire_events()
    with db() as connection:
        counts={r['state']:r['n'] for r in connection.execute('SELECT state,COUNT(*) n FROM jobs WHERE owner_id=? GROUP BY state',(uid,))}
        events={r['state']:r['n'] for r in connection.execute('SELECT state,COUNT(*) n FROM events WHERE owner_id=? GROUP BY state',(uid,))}
        legacy={r['state']:r['n'] for r in connection.execute('SELECT state,COUNT(*) n FROM jobs WHERE owner_id=? AND event_key IS NULL GROUP BY state',(uid,))}
    return {'server':'running','jobs':counts,'events':events,'legacy_jobs':legacy,'upload_auth_enabled':bool(UPLOAD_TOKEN),
            'inference_mode':os.environ.get('AI_RUNTIME_MODE','gpu_required')}

def report_rows(connection,owner,before=9223372036854775807,limit=1):
    # Existing job sequence cursors remain valid; an event uses its first frame.
    return connection.execute('''SELECT * FROM (
        SELECT id,received,report,seq AS sort_seq FROM jobs
        WHERE owner_id=? AND state='suspicious' AND event_key IS NULL
        UNION ALL
        SELECT e.id,e.received,e.report,(SELECT MIN(j.seq) FROM jobs j WHERE j.event_key=e.id) AS sort_seq
        FROM events e WHERE e.owner_id=? AND e.state='suspicious'
        ) WHERE sort_seq<? ORDER BY sort_seq DESC LIMIT ?''',(owner,owner,before,limit)).fetchall()

@app.get('/api/analysis/latest')
def latest(request:Request):
    uid=current_owner(request)
    with db() as connection:
        rows=report_rows(connection,uid)
        row=rows[0] if rows else None
    return report_for(row) if row else {'analysis_status':'pending','description':'No suspicious activity reports yet. Processing and review counts appear below.','timestamp':None}

@app.get('/api/analysis/history')
def history(request:Request,limit:int=Query(100,ge=1,le=100),before:int|None=Query(None,ge=1)):
    uid=current_owner(request)
    with db() as connection:
        rows=report_rows(connection,uid,before or 9223372036854775807,limit+1)
    return {'items':[report_for(r) for r in rows[:limit]],'next_cursor':str(rows[limit-1]['sort_seq']) if len(rows)>limit else None}

@app.get('/api/photos')
def photos(request:Request):
    uid=current_owner(request)
    with db() as connection:
        rows=connection.execute("SELECT * FROM jobs WHERE state='suspicious' AND owner_id=? ORDER BY seq DESC",(uid,)).fetchall()
    return [{'name':r['id']+'.jpg','url':'/photos/'+r['id']+'.jpg',
             'download_url':'/photos/'+r['id']+'.jpg?download=true','captured_at':r['received']} for r in rows]

@app.get('/photos/{name}')
def photo(name:str,request:Request,download:bool=False):
    uid=current_owner(request)
    if not re.fullmatch(r'[a-f0-9]{64}\.jpg',name):
        raise HTTPException(404)
    with db() as connection:
        row=connection.execute("SELECT id FROM jobs WHERE id=? AND state='suspicious' AND owner_id=?",(name[:-4],uid)).fetchone()
    if not row or not (PHOTOS/name).is_file():
        raise HTTPException(404)
    return FileResponse(PHOTOS/name,media_type='image/jpeg',filename=name if download else None)

class ReviewDecision(BaseModel):
    action: Literal['keep','dismiss','retry']
    description: str = Field(default='',max_length=2000)

def review_access(request):
    return current_owner(request)

@app.get('/api/review')
def review_list(request:Request):
    uid=review_access(request)
    expire_events()
    with db() as connection:
        rows=connection.execute("SELECT * FROM jobs WHERE state='review' AND owner_id=? AND event_key IS NULL ORDER BY seq DESC",(uid,)).fetchall()
        events=connection.execute("SELECT * FROM events WHERE state='review' AND owner_id=? ORDER BY seq DESC",(uid,)).fetchall()
        items=[]
        for event in events:
            frames=frames_for(connection,event['id'])
            present={f['photo_index'] for f in frames}
            missing=[i for i in range(1,event['expected_frames']+1) if i not in present]
            photos=[{'id':f['id'],'photo_index':f['photo_index'],'captured_at':f['captured_at'],
                     'received_at':f['received'],'photo_url':'/api/review/'+event['id']+'/photo?frame_index='+str(f['photo_index'])} for f in frames]
            report=report_for(event) or {}
            items.append({'id':event['id'],'event_id':event['event'],'received_at':event['received'],
                          'description':event['error'] or report.get('description') or 'AI could not complete analysis',
                          'device_id':event['device'],'frame_count':len(frames),'expected_frames':event['expected_frames'],
                          'input_complete':not missing,'missing_indices':missing,'frames':photos,
                          'evidence_frame_indices':report.get('evidence_frame_indices',[]),
                          'photo_url':photos[0]['photo_url'] if photos else None})
    items.extend([{'id':r['id'],'received_at':r['received'],'description':r['error'] or 'AI could not complete analysis',
                      'device_id':r['device'],'photo_index':r['photo_index'],
                      'photo_url':'/api/review/'+r['id']+'/photo'} for r in rows])
    return {'items':sorted(items,key=lambda r:r['received_at'],reverse=True)}

@app.get('/api/review/{job_id}/photo')
def review_photo(job_id:str,request:Request,frame_index:int|None=Query(None,ge=1,le=5)):
    uid=review_access(request)
    if not re.fullmatch(r'[a-f0-9]{64}',job_id): raise HTTPException(404)
    with db() as connection:
        event=connection.execute("SELECT id FROM events WHERE id=? AND state='review' AND owner_id=?",(job_id,uid)).fetchone()
        if event:
            row=connection.execute('SELECT id FROM jobs WHERE event_key=? AND owner_id=? AND (? IS NULL OR photo_index=?) ORDER BY photo_index LIMIT 1',
                                   (job_id,uid,frame_index,frame_index)).fetchone()
        else:
            row=connection.execute("SELECT id FROM jobs WHERE id=? AND state='review' AND owner_id=? AND event_key IS NULL",(job_id,uid)).fetchone()
    if not row: raise HTTPException(404,'Photo no longer awaiting review')
    job_id=row['id']
    path=INCOMING/(job_id+'.jpg')
    if not path.is_file(): path=PHOTOS/(job_id+'.jpg')
    if not path.is_file(): raise HTTPException(404,'Photo file is missing')
    return FileResponse(path,media_type='image/jpeg',headers={'Cache-Control':'no-store'})

@app.post('/api/review/{job_id}')
def review_decision(job_id:str,body:ReviewDecision,request:Request):
    uid=review_access(request)
    origin=request.headers.get('origin')
    if origin and urlsplit(origin).netloc!=request.headers.get('host'):
        raise HTTPException(403,'Use the dashboard on this server')
    if not re.fullmatch(r'[a-f0-9]{64}',job_id): raise HTTPException(404)
    description=body.description.strip()
    if body.action=='keep' and not description:
        raise HTTPException(422,'Add a description of what you observed')
    try: return decide(job_id,body.action,description or ('Dismissed by reviewer' if body.action=='dismiss' else 'AI retry requested by reviewer'),owner_id=uid)
    except ReviewConflict as error: raise HTTPException(409,str(error))
    except FileNotFoundError as error: raise HTTPException(404,str(error))

from device_alarm_api import install_device_alarm_api
from store import db as alarm_db
install_device_alarm_api(app, alarm_db)

from user_auth import install_user_auth
from store import DATA
install_user_auth(app, ROOT, DATA)
install_ownership(app)
from push_dispatcher import install_dispatcher
from push_api import install_push_api
install_dispatcher(app)
install_push_api(app)

from audio_api import install_audio_api
install_audio_api(app)

from identity_ingest import install_identity_api
install_identity_api(app, UPLOAD_LOCK)
from member_api import install_member_api
from device_commands import install_device_commands
from family_events import install_family_events
install_member_api(app)
install_device_commands(app)
install_family_events(app)

if __name__=='__main__':
    uvicorn.run(app,host='0.0.0.0',port=8000)
