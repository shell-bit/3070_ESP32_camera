"""Transactional family authorization. Camera ACKs describe durable writes, never intent.

Call mutations in the caller's BEGIN IMMEDIATE transaction. No function commits.
The credential digest binds registrations to the current camera assignment.
"""
import json
import time
import uuid


class RegistryError(ValueError):
    def __init__(self, message, status_code=409):
        super().__init__(message)
        self.status_code = status_code


def schema(connection):
    statements = [
        '''CREATE TABLE IF NOT EXISTS family_members (
            id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL, display_name TEXT NOT NULL,
            enabled INTEGER NOT NULL DEFAULT 1, deleted_at REAL,
            created_at REAL NOT NULL, updated_at REAL NOT NULL)''',
        '''CREATE TABLE IF NOT EXISTS member_devices (
            owner_id INTEGER NOT NULL, member_id TEXT NOT NULL, device_id TEXT NOT NULL,
            member_version INTEGER NOT NULL, state TEXT NOT NULL, command_id TEXT NOT NULL,
            credential_digest TEXT NOT NULL, ack_version INTEGER,
            recognizer_version TEXT, database_version TEXT, error TEXT, updated_at REAL NOT NULL,
            PRIMARY KEY(owner_id,member_id,device_id))''',
        '''CREATE TABLE IF NOT EXISTS device_commands (
            id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL, member_id TEXT NOT NULL,
            device_id TEXT NOT NULL, member_version INTEGER NOT NULL, operation TEXT NOT NULL,
            request_id TEXT NOT NULL, credential_digest TEXT NOT NULL,
            created_at REAL NOT NULL, expires_at REAL NOT NULL, status TEXT NOT NULL,
            ack_payload TEXT, error TEXT, UNIQUE(owner_id,request_id))''',
        '''CREATE TABLE IF NOT EXISTS family_device_state (
            owner_id INTEGER NOT NULL, device_id TEXT NOT NULL, credential_digest TEXT NOT NULL,
            recognizer_version TEXT, database_version TEXT, compatible INTEGER NOT NULL DEFAULT 0,
            last_seen_at REAL NOT NULL, PRIMARY KEY(owner_id,device_id))''',
        'CREATE INDEX IF NOT EXISTS family_members_owner ON family_members(owner_id,deleted_at)',
        'CREATE INDEX IF NOT EXISTS device_commands_poll ON device_commands(device_id,owner_id,status,expires_at)',
    ]
    for statement in statements:
        connection.execute(statement)


def _now(value):
    return time.time() if value is None else value


def _text(value, label, maximum=128):
    if not isinstance(value,str) or not value.strip() or len(value)>maximum:
        raise RegistryError('Invalid '+label,422)
    return value.strip()


def _device(c,owner_id,device_id):
    row=c.execute('SELECT * FROM devices WHERE device=? AND owner_id=?',(device_id,owner_id)).fetchone()
    if row is None or owner_id is None:
        raise RegistryError('Camera not found in your account.',404)
    return row


def _member(c,owner_id,member_id,include_deleted=False):
    row=c.execute('SELECT * FROM family_members WHERE id=? AND owner_id=?',(member_id,owner_id)).fetchone()
    if row is None or (row['deleted_at'] is not None and not include_deleted):
        raise RegistryError('Member not found.',404)
    return row


def create_member(c,owner_id,display_name,now=None):
    name=_text(display_name,'display name',80); now=_now(now); member_id=uuid.uuid4().hex
    c.execute('INSERT INTO family_members VALUES(?,?,?,1,NULL,?,?)',(member_id,owner_id,name,now,now))
    return dict(_member(c,owner_id,member_id))


def expire_commands(c,now=None):
    now=_now(now)
    c.execute("""UPDATE member_devices SET state='failed',error='Enrollment expired. Start again with the camera online.',updated_at=?
        WHERE state='pending' AND command_id IN
        (SELECT id FROM device_commands WHERE status IN ('pending','sampling') AND expires_at<=?)""",(now,now))
    c.execute("UPDATE device_commands SET status='expired' WHERE status IN ('pending','sampling') AND expires_at<=?",(now,))


