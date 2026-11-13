"""Bounded V2 JPEG + immutable identity ingestion. No model is loaded here."""
import asyncio
import hashlib
import json
import math
import os
import re
from datetime import datetime
from email.parser import BytesParser
from email.policy import default
import cv2
import numpy as np
from fastapi import HTTPException, Request
import store
from ownership import device_owner
from event_queue import ensure_event, register_frame, expire_events, EventConflict

MAX_JPEG = 4 * 1024 * 1024
MAX_METADATA = 16 * 1024


def schema(c):
    c.execute('''CREATE TABLE IF NOT EXISTS event_identity_protocol (
        event_key TEXT PRIMARY KEY, supports_exempted INTEGER NOT NULL)''')
    c.execute('''CREATE TABLE IF NOT EXISTS frame_identities (
        job_id TEXT PRIMARY KEY, metadata TEXT NOT NULL, digest TEXT NOT NULL,
        authorized_faces TEXT NOT NULL)''')
    c.execute('''CREATE TABLE IF NOT EXISTS alarm_decisions (
        seq INTEGER PRIMARY KEY AUTOINCREMENT, event_key TEXT NOT NULL,
        created TEXT NOT NULL, assessment TEXT NOT NULL, decision TEXT NOT NULL)''')


def _bad(message):
    raise HTTPException(422, message)


def _text(value, name, required=True):
    if not required and value is None:
        return
    if not isinstance(value, str) or not 1 <= len(value) <= 128 or any(ord(c) < 32 for c in value):
        _bad('Invalid ' + name)


def _finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def jpeg_dimensions(data):
    # Inspect SOF before allocating decoded pixels, including malicious huge JPEGs.
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        _bad('Incomplete JPEG')
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 255:
            _bad('Invalid JPEG marker')
        while pos < len(data) and data[pos] == 255:
            pos += 1
        if pos >= len(data): break
        marker = data[pos]; pos += 1
        if marker in (0xD9, 0xDA): break
        if marker == 0x01 or 0xD0 <= marker <= 0xD7: continue
        length = int.from_bytes(data[pos:pos+2], 'big')
        if length < 2 or pos + length > len(data): _bad('Invalid JPEG segment')
        if marker in (0xC0, 0xC1, 0xC2):
            if length < 8: _bad('Invalid JPEG dimensions')
            height = int.from_bytes(data[pos+3:pos+5], 'big')
            width = int.from_bytes(data[pos+5:pos+7], 'big')
            if not width or not height or width * height > 12000000:
                _bad('Invalid image or dimensions too large')
            return width, height
        pos += length
    _bad('JPEG requires a supported frame header')


