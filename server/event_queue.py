"""Transactional, whole-event queue layered over unchanged legacy frame jobs."""
import hashlib
import json
import math
import os
import time

import store


class EventConflict(ValueError):
    """An upload or result conflicts with an event's frozen metadata/state."""


def schema(connection):
    connection.execute('''CREATE TABLE IF NOT EXISTS events (
        seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
        device TEXT NOT NULL, event TEXT NOT NULL, owner_id INTEGER,
        expected_frames INTEGER NOT NULL, received TEXT NOT NULL,
        deadline REAL NOT NULL, state TEXT NOT NULL DEFAULT 'collecting',
        report TEXT, error TEXT, UNIQUE(device,event))''')
    connection.execute('CREATE INDEX IF NOT EXISTS events_state_seq ON events(state,seq)')
    connection.execute('CREATE INDEX IF NOT EXISTS events_owner_state ON events(owner_id,state,seq)')


def event_id(device, event):
    return hashlib.sha256(json.dumps(['event', device, event]).encode()).hexdigest()


def ensure_event(connection, device, event, owner_id, expected_frames=5):
    if type(expected_frames) is not int or not 1 <= expected_frames <= 5:
        raise EventConflict('Expected photos must be an integer from 1 to 5.')
    if connection.execute('SELECT 1 FROM jobs WHERE device=? AND event=? AND event_key IS NULL LIMIT 1',
                          (device, event)).fetchone() is not None:
        raise EventConflict('Historical single-frame jobs use this event ID; start a new event ID.')
    row = connection.execute('SELECT * FROM events WHERE device=? AND event=?', (device, event)).fetchone()
    if row is not None:
        if row['owner_id'] != owner_id or row['expected_frames'] != expected_frames:
            raise EventConflict('Event owner and expected photos cannot change; use a new event ID.')
        if row['state'] != 'collecting':
            raise EventConflict('Event is closed to new frames.')
        if time.time() >= row['deadline']:
            raise EventConflict('Event collection deadline has passed.')
        return dict(row)
    wait = float(os.environ.get('EVENT_WAIT_SECONDS', '30'))
    if not math.isfinite(wait) or wait <= 0:
        raise ValueError('EVENT_WAIT_SECONDS must be finite and positive.')
    key = event_id(device, event)
    connection.execute('''INSERT INTO events
        (id,device,event,owner_id,expected_frames,received,deadline,state)
        VALUES(?,?,?,?,?,?,?,?)''',
        (key, device, event, owner_id, expected_frames, store.now(), time.time() + wait, 'collecting'))
    return dict(connection.execute('SELECT * FROM events WHERE id=?', (key,)).fetchone())


def frames_for(connection, event_key):
    return [dict(row) for row in connection.execute(
        'SELECT * FROM jobs WHERE event_key=? ORDER BY photo_index,seq', (event_key,))]


def _missing(event, frames):
    available = {frame['photo_index'] for frame in frames}
    return [index for index in range(1, event['expected_frames'] + 1) if index not in available]


def register_frame(connection, event_key):
    row = connection.execute('SELECT * FROM events WHERE id=?', (event_key,)).fetchone()
    if row is None or row['state'] != 'collecting':
        raise EventConflict('Event is closed to new frames.')
    if time.time() >= row['deadline']:
        raise EventConflict('Event collection deadline has passed.')
    frames = frames_for(connection, event_key)
    indices = [frame['photo_index'] for frame in frames]
    if any(type(index) is not int or not 1 <= index <= row['expected_frames'] for index in indices):
        raise EventConflict('Frame index exceeds the configured event size.')
    if len(set(indices)) != len(indices):
        raise EventConflict('An event cannot contain duplicate frame indices.')
    if any(frame['owner_id'] != row['owner_id'] or frame['device'] != row['device'] or
           frame['event'] != row['event'] for frame in frames):
        raise EventConflict('Frame metadata does not match the event.')
    state = 'queued' if not _missing(row, frames) else 'collecting'
    connection.execute('UPDATE events SET state=? WHERE id=?', (state, event_key))
    connection.execute('UPDATE jobs SET state=? WHERE event_key=?', (state, event_key))
    return state


def _evidence(indices, frames):
    if indices is None:
        return []
    available = {frame['photo_index'] for frame in frames}
    if not isinstance(indices, list) or any(type(index) is not int or index not in available for index in indices):
        raise ValueError('Evidence frame indices must be a list of available integer indices.')
    if len(set(indices)) != len(indices):
        raise ValueError('Evidence frame indices must be unique.')
    return list(indices)