def create_enrollment(c,owner_id,member_id,device_id,request_id,now=None):
    now=_now(now); request_id=_text(request_id,'request ID'); member=_member(c,owner_id,member_id)
    device=_device(c,owner_id,device_id)
    prior=c.execute('SELECT * FROM device_commands WHERE owner_id=? AND request_id=?',(owner_id,request_id)).fetchone()
    expire_commands(c,now)
    if prior:
        if (prior['member_id'],prior['device_id'],prior['operation'],prior['credential_digest']) != (member_id,device_id,'enroll',device['token_digest']):
            raise RegistryError('Request ID already belongs to a different operation.')
        return dict(c.execute('SELECT * FROM device_commands WHERE id=?',(prior['id'],)).fetchone())
    if not member['enabled']:
        raise RegistryError('Enable this member before starting a new enrollment.')
    busy=c.execute("SELECT id FROM device_commands WHERE device_id=? AND owner_id=? AND credential_digest=? AND operation='enroll' AND status IN ('pending','sampling')",
                   (device_id,owner_id,device['token_digest'])).fetchone()
    if busy: raise RegistryError('This camera already has an enrollment in progress.')
    previous=c.execute('SELECT member_version FROM member_devices WHERE owner_id=? AND member_id=? AND device_id=?',
                       (owner_id,member_id,device_id)).fetchone()
    version=previous['member_version']+1 if previous else 1
    command=_new_command(c,owner_id,member_id,device_id,version,'enroll',request_id,device['token_digest'],now)
    c.execute('''INSERT INTO member_devices(owner_id,member_id,device_id,member_version,state,command_id,credential_digest,updated_at)
        VALUES(?,?,?,?,'pending',?,?,?) ON CONFLICT(owner_id,member_id,device_id) DO UPDATE SET
        member_version=excluded.member_version,state='pending',command_id=excluded.command_id,
        credential_digest=excluded.credential_digest,ack_version=NULL,recognizer_version=NULL,database_version=NULL,error=NULL,updated_at=excluded.updated_at''',
        (owner_id,member_id,device_id,version,command['id'],device['token_digest'],now))
    return command


def _new_command(c,owner_id,member_id,device_id,version,operation,request_id,digest,now):
    command_id=uuid.uuid4().hex
    c.execute('''INSERT INTO device_commands(id,owner_id,member_id,device_id,member_version,operation,request_id,
        credential_digest,created_at,expires_at,status) VALUES(?,?,?,?,?,?,?,?,?,?,'pending')''',
        (command_id,owner_id,member_id,device_id,version,operation,request_id,digest,now,now+120))
    return dict(c.execute('SELECT * FROM device_commands WHERE id=?',(command_id,)).fetchone())


def _registration(c,owner_id,member_id,device_id):
    row=c.execute('SELECT * FROM member_devices WHERE owner_id=? AND member_id=? AND device_id=?',
                  (owner_id,member_id,device_id)).fetchone()
    return dict(row) if row else None


def revoke_member(c,owner_id,member_id,now=None,delete=False):
    now=_now(now); member=_member(c,owner_id,member_id,include_deleted=True)
    if member['deleted_at'] is not None:
        return dict(member)
    c.execute('UPDATE family_members SET enabled=0,deleted_at=?,updated_at=? WHERE id=? AND owner_id=?',
              (now if delete else None,now,member_id,owner_id))
    rows=c.execute("SELECT * FROM member_devices WHERE owner_id=? AND member_id=? AND state!='revoked'",(owner_id,member_id)).fetchall()
    for row in rows:
        c.execute("UPDATE device_commands SET status='cancelled' WHERE owner_id=? AND member_id=? AND device_id=? AND status IN ('pending','sampling')",
                  (owner_id,member_id,row['device_id']))
        version=row['member_version']+1
        device=c.execute('SELECT * FROM devices WHERE device=? AND owner_id=?',(row['device_id'],owner_id)).fetchone()
        command_id=row['command_id']
        if device and device['token_digest']==row['credential_digest']:
            command=_new_command(c,owner_id,member_id,row['device_id'],version,'revoke','revoke:'+uuid.uuid4().hex,device['token_digest'],now)
            command_id=command['id']
        c.execute("UPDATE member_devices SET member_version=?,state='revoked',command_id=?,error=NULL,updated_at=? WHERE owner_id=? AND member_id=? AND device_id=?",
                  (version,command_id,now,owner_id,member_id,row['device_id']))
    return dict(_member(c,owner_id,member_id,include_deleted=True))