def validate_metadata(raw, jpeg):
    if len(raw) > MAX_METADATA: _bad('Maximum 16 KiB identity metadata')
    try:
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result: raise ValueError('Duplicate JSON key')
                result[key] = value
            return result
        m = json.loads(raw, object_pairs_hook=pairs, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Non-finite JSON')))
    except (ValueError, UnicodeError, RecursionError): _bad('Invalid metadata JSON')
    if not isinstance(m, dict): _bad('Metadata must be an object')
    required = {'schema_version','device_id','event_id','photo_index','expected_frames','jpeg_sha256',
                'width','height','captured_at','night_capture','recognizer_version','database_version',
                'frame_identity_status','faces'}
    if not required <= m.keys() or m.keys() - required - {'supports_exempted'}:
        _bad('Missing or unsupported metadata fields')
    if type(m['schema_version']) is not int or m['schema_version'] != 1: _bad('Unsupported schema')
    for field, size in (('device_id',40), ('event_id',48)):
        if not isinstance(m[field], str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,'+str(size)+'}', m[field]):
            _bad('Invalid ' + field)
    if type(m['photo_index']) is not int or not 1 <= m['photo_index'] <= 5: _bad('Invalid photo index')
    if type(m['expected_frames']) is not int or m['expected_frames'] != 5: _bad('V2 expects five frames')
    width, height = jpeg_dimensions(jpeg)
    if type(m['width']) is not int or type(m['height']) is not int or (m['width'],m['height']) != (width,height):
        _bad('JPEG dimensions do not match metadata')
    if m['jpeg_sha256'] != hashlib.sha256(jpeg).hexdigest(): _bad('JPEG digest does not match metadata')
    if type(m['night_capture']) is not bool or type(m.get('supports_exempted', False)) is not bool:
        _bad('Expected boolean capture/capability flag')
    m.setdefault('supports_exempted', False)
    if m['captured_at'] is not None:
        try:
            if not isinstance(m['captured_at'], str) or len(m['captured_at']) > 64: raise ValueError()
            parsed = datetime.fromisoformat(m['captured_at'].replace('Z','+00:00'))
            if parsed.utcoffset() is None: raise ValueError()
            m['captured_at'] = parsed.isoformat()
        except ValueError: _bad('Invalid capture time')
    for field in ('recognizer_version','database_version'): _text(m[field], field, required=False)
    if m['frame_identity_status'] not in ('complete','partial','timeout','error','not_run'):
        _bad('Invalid identity coverage status')
    if not isinstance(m['faces'], list) or len(m['faces']) > 8: _bad('Maximum eight faces per frame')
    ids = set()
    for face in m['faces']:
        if not isinstance(face, dict) or set(face) != {'face_id','bbox','status','member_id','match_score','member_version'}:
            _bad('Invalid face fields')
        _text(face['face_id'], 'face ID')
        if face['face_id'] in ids: _bad('Duplicate face ID')
        ids.add(face['face_id'])
        box = face['bbox']
        if not isinstance(box,list) or len(box)!=4 or not all(_finite(v) for v in box): _bad('Invalid face bbox')
        x1,y1,x2,y2 = box
        if not (0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height): _bad('Face bbox outside JPEG')
        if not _finite(face['match_score']): _bad('Invalid match score')
        if face['status'] not in ('matched','unknown'): _bad('Invalid face status')
        if face['status'] == 'matched':
            _text(face['member_id'], 'member ID')
            if type(face['member_version']) is not int or face['member_version'] < 1: _bad('Invalid member version')
            if not m['recognizer_version'] or not m['database_version']: _bad('Matched identity requires versions')
            if m['frame_identity_status'] not in ('complete','partial'): _bad('Unfinished identity cannot claim matches')
        elif face['member_id'] is not None or face['member_version'] is not None:
            _bad('Unknown face cannot claim a member')
    image = cv2.imdecode(np.frombuffer(jpeg,dtype=np.uint8), cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
    if image is None or image.shape[:2] != (height,width): _bad('JPEG decoding failed')
    return m


async def read_multipart(request):
    content_type = request.headers.get('content-type','')
    if len(content_type) > 256 or '\r' in content_type or '\n' in content_type:
        raise HTTPException(415,'Invalid multipart content type')
    if content_type.split(';',1)[0].lower() != 'multipart/form-data':
        raise HTTPException(415,'Send multipart/form-data with photo and metadata')
    data = bytearray()
    async for chunk in request.stream():
        if len(data)+len(chunk) > MAX_JPEG + MAX_METADATA + 65536:
            raise HTTPException(413,'Upload body too large')
        data.extend(chunk)
    message = BytesParser(policy=default).parsebytes(
        b'Content-Type: '+content_type.encode('ascii',errors='replace')+b'\r\nMIME-Version: 1.0\r\n\r\n'+bytes(data))
    if not message.is_multipart() or message.defects or not message.get_boundary() or len(message.get_boundary()) > 70:
        _bad('Malformed multipart body')
    parts = {}
    for part in message.iter_parts():
        name = part.get_param('name',header='content-disposition')
        if part.is_multipart() or part.defects or part.get_content_disposition() != 'form-data' or name not in ('photo','metadata') or name in parts:
            _bad('Expected one photo and one metadata part')
        if part.get('Content-Transfer-Encoding'): _bad('Encoded MIME parts are unsupported')
        if name=='photo' and part.get_content_type() != 'image/jpeg': _bad('Photo must be image/jpeg')
        parts[name] = part.get_payload(decode=True)
    if set(parts) != {'photo','metadata'}: _bad('Missing photo or metadata')
    if len(parts['photo']) > MAX_JPEG: raise HTTPException(413,'Maximum 4 MiB per photo')
    return parts['photo'], parts['metadata']


def _owned_device(request, device, c):
    if not c.execute('SELECT 1 FROM devices WHERE device=? AND owner_id IS NOT NULL',(device,)).fetchone():
        raise HTTPException(403,'Register and assign the camera before using identity features')
    return device_owner(request,device,c)


def install_identity_api(app, upload_lock):
    @app.post('/api/device/upload-v2', status_code=201)
    async def upload_v2(request: Request):
        jpeg, raw = await read_multipart(request)
        # Authenticate before allocating decoded pixels or running expensive validation.
        if len(raw)>MAX_METADATA:_bad('Maximum 16 KiB identity metadata')
        try:
            claimed=json.loads(raw)
            device=claimed.get('device_id') if isinstance(claimed,dict) else None
            if not isinstance(device,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',device):raise ValueError()
        except (ValueError,UnicodeError,RecursionError):_bad('Invalid metadata device')
        with store.db() as c:_owned_device(request,device,c)
        metadata = await asyncio.to_thread(validate_metadata, raw, jpeg)
        device,event,index = (metadata[k] for k in ('device_id','event_id','photo_index'))
        key = hashlib.sha256(json.dumps([device,event,str(index)]).encode()).hexdigest()
        canonical = json.dumps(metadata,sort_keys=True,separators=(',',':'),ensure_ascii=True,allow_nan=False)
        digest = hashlib.sha256(canonical.encode()).hexdigest()
        expire_events()
        async with upload_lock:
            with store.db() as c:
                c.execute('BEGIN IMMEDIATE')
                owner = _owned_device(request,device,c)
                old = c.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone()
                if old:
                    identity = c.execute('SELECT digest FROM frame_identities WHERE job_id=?',(key,)).fetchone()
                    if old['owner_id']!=owner or old['digest']!=metadata['jpeg_sha256'] or identity is None or identity['digest']!=digest:
                        raise HTTPException(409,'Frame image, protocol or identity changed on retry')
                    row=c.execute('SELECT state FROM events WHERE id=?',(old['event_key'],)).fetchone()
                    return {'saved':True,'duplicate':True,'job_id':key,'event_key':old['event_key'],'analysis_status':row['state']}
                existing = c.execute('SELECT id FROM events WHERE device=? AND event=?',(device,event)).fetchone()
                if existing:
                    protocol=c.execute('SELECT * FROM event_identity_protocol WHERE event_key=?',(existing['id'],)).fetchone()
                    if protocol is None or bool(protocol['supports_exempted'])!=metadata['supports_exempted']:
                        raise HTTPException(409,'Event protocol/capability cannot change')
                if c.execute("SELECT count(*) FROM jobs WHERE state IN ('collecting','queued','processing','review')").fetchone()[0] >= int(os.environ.get('MAX_PENDING','200')):
                    raise HTTPException(503,'Analysis/review queue full')
                authorized = []
                if metadata['faces']:
                    from face_registry import is_registration_active
                    for face in metadata['faces']:
                        if face['status'] != 'matched': continue
                        member=c.execute('SELECT owner_id FROM family_members WHERE id=?',(face['member_id'],)).fetchone()
                        if member is None or member['owner_id'] != owner: _bad('Invalid member reference for this account')
                        if is_registration_active(c,owner,device,face['member_id'],face['member_version'],metadata['recognizer_version'],metadata['database_version']):
                            authorized.append(face['face_id'])
                try:
                    row=ensure_event(c,device,event,owner,expected_frames=5)
                except EventConflict as error: raise HTTPException(409,str(error))
                c.execute('INSERT OR IGNORE INTO event_identity_protocol VALUES(?,?)',(row['id'],int(metadata['supports_exempted'])))
                path=store.INCOMING/(key+'.jpg'); temp=path.with_suffix('.tmp')
                try:
                    with temp.open('wb') as output:
                        output.write(jpeg); output.flush(); os.fsync(output.fileno())
                    temp.replace(path)
                    c.execute('''INSERT INTO jobs(id,digest,device,event,photo_index,night,received,owner_id,event_key,captured_at,state)
                        VALUES(?,?,?,?,?,?,?,?,?,?,?)''',(key,metadata['jpeg_sha256'],device,event,index,int(metadata['night_capture']),store.now(),owner,row['id'],metadata['captured_at'],'collecting'))
                    c.execute('INSERT INTO frame_identities VALUES(?,?,?,?)',(key,canonical,digest,json.dumps(authorized)))
                    state=register_frame(c,row['id'])
                except Exception as error:
                    path.unlink(missing_ok=True); temp.unlink(missing_ok=True)
                    if isinstance(error,EventConflict): raise HTTPException(409,str(error))
                    raise
        return {'saved':True,'job_id':key,'event_key':row['id'],'analysis_status':state}
