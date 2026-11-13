import os
import json
import sqlite3
from pathlib import Path
from contextlib import contextmanager
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent
DATA = Path(os.environ.get('CAMERA_DATA_DIR', str(ROOT / 'ai-data'))).resolve()
INCOMING = DATA / 'incoming'
PHOTOS = DATA / 'photos'
for directory in (INCOMING, PHOTOS):
    directory.mkdir(parents=True, exist_ok=True)

def now():
    return datetime.now(timezone.utc).isoformat()

@contextmanager
def db():
    connection = sqlite3.connect(DATA / 'queue.sqlite3', timeout=30)
    connection.row_factory = sqlite3.Row
    try:
        with connection:
            connection.execute('''CREATE TABLE IF NOT EXISTS jobs (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
                digest TEXT NOT NULL, device TEXT NOT NULL, event TEXT NOT NULL,
                photo_index INTEGER NOT NULL, night INTEGER NOT NULL,
                received TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'queued',
                report TEXT, error TEXT, owner_id INTEGER)''')
            # Additive migration: historical records remain unassigned until an admin
            # explicitly imports that camera's unassigned history for a user.
            for name, datatype in (('owner_id', 'INTEGER'), ('event_key', 'TEXT'), ('captured_at', 'TEXT')):
                if name not in {r['name'] for r in connection.execute('PRAGMA table_info(jobs)')}:
                    try:
                        connection.execute('ALTER TABLE jobs ADD COLUMN ' + name + ' ' + datatype)
                    except sqlite3.OperationalError:
                        if name not in {r['name'] for r in connection.execute('PRAGMA table_info(jobs)')}:
                            raise
            connection.execute('CREATE INDEX IF NOT EXISTS jobs_owner_state ON jobs(owner_id,state,seq)')
            connection.execute('CREATE UNIQUE INDEX IF NOT EXISTS jobs_event_frame ON jobs(event_key,photo_index) WHERE event_key IS NOT NULL')
            connection.execute('CREATE TABLE IF NOT EXISTS devices (device TEXT PRIMARY KEY, owner_id INTEGER, token_digest TEXT NOT NULL)')
            connection.execute('CREATE TABLE IF NOT EXISTS device_assignments (seq INTEGER PRIMARY KEY, device TEXT, actor INTEGER, old_owner INTEGER, new_owner INTEGER, imported INTEGER, created TEXT)')
            from push_events import schema as push_schema
            push_schema(connection)
            from event_queue import schema as event_schema
            event_schema(connection)
            from identity_ingest import schema as identity_schema
            identity_schema(connection)
            from face_registry import schema as family_schema
            family_schema(connection)
            yield connection
    finally:
        connection.close()

SUSPICIOUS_TYPES = frozenset(('forced_entry', 'suspected_theft', 'violence', 'suspicious'))

def normalize_activity_type(verdict, activity_type=None):
    if verdict not in ('suspicious', 'normal', 'uncertain'):
        raise ValueError('Invalid assessment verdict')
    if activity_type is not None and not isinstance(activity_type, str):
        raise ValueError('Activity type must be a string or null')
    if verdict != 'suspicious':
        if activity_type not in (None, ''):
            raise ValueError('Activity subtype contradicts non-suspicious verdict')
        return None
    # Unknown/missing subtypes must never suppress a suspicious result.
    return activity_type if activity_type in SUSPICIOUS_TYPES else 'suspicious'

def report_for(row):
    report = json.loads(row['report']) if row['report'] else None
    if report is not None:
        if report.get('status') == 'suspicious':
            report['activity_type'] = normalize_activity_type('suspicious', report.get('activity_type'))
        else:
            report.pop('activity_type', None)
    return report

def finish(job, verdict, description, activity_type=None):
    with db() as connection:
        row = connection.execute('SELECT event_key FROM jobs WHERE id=?', (job['id'],)).fetchone()
        if row is not None and row['event_key'] is not None:
            raise ValueError('Event-linked frames require a whole-event verdict.')
    activity_type = normalize_activity_type(verdict, activity_type)
    if verdict == 'uncertain':
        raise ValueError('Uncertain assessments require manual review')
    source = INCOMING / (job['id'] + '.jpg')
    target = PHOTOS / (job['id'] + '.jpg')
    report = {'report_id': job['id'], 'id': job['id'], 'status': verdict,
              'analysis_status': 'completed', 'description': description,
              'timestamp': job['received'], 'detected_at': job['received'],
              'analyzed_at': now(), 'device_id': job['device'],
              'photo_name': target.name, 'photo_url': '/photos/' + target.name}
    if verdict == 'suspicious':
        report['activity_type'] = activity_type
        if source.exists():
            source.replace(target)
        if not target.is_file():
            raise FileNotFoundError('Suspicious photo is missing')
    state = 'suspicious' if verdict == 'suspicious' else 'discarded'
    with db() as connection:
        connection.execute('UPDATE jobs SET state=?,report=?,error=NULL WHERE id=?',
                           (state, json.dumps(report), job['id']))
        if state=='suspicious':
            from push_events import enqueue
            enqueue(connection,job['id'])
    if state == 'discarded':
        source.unlink(missing_ok=True)

def recover():
    # One worker only. Resume interrupted jobs; replaying inference is safe.
    with db() as connection:
        connection.execute('BEGIN IMMEDIATE')
        connection.execute("UPDATE jobs SET state='queued' WHERE state='processing' AND event_key IS NULL")
        from event_queue import recover_events
        recover_events(connection)
        discarded = connection.execute("SELECT id FROM jobs WHERE state='discarded'").fetchall()
    for row in discarded:
        (INCOMING / (row['id'] + '.jpg')).unlink(missing_ok=True)
        (PHOTOS / (row['id'] + '.jpg')).unlink(missing_ok=True)