def update_member(c,owner_id,member_id,display_name=None,enabled=None,now=None):
    now=_now(now); member=_member(c,owner_id,member_id)
    if enabled is False:
        revoke_member(c,owner_id,member_id,now)
    elif enabled is True:
        c.execute('UPDATE family_members SET enabled=1,updated_at=? WHERE id=? AND owner_id=?',(now,member_id,owner_id))
    if display_name is not None:
        name=_text(display_name,'display name',80)
        c.execute('UPDATE family_members SET display_name=?,updated_at=? WHERE id=? AND owner_id=?',(name,now,member_id,owner_id))
    return dict(_member(c,owner_id,member_id))


def commands_for_device(c,owner_id,device_id,now=None):
    now=_now(now); device=_device(c,owner_id,device_id); expire_commands(c,now)
    # Revocation is durable authorization intent; only its delivery lease expires.
    # A failed terminal ACK needs a fresh command ID so device deduplication can
    # distinguish the retry from replaying that failed attempt forever.
    retryable=c.execute('''SELECT cmd.* FROM device_commands cmd
        JOIN member_devices r ON r.owner_id=cmd.owner_id AND r.member_id=cmd.member_id
            AND r.device_id=cmd.device_id AND r.command_id=cmd.id
            AND r.member_version=cmd.member_version AND r.credential_digest=cmd.credential_digest
        JOIN devices d ON d.device=cmd.device_id AND d.owner_id=cmd.owner_id
            AND d.token_digest=cmd.credential_digest
        WHERE cmd.owner_id=? AND cmd.device_id=? AND r.state='revoked'
            AND cmd.operation='revoke' AND cmd.status IN ('expired','failed') AND cmd.expires_at<=?''',
        (owner_id,device_id,now)).fetchall()
    for command in retryable:
        if command['status']=='expired':
            c.execute("UPDATE device_commands SET status='pending',expires_at=? WHERE id=?",(now+120,command['id']))
        else:
            retry=_new_command(c,owner_id,command['member_id'],device_id,command['member_version'],
                               'revoke','revoke:'+uuid.uuid4().hex,device['token_digest'],now)
            c.execute('''UPDATE member_devices SET command_id=?,updated_at=?
                WHERE owner_id=? AND member_id=? AND device_id=? AND command_id=?''',
                (retry['id'],now,owner_id,command['member_id'],device_id,command['id']))
    c.execute('''INSERT INTO family_device_state(owner_id,device_id,credential_digest,last_seen_at) VALUES(?,?,?,?)
        ON CONFLICT(owner_id,device_id) DO UPDATE SET last_seen_at=excluded.last_seen_at''',
        (owner_id,device_id,device['token_digest'],now))
    return [dict(r) for r in c.execute("""SELECT cmd.* FROM device_commands cmd
        JOIN member_devices r ON r.owner_id=cmd.owner_id AND r.member_id=cmd.member_id
            AND r.device_id=cmd.device_id AND r.command_id=cmd.id
            AND r.member_version=cmd.member_version AND r.credential_digest=cmd.credential_digest
        WHERE cmd.owner_id=? AND cmd.device_id=? AND cmd.credential_digest=?
            AND cmd.status IN ('pending','sampling') AND cmd.expires_at>?
            AND ((cmd.operation='enroll' AND r.state='pending') OR (cmd.operation='revoke' AND r.state='revoked'))
        ORDER BY cmd.created_at,cmd.id""",
        (owner_id,device_id,device['token_digest'],now))]


