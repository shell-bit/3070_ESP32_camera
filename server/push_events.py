"""Transactional outbox. One alert for the first flagged photo in each burst."""
import hashlib
import json
import time

def schema(c):
    c.execute('''CREATE TABLE IF NOT EXISTS push_events (
        id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL, group_key TEXT UNIQUE NOT NULL,
        created REAL NOT NULL, expanded INTEGER NOT NULL DEFAULT 0)''')
    c.execute('''CREATE TABLE IF NOT EXISTS push_deliveries (
        event_id TEXT NOT NULL, installation_id TEXT NOT NULL, binding TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,
        next_try REAL NOT NULL DEFAULT 0, last_error TEXT,
        PRIMARY KEY(event_id,installation_id,binding))''')

def enqueue(c, job_id):
    row=c.execute('SELECT id,owner_id,device,event,state FROM jobs WHERE id=?',(job_id,)).fetchone()
    if not row or row['owner_id'] is None or row['state']!='suspicious':return
    schema(c)
    group=hashlib.sha256(json.dumps([row['owner_id'],row['device'],row['event']]).encode()).hexdigest()
    c.execute('INSERT OR IGNORE INTO push_events(id,owner_id,group_key,created) VALUES(?,?,?,?)',
              (row['id'],row['owner_id'],group,time.time()))

def enqueue_event(c, event_key):
    """Use the same transactional outbox with one report per complete event."""
    row=c.execute('SELECT id,owner_id,device,event,state FROM events WHERE id=?',(event_key,)).fetchone()
    if not row or row['owner_id'] is None or row['state']!='suspicious':return
    schema(c)
    group=hashlib.sha256(json.dumps([row['owner_id'],row['device'],row['event']]).encode()).hexdigest()
    c.execute('INSERT OR IGNORE INTO push_events(id,owner_id,group_key,created) VALUES(?,?,?,?)',
              (row['id'],row['owner_id'],group,time.time()))