def build_report(event, frames, verdict, description, activity_type=None,
                 evidence_frame_indices=None, review_source=None):
    activity_type = store.normalize_activity_type(verdict, activity_type)
    if not isinstance(description, str) or not description.strip():
        raise ValueError('Assessment description must be a non-empty string.')
    frames = sorted(frames, key=lambda frame: frame['photo_index'])
    evidence = _evidence(evidence_frame_indices, frames)
    missing = _missing(event, frames)
    public = verdict == 'suspicious'
    details = []
    for frame in frames:
        url = ('/photos/' + frame['id'] + '.jpg') if public else (
            '/api/review/' + event['id'] + '/photo?frame_index=' + str(frame['photo_index']))
        details.append({'id': frame['id'], 'photo_index': frame['photo_index'],
                        'captured_at': frame.get('captured_at'), 'received_at': frame['received'],
                        'photo_url': url})
    representative = next((frame for frame in details if evidence and frame['photo_index'] == evidence[0]),
                          details[0] if details else None)
    analyzed_at = store.now()
    report = {'id': event['id'], 'report_id': event['id'], 'status': verdict,
              'analysis_status': 'review' if verdict == 'uncertain' else 'completed',
              'description': description[:4000], 'timestamp': event['received'],
              'detected_at': event['received'], 'analyzed_at': analyzed_at,
              'device_id': event['device'], 'event_id': event['event'],
              'expected_frames': event['expected_frames'], 'frame_count': len(frames),
              'input_complete': not missing, 'missing_indices': missing,
              'evidence_frame_indices': evidence, 'frames': details,
              'photo_name': representative['id'] + '.jpg' if representative else None,
              'photo_url': representative['photo_url'] if representative else None}
    if activity_type is not None:
        report['activity_type'] = activity_type
    if review_source is not None:
        report['review_source'] = review_source
        report['reviewed_at'] = analyzed_at
    return report


def _set_state(connection, key, state, report=None, error=None):
    serialized = json.dumps(report) if report is not None else None
    connection.execute('UPDATE events SET state=?,report=?,error=? WHERE id=?', (state, serialized, error, key))
    # The aggregate report belongs only to events; individual frame jobs stay addressable.
    connection.execute('UPDATE jobs SET state=?,report=NULL,error=? WHERE event_key=?', (state, error, key))


def expire_events(connection=None):
    if connection is None:
        with store.db() as own_connection:
            own_connection.execute('BEGIN IMMEDIATE')
            return expire_events(own_connection)
    # Supplied connections are transaction-owned by callers, including legacy API DBs.
    if connection.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='events'").fetchone() is None:
        return 0
    expired = connection.execute("SELECT * FROM events WHERE state='collecting' AND deadline<=? ORDER BY seq",
                                 (time.time(),)).fetchall()
    count = 0
    for row in expired:
        event = dict(row)
        frames = frames_for(connection, event['id'])
        error = 'Event collection timed out; missing frame indices: ' + ', '.join(map(str, _missing(event, frames)))
        report = build_report(event, frames, 'uncertain', error)
        from alarm_policy import decide_in_transaction
        report['model_verdict']='uncertain'
        report['alarm_decision']=decide_in_transaction(connection,event,frames,'uncertain',[])
        updated = connection.execute("UPDATE events SET state='review',report=?,error=? WHERE id=? AND state='collecting'",
                                     (json.dumps(report), error, event['id'])).rowcount
        if updated:
            connection.execute("UPDATE jobs SET state='review',report=NULL,error=? WHERE event_key=?", (error, event['id']))
            count += 1
    return count


def claim_event():
    expire_events()
    with store.db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        row = connection.execute("SELECT * FROM events WHERE state='queued' ORDER BY seq LIMIT 1").fetchone()
        if row is None:
            return None
        key = row['id']
        event = dict(row)
        frames = frames_for(connection, key)
        if _missing(event, frames):
            error = 'Queued event has incomplete input; manual review required.'
            _set_state(connection, key, 'review', build_report(event, frames, 'uncertain', error), error)
            return None
        _set_state(connection, key, 'processing')
        event['state'], event['report'], event['error'] = 'processing', None, None
        return event, frames_for(connection, key)


def _require_files(frames):
    for frame in frames:
        name = frame['id'] + '.jpg'
        if not (store.INCOMING / name).is_file() and not (store.PHOTOS / name).is_file():
            raise FileNotFoundError('Event frame ' + str(frame['photo_index']) + ' is missing. Retry/keep is unavailable.')


def _retain_frames(frames):
    _require_files(frames)
    for frame in frames:
        source, target = store.INCOMING / (frame['id'] + '.jpg'), store.PHOTOS / (frame['id'] + '.jpg')
        if source.is_file():
            source.replace(target)
        if not target.is_file():
            raise FileNotFoundError('Suspicious event photo is missing.')


def _delete_frames(frames):
    # Called only after the discard transaction has committed; recover() repeats cleanup.
    for frame in frames:
        for folder in (store.INCOMING, store.PHOTOS):
            (folder / (frame['id'] + '.jpg')).unlink(missing_ok=True)


