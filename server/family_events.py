"""Owner-private exemption records, separate from normal deletion and alerts."""
import json
import os
from datetime import datetime,timezone,timedelta
from fastapi import HTTPException,Request,Query
from fastapi.responses import FileResponse
import store
from ownership import current_owner


def _remove_media(c,event):
    report=json.loads(event['report'])
    report['media_deleted']=True;report['photo_url']=None
    for frame in report.get('frames',[]): frame['photo_url']=None
    c.execute('UPDATE events SET report=? WHERE id=?',(json.dumps(report),event['id']))
    return [r['id'] for r in c.execute('SELECT id FROM jobs WHERE event_key=?',(event['id'],))]


def _delete_files(keys):
    for key in keys:
        for directory in (store.INCOMING,store.PHOTOS):
            (directory/(key+'.jpg')).unlink(missing_ok=True)


def cleanup_exempted():
    try:days=int(os.environ.get('FAMILY_EXEMPTION_RETENTION_DAYS','0'))
    except ValueError:return
    if days<=0:return
    cutoff=(datetime.now(timezone.utc)-timedelta(days=days)).isoformat()
    keys=[]
    with store.db() as c:
        c.execute('BEGIN IMMEDIATE')
        for event in c.execute("SELECT * FROM events WHERE state='exempted' AND received<?",(cutoff,)).fetchall():
            keys.extend(_remove_media(c,event))
    _delete_files(keys)


def install_family_events(app):
    @app.get('/api/family/events')
    def events(request:Request,limit:int=Query(50,ge=1,le=100)):
        owner=current_owner(request);cleanup_exempted()
        with store.db() as c:
            rows=c.execute("SELECT * FROM events WHERE owner_id=? AND state='exempted' ORDER BY seq DESC LIMIT ?",(owner,limit)).fetchall()
        return {'items':[store.report_for(row) for row in rows]}

    @app.get('/api/family/events/{event_id}/photo')
    def photo(event_id:str,request:Request,frame_index:int=Query(...,ge=1,le=5)):
        owner=current_owner(request);cleanup_exempted()
        with store.db() as c:
            event=c.execute("SELECT * FROM events WHERE id=? AND owner_id=? AND state='exempted'",(event_id,owner)).fetchone()
            if event is None or json.loads(event['report']).get('media_deleted'):raise HTTPException(404,'Private photo unavailable')
            frame=c.execute('SELECT id FROM jobs WHERE event_key=? AND photo_index=?',(event_id,frame_index)).fetchone()
        path=store.PHOTOS/(frame['id']+'.jpg') if frame else None
        if path is None or not path.is_file():raise HTTPException(404,'Private photo unavailable')
        return FileResponse(path,media_type='image/jpeg',headers={'Cache-Control':'no-store'})

    @app.delete('/api/family/events/{event_id}')
    def delete(event_id:str,request:Request):
        owner=current_owner(request)
        with store.db() as c:
            c.execute('BEGIN IMMEDIATE')
            event=c.execute("SELECT * FROM events WHERE id=? AND owner_id=? AND state='exempted'",(event_id,owner)).fetchone()
            if event is None:raise HTTPException(404,'Private event unavailable')
            keys=_remove_media(c,event)
        _delete_files(keys)
        return {'deleted':True,'event_id':event_id}