def _invalidate_device(c,owner_id,device_id,now,reason,except_command=None):
    c.execute("""UPDATE member_devices SET state='revoked',member_version=member_version+1,error=?,updated_at=?
        WHERE owner_id=? AND device_id=? AND state!='revoked' AND command_id!=?""",
        (reason,now,owner_id,device_id,except_command or ''))
    c.execute("""UPDATE device_commands SET status='cancelled',error=? WHERE owner_id=? AND device_id=?
        AND status IN ('pending','sampling') AND id!=?""",(reason,owner_id,device_id,except_command or ''))


def report_device_state(c,owner_id,device_id,recognizer_version,database_version,compatible=True,reset=False,now=None):
    now=_now(now); device=_device(c,owner_id,device_id)
    model=_text(recognizer_version,'recognizer version'); database=_text(database_version,'database version')
    previous=c.execute('SELECT * FROM family_device_state WHERE owner_id=? AND device_id=?',(owner_id,device_id)).fetchone()
    changed=previous and previous['database_version'] is not None and (previous['recognizer_version']!=model or previous['database_version']!=database or previous['credential_digest']!=device['token_digest'])
    if reset or not compatible or changed:
        _invalidate_device(c,owner_id,device_id,now,'Camera library reset or changed; enroll again.')
    _device_state(c,owner_id,device_id,device['token_digest'],model,database,compatible and not reset,now)
    return {'device_id':device_id,'requires_enrollment':bool(reset or not compatible or changed)}


def _device_state(c,owner_id,device_id,digest,model,database,compatible,now):
    c.execute('''INSERT INTO family_device_state VALUES(?,?,?,?,?,?,?) ON CONFLICT(owner_id,device_id) DO UPDATE SET
        credential_digest=excluded.credential_digest,recognizer_version=excluded.recognizer_version,
        database_version=excluded.database_version,compatible=excluded.compatible,last_seen_at=excluded.last_seen_at''',
        (owner_id,device_id,digest,model,database,int(compatible),now))


def ack_command(c,owner_id,device_id,command_id,version,result,database_version=None,recognizer_version=None,error=None,now=None):
    now=_now(now); device=_device(c,owner_id,device_id)
    command=c.execute('SELECT * FROM device_commands WHERE id=? AND owner_id=? AND device_id=?',
                      (command_id,owner_id,device_id)).fetchone()
    if not command or command['credential_digest']!=device['token_digest']:
        raise RegistryError('Command not found.',404)
    registration=_registration(c,owner_id,command['member_id'],device_id)
    if isinstance(version,bool) or not isinstance(version,int) or version!=command['member_version'] or registration['command_id']!=command_id or registration['member_version']!=version:
        raise RegistryError('This command version is no longer current.')
    if result not in ('sampling','committed','failed'):
        raise RegistryError('Invalid ACK result.',422)
    if result=='sampling' and command['operation']!='enroll':
        raise RegistryError('Only enrollment commands collect samples.',422)
    if result=='committed':
        database_version=_text(database_version,'database version'); recognizer_version=_text(recognizer_version,'recognizer version')
    if error is not None: error=_text(error,'error',500)
    payload=json.dumps([result,database_version,recognizer_version,error],separators=(',',':'))
    if command['ack_payload'] is not None:
        if command['ack_payload']!=payload: raise RegistryError('ACK payload changed on retry.')
        return registration
    expire_commands(c,now)
    if command['expires_at']<=now:
        return _registration(c,owner_id,command['member_id'],device_id)
    if command['status'] not in ('pending','sampling'):
        raise RegistryError('Command has been cancelled.')
    member=_member(c,owner_id,command['member_id'],include_deleted=True)
    if command['operation']=='enroll' and (not member['enabled'] or member['deleted_at'] is not None):
        raise RegistryError('Member authorization was revoked.')
    if result=='sampling':
        c.execute("UPDATE device_commands SET status='sampling' WHERE id=?",(command_id,))
        return registration
    state='active' if result=='committed' and command['operation']=='enroll' else 'revoked' if command['operation']=='revoke' else 'failed'
    if result=='committed':
        previous=c.execute('SELECT recognizer_version FROM family_device_state WHERE owner_id=? AND device_id=?',(owner_id,device_id)).fetchone()
        if previous and previous['recognizer_version'] not in (None,recognizer_version):
            _invalidate_device(c,owner_id,device_id,now,'Recognizer changed; enroll again.',command_id)
        # A durable library transaction preserves other mappings of the same model.
        c.execute("UPDATE member_devices SET database_version=? WHERE owner_id=? AND device_id=? AND credential_digest=? AND state='active' AND recognizer_version=?",
                  (database_version,owner_id,device_id,device['token_digest'],recognizer_version))
        _device_state(c,owner_id,device_id,device['token_digest'],recognizer_version,database_version,True,now)
    c.execute('UPDATE member_devices SET state=?,ack_version=?,recognizer_version=?,database_version=?,error=?,updated_at=? WHERE owner_id=? AND member_id=? AND device_id=?',
              (state,version if result=='committed' else None,recognizer_version,database_version,error,now,owner_id,command['member_id'],device_id))
    c.execute('UPDATE device_commands SET status=?,ack_payload=?,error=? WHERE id=?',(result,payload,error,command_id))
    return _registration(c,owner_id,command['member_id'],device_id)


