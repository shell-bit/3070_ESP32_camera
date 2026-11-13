"""Camera credentials and immutable per-report ownership."""
import hashlib
import hmac
import os
import re
import secrets
from fastapi import HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field
from store import db, ROOT, now


def current_owner(request):
    user=getattr(request.state,'user',None)
    if not user:
        raise HTTPException(401,'Sign in to continue.')
    if user['role']!='viewer':
        raise HTTPException(403,'Use the administration page. Activity reports belong to user accounts.')
    return user['id']


def device_owner(request, device, connection):
    """Call inside the upload transaction too, to serialize credential rotation."""
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',device):
        raise HTTPException(400,'Invalid device ID')
    authorization=request.headers.get('authorization','')
    row=connection.execute('SELECT * FROM devices WHERE device=?',(device,)).fetchone()
    if row:
        raw=authorization[7:] if authorization.startswith('Bearer ') else ''
        if not raw or not hmac.compare_digest(hashlib.sha256(raw.encode()).hexdigest(),row['token_digest']):
            raise HTTPException(401,'Invalid camera upload token')
        owner=row['owner_id']
        if owner is not None:
            with request.app.state.accounts.db() as c:
                user=c.execute('SELECT active,role FROM users WHERE id=?',(owner,)).fetchone()
            if not user or not user['active'] or user['role']!='viewer':
                raise HTTPException(403,'Camera owner is unavailable. Contact the administrator.')
        return owner
    # Transitional compatibility: an unregistered camera can only upload into
    # unassigned storage. It can never choose a user's ID through headers.
    legacy=os.environ.get('UPLOAD_TOKEN','')
    if legacy and not hmac.compare_digest(authorization.encode(),('Bearer '+legacy).encode()):
        raise HTTPException(401,'Invalid upload token')
    return None


class Assignment(BaseModel):
    device_id: str = Field(pattern=r'^[A-Za-z0-9_-]{1,40}$')
    owner_id: int | None = Field(default=None,ge=1)
    include_unassigned: bool = False


def install_ownership(app):
    @app.get('/admin')
    def admin_page(): return FileResponse(ROOT/'admin.html')

    @app.get('/admin.js')
    def admin_script(): return FileResponse(ROOT/'admin.js',media_type='text/javascript')

    @app.get('/api/my/devices')
    def my_devices(request:Request):
        uid=current_owner(request)
        with db() as c:
            return {'devices':[r['device'] for r in c.execute('SELECT device FROM devices WHERE owner_id=? ORDER BY device',(uid,))]}

    @app.get('/api/admin/devices')
    def devices(request:Request):
        with db() as c:
            rows=c.execute('''SELECT d.device,d.owner_id,d.token_digest IS NOT NULL AS registered,
                COUNT(j.id) AS reports,COALESCE(SUM(j.owner_id IS NULL AND j.id IS NOT NULL),0) AS unassigned
                FROM (SELECT device,owner_id,token_digest FROM devices UNION ALL
                      SELECT DISTINCT device,NULL,NULL FROM jobs WHERE device NOT IN (SELECT device FROM devices)) d
                LEFT JOIN jobs j ON j.device=d.device GROUP BY d.device ORDER BY d.device''').fetchall()
        return {'devices':[dict(r) for r in rows]}

    @app.post('/api/admin/devices')
    def assign(body:Assignment, request:Request):
        if body.include_unassigned and body.owner_id is None:
            raise HTTPException(422,'Select a user before importing unassigned history.')
        if body.owner_id is not None:
            with app.state.accounts.db() as c:
                user=c.execute('SELECT active,role FROM users WHERE id=?',(body.owner_id,)).fetchone()
            if not user or not user['active'] or user['role']!='viewer':
                raise HTTPException(422,'Choose an active user account, not an administrator.')
        token=secrets.token_urlsafe(32)
        digest=hashlib.sha256(token.encode()).hexdigest()
        with db() as c:
            c.execute('BEGIN IMMEDIATE')
            old=c.execute('SELECT owner_id FROM devices WHERE device=?',(body.device_id,)).fetchone()
            c.execute('INSERT INTO devices VALUES(?,?,?) ON CONFLICT(device) DO UPDATE SET owner_id=excluded.owner_id,token_digest=excluded.token_digest',
                      (body.device_id,body.owner_id,digest))
            imported=0
            if body.include_unassigned:
                c.execute('UPDATE events SET owner_id=? WHERE device=? AND owner_id IS NULL',(body.owner_id,body.device_id))
                imported=c.execute('UPDATE jobs SET owner_id=? WHERE device=? AND owner_id IS NULL',(body.owner_id,body.device_id)).rowcount
            c.execute('INSERT INTO device_assignments(device,actor,old_owner,new_owner,imported,created) VALUES(?,?,?,?,?,?)',
                      (body.device_id,request.state.user['id'],old['owner_id'] if old else None,body.owner_id,imported,now()))
        return {'device_id':body.device_id,'owner_id':body.owner_id,'imported':imported,'upload_token':token,
                'message':'Save this camera token now. Update both uploads and alarm polling. Previous tokens no longer work.'}