def finish_event(event, frames, verdict, description, activity_type=None, evidence_frame_indices=None,
                 actor_assessment=None, tracks=None):
    key = event['id']
    with store.db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        row = connection.execute('SELECT * FROM events WHERE id=?', (key,)).fetchone()
        if row is None or row['state'] != 'processing':
            raise EventConflict('Event is no longer processing; refusing a stale result.')
        event, frames = dict(row), frames_for(connection, key)
        if _missing(event, frames):
            raise EventConflict('Incomplete input requires manual review.')
        report = build_report(event, frames, verdict, description, activity_type, evidence_frame_indices)
        from alarm_policy import decide_in_transaction
        decision=decide_in_transaction(connection,event,frames,verdict,evidence_frame_indices,actor_assessment,tracks)
        report['model_verdict']=verdict
        report['alarm_decision']=decision
        state = {'alert':'suspicious','no_alert':'discarded','review':'review','exempted':'exempted'}[decision['decision']]
        if state=='suspicious':
            report['status']='suspicious'
            report['activity_type']=store.normalize_activity_type('suspicious',activity_type)
            for item in report['frames']:item['photo_url']='/photos/'+item['id']+'.jpg'
            report['photo_url']='/photos/'+report['photo_name'] if report['photo_name'] else None
        if state=='review':
            report['status']='uncertain'; report['analysis_status']='review'
        if state=='exempted':
            report['status']='exempted'
            for item in report['frames']:
                item['photo_url']='/api/family/events/'+key+'/photo?frame_index='+str(item['photo_index'])
            representative=next((item for item in report['frames'] if item['id']+'.jpg'==report['photo_name']),None)
            report['photo_url']=representative['photo_url'] if representative else None
        if state in ('suspicious','exempted'):
            _retain_frames(frames)
        _set_state(connection, key, state, report, description[:4000] if state == 'review' else None)
        if state == 'suspicious':
            from push_events import enqueue_event
            enqueue_event(connection, key)
    if state == 'discarded':
        _delete_frames(frames)
    return report


def fail_event(event_key, error):
    with store.db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        row = connection.execute('SELECT * FROM events WHERE id=?', (event_key,)).fetchone()
        if row is None or row['state'] != 'processing':
            return
        error = str(error)[:4000] or 'Event analysis failed; manual review required.'
        frames=frames_for(connection,event_key)
        report = build_report(dict(row),frames,'uncertain',error)
        from alarm_policy import decide_in_transaction
        report['model_verdict']='uncertain'
        report['alarm_decision']=decide_in_transaction(connection,dict(row),frames,'uncertain',[])
        _set_state(connection, event_key, 'review', report, error)


def recover_events(connection):
    for row in connection.execute("SELECT id FROM events WHERE state='processing'").fetchall():
        _set_state(connection, row['id'], 'queued')


def decide_event(event_key, action, description, owner_id=None):
    from manual_review import ReviewConflict
    if action not in ('keep', 'dismiss', 'retry'):
        raise ValueError('Unknown review action.')
    with store.db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        row = connection.execute('SELECT * FROM events WHERE id=?', (event_key,)).fetchone()
        if owner_id is not None and (row is None or row['owner_id'] != owner_id):
            raise FileNotFoundError('Event not found.')
        if row is None or row['state'] != 'review':
            raise ReviewConflict('This event is no longer awaiting review. Refresh the list.')
        event, frames = dict(row), frames_for(connection, event_key)
        if action == 'retry' and _missing(event, frames):
            raise ReviewConflict('Incomplete events cannot be retried; review or use a new event ID.')
        if action != 'dismiss':
            _require_files(frames)
        report = None if action == 'retry' else build_report(
            event, frames, 'suspicious' if action == 'keep' else 'normal', description, review_source='human')
        state = {'keep': 'suspicious', 'dismiss': 'discarded', 'retry': 'queued'}[action]
        if report is not None:
            from alarm_policy import decide_in_transaction
            # Human keep has no validated actor attribution. It conservatively alerts.
            decision=decide_in_transaction(connection,event,frames,'suspicious' if action=='keep' else 'normal',[])
            report['alarm_decision']=decision
            report['model_verdict']='suspicious' if action=='keep' else 'normal'
        connection.execute('CREATE TABLE IF NOT EXISTS manual_decisions (seq INTEGER PRIMARY KEY AUTOINCREMENT, job_id TEXT, action TEXT, description TEXT, reviewed_at TEXT, original_error TEXT)')
        connection.execute('INSERT INTO manual_decisions(job_id,action,description,reviewed_at,original_error) VALUES(?,?,?,?,?)',
                           (event_key, action, description, store.now(), event['error']))
        if action == 'keep':
            _retain_frames(frames)
        _set_state(connection, event_key, state, report)
        if action == 'keep':
            from push_events import enqueue_event
            enqueue_event(connection, event_key)
    if action == 'dismiss':
        _delete_frames(frames)
    return {'accepted': True, 'state': state, 'job_id': event_key}