def is_registration_active(connection,owner_id,device_id,member_id,member_version,recognizer_version,database_version):
    if owner_id is None or isinstance(member_version,bool) or not isinstance(member_version,int) or member_version<1 or not recognizer_version or not database_version:
        return False
    row=connection.execute('''SELECT 1 FROM member_devices r
        JOIN family_members m ON m.id=r.member_id AND m.owner_id=r.owner_id
        JOIN devices d ON d.device=r.device_id AND d.owner_id=r.owner_id AND d.token_digest=r.credential_digest
        JOIN family_device_state s ON s.device_id=r.device_id AND s.owner_id=r.owner_id AND s.credential_digest=r.credential_digest
        WHERE r.owner_id=? AND r.device_id=? AND r.member_id=? AND r.member_version=? AND r.ack_version=r.member_version
        AND r.state='active' AND m.enabled=1 AND m.deleted_at IS NULL
        AND r.recognizer_version=? AND r.database_version=? AND s.compatible=1
        AND s.recognizer_version=r.recognizer_version AND s.database_version=r.database_version''',
        (owner_id,device_id,member_id,member_version,recognizer_version,database_version)).fetchone()
    return row is not None


def list_members(c,owner_id,now=None):
    now=_now(now); expire_commands(c,now)
    members=[]
    for row in c.execute('SELECT * FROM family_members WHERE owner_id=? AND deleted_at IS NULL ORDER BY created_at,id',(owner_id,)):
        member=dict(row); member['devices']=[]
        for registration in c.execute('''SELECT r.*,cmd.status AS command_status,cmd.expires_at,s.last_seen_at,
            d.token_digest AS current_digest FROM member_devices r
            JOIN devices d ON d.device=r.device_id AND d.owner_id=r.owner_id
            LEFT JOIN device_commands cmd ON cmd.id=r.command_id
            LEFT JOIN family_device_state s ON s.owner_id=r.owner_id AND s.device_id=r.device_id
            WHERE r.owner_id=? AND r.member_id=? ORDER BY r.device_id''',(owner_id,row['id'])):
            item=dict(registration)
            if item['credential_digest']!=item.pop('current_digest'):
                item['state']='revoked'; item['error']='Camera credentials changed; enroll again.'
            item.pop('credential_digest',None)
            item['online']=item['last_seen_at'] is not None and now-item['last_seen_at']<15
            member['devices'].append(item)
        members.append(member)
    return members
