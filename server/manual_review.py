"""Manual decisions for review-state jobs only, compatible with existing queue DB."""
import json
from store import db, INCOMING, PHOTOS, now

class ReviewConflict(Exception): pass

def decide(job_id, action, description, owner_id=None):
    # A direct frame ID must never resolve only one frame of a grouped event.
    with db() as connection:
        event=connection.execute('SELECT id FROM events WHERE id=?',(job_id,)).fetchone()
        frame=connection.execute('SELECT event_key FROM jobs WHERE id=?',(job_id,)).fetchone()
        event_key=event['id'] if event is not None else (frame['event_key'] if frame is not None else None)
    if event_key is not None:
        from event_queue import decide_event
        return decide_event(event_key, action, description, owner_id)
    # Lock before reading: two reviewers cannot resolve the same job concurrently.
    with db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        row=connection.execute('SELECT * FROM jobs WHERE id=?',(job_id,)).fetchone()
        if owner_id is not None and (row is None or row['owner_id']!=owner_id):
            raise FileNotFoundError('Photo not found.')
        if row is None or row['state']!='review':
            raise ReviewConflict('This photo is no longer awaiting review. Refresh the list.')
        source=INCOMING/(job_id+'.jpg')
        target=PHOTOS/(job_id+'.jpg')
        if action!='dismiss' and not source.is_file() and not target.is_file():
            raise FileNotFoundError('Photo file is missing. Retry/keep is unavailable.')
        reviewed_at=now()
        report={'id':job_id,'report_id':job_id,'status':'suspicious' if action=='keep' else 'normal',
                'analysis_status':'completed','review_source':'human','reviewed_at':reviewed_at,
                'description':description,'timestamp':row['received'],'detected_at':row['received'],
                'device_id':row['device'],'photo_name':job_id+'.jpg','photo_url':'/photos/'+job_id+'.jpg'}
        connection.execute('CREATE TABLE IF NOT EXISTS manual_decisions (seq INTEGER PRIMARY KEY AUTOINCREMENT, job_id TEXT, action TEXT, description TEXT, reviewed_at TEXT, original_error TEXT)')
        connection.execute('INSERT INTO manual_decisions(job_id,action,description,reviewed_at,original_error) VALUES(?,?,?,?,?)',
                           (job_id,action,description,reviewed_at,row['error']))
        if action=='keep':
            report['activity_type']='suspicious'
            if source.is_file(): source.replace(target)
            state='suspicious'
        elif action=='retry':
            state='queued'
        else: state='discarded'
        connection.execute('UPDATE jobs SET state=?,report=?,error=NULL WHERE id=?',
                           (state,json.dumps(report) if action!='retry' else None,job_id))
        if state=='suspicious':
            from push_events import enqueue
            enqueue(connection,job_id)
    # Commit the discard first so restart cleanup can finish an interrupted delete.
    if action=='dismiss':
        source.unlink(missing_ok=True)
        target.unlink(missing_ok=True)
    return {'accepted':True,'state':state,'job_id':job_id}
