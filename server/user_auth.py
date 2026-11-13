"""Individual project accounts. Device authentication remains in the device routes."""
import hashlib
import hmac
import os
import re
import secrets
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path

from fastapi import HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from pydantic import BaseModel, Field
from typing import Literal

COOKIE = 'sentinel_session'
ITERATIONS = 600_000
TTL = 12 * 3600
IDLE = 30 * 60
USERNAME = re.compile(r'[a-z0-9][a-z0-9_.-]{2,39}')
DUMMY = None


def password_hash(password):
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), salt.encode(), ITERATIONS).hex()
    return f'pbkdf2_sha256${ITERATIONS}${salt}${digest}'


def password_matches(password, stored):
    kind, rounds, salt, expected = stored.split('$')
    if kind != 'pbkdf2_sha256':
        return False
    actual = hashlib.pbkdf2_hmac('sha256', password.encode(), salt.encode(), int(rounds)).hex()
    return hmac.compare_digest(actual, expected)


def normalized_name(value):
    name = value.strip().lower()
    if not USERNAME.fullmatch(name):
        raise HTTPException(422, 'Username: 3–40 letters, numbers, dots, underscores or hyphens.')
    return name


def check_password(password):
    if not 12 <= len(password) <= 128:
        raise HTTPException(422, 'Use a password between 12 and 128 characters.')


class Accounts:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True, exist_ok=True)
        self.path = self.folder / 'accounts.sqlite3'
        with self.db() as c:
            c.executescript('''
                CREATE TABLE IF NOT EXISTS users (
                  id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
                  password_hash TEXT NOT NULL, role TEXT NOT NULL,
                  active INTEGER NOT NULL DEFAULT 1, created INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS sessions (
                  digest TEXT PRIMARY KEY, user_id INTEGER NOT NULL,
                  expires INTEGER NOT NULL, seen INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS limits (
                  bucket TEXT PRIMARY KEY, count INTEGER NOT NULL, until INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS auth_audit (
                  id INTEGER PRIMARY KEY, actor INTEGER, action TEXT NOT NULL,
                  target INTEGER, created INTEGER NOT NULL);
            ''')
        if os.name != 'nt':
            self.folder.chmod(0o700)
            self.path.chmod(0o600)
        global DUMMY
        if DUMMY is None:
            DUMMY = password_hash(secrets.token_urlsafe(24))

    @contextmanager
    def db(self):
        c = sqlite3.connect(self.path, timeout=15)
        c.row_factory = sqlite3.Row
        try:
            with c:
                yield c
        finally:
            c.close()

    def create(self, username, password, role='viewer', actor=None):
        name = normalized_name(username)
        check_password(password)
        if role not in ('admin', 'viewer'):
            raise HTTPException(422, 'Invalid role')
        encoded = password_hash(password)
        try:
            with self.db() as c:
                cursor = c.execute('INSERT INTO users(username,password_hash,role,created) VALUES(?,?,?,?)',
                                   (name, encoded, role, int(time.time())))
                uid = cursor.lastrowid
                self.audit(c, actor, 'create_user', uid)
            return uid
        except sqlite3.IntegrityError:
            raise HTTPException(409, 'Username already exists')

    def audit(self, c, actor, action, target):
        c.execute('INSERT INTO auth_audit(actor,action,target,created) VALUES(?,?,?,?)',
                  (actor, action, target, int(time.time())))

    def limit(self, username, ip):
        now = int(time.time())
        with self.db() as c:
            c.execute('BEGIN IMMEDIATE')
            c.execute('DELETE FROM limits WHERE until<=?', (now,))
            buckets = [('u:' + hashlib.sha256(username.encode()).hexdigest(), 10),
                       ('ip:' + hashlib.sha256(ip.encode()).hexdigest(), 100)]
            for bucket, maximum in buckets:
                row = c.execute('SELECT count FROM limits WHERE bucket=?', (bucket,)).fetchone()
                if row and row['count'] >= maximum:
                    raise HTTPException(429, 'Too many sign-in attempts. Try again in 15 minutes.', headers={'Retry-After': '900'})
            for bucket, _ in buckets:
                c.execute('INSERT INTO limits VALUES(?,1,?) ON CONFLICT(bucket) DO UPDATE SET count=count+1',
                          (bucket, now + 900))

    def login(self, username, password, ip):
        name = username.strip().lower()
        self.limit(name, ip)
        with self.db() as c:
            row = c.execute('SELECT * FROM users WHERE username=?', (name,)).fetchone()
        valid = password_matches(password, row['password_hash'] if row else DUMMY)
        if not valid or not row or not row['active']:
            raise HTTPException(401, 'Incorrect username or password.')
        raw = secrets.token_urlsafe(32)
        now = int(time.time())
        with self.db() as c:
            # Recheck after hashing, so a concurrently disabled/reset account cannot sign in.
            c.execute('BEGIN IMMEDIATE')
            current = c.execute('SELECT * FROM users WHERE id=?', (row['id'],)).fetchone()
            if not current['active'] or current['password_hash'] != row['password_hash']:
                raise HTTPException(401, 'Incorrect username or password.')
            c.execute('DELETE FROM sessions WHERE expires<=? OR seen<=?', (now, now-IDLE))
            c.execute('INSERT INTO sessions VALUES(?,?,?,?)', (self.digest(raw), row['id'], now+TTL, now))
            self.audit(c, row['id'], 'login', row['id'])
        return raw

    @staticmethod
    def digest(raw):
        return hashlib.sha256(raw.encode()).hexdigest()

    @staticmethod
    def csrf(raw):
        return hmac.new(raw.encode(), b'sentinel-csrf-v1', hashlib.sha256).hexdigest()

    def session(self, raw):
        if not raw or len(raw) > 128:
            return None
        now = int(time.time())
        digest = self.digest(raw)
        with self.db() as c:
            row = c.execute('''SELECT users.id,username,role,seen FROM sessions
                JOIN users ON users.id=sessions.user_id WHERE digest=? AND active=1
                AND expires>? AND seen>?''', (digest, now, now-IDLE)).fetchone()
            if row and now-row['seen'] >= 60:
                c.execute('UPDATE sessions SET seen=? WHERE digest=?', (now, digest))
        return dict(row) if row else None

    def change_user(self, uid, role, active, password, actor):
        encoded = None
        if password is not None:
            check_password(password)
            encoded = password_hash(password)
        with self.db() as c:
            c.execute('BEGIN IMMEDIATE')
            row = c.execute('SELECT * FROM users WHERE id=?', (uid,)).fetchone()
            if not row:
                raise HTTPException(404, 'User not found')
            new_role = role if role is not None else row['role']
            new_active = int(active) if active is not None else row['active']
            if row['role']=='admin' and row['active'] and (new_role!='admin' or not new_active):
                count = c.execute("SELECT count(*) FROM users WHERE role='admin' AND active=1").fetchone()[0]
                if count <= 1:
                    raise HTTPException(409, 'Keep at least one active administrator.')
            c.execute('UPDATE users SET role=?,active=?,password_hash=? WHERE id=?',
                      (new_role, new_active, encoded or row['password_hash'], uid))
            c.execute('DELETE FROM sessions WHERE user_id=?', (uid,))
            from push_api import revoke
            revoke(c,uid=uid)
            self.audit(c, actor, 'update_user', uid)


class Login(BaseModel):
    username: str = Field(min_length=1, max_length=40)
    password: str = Field(min_length=1, max_length=128)


class NewUser(Login):
    role: Literal['admin', 'viewer'] = 'viewer'


class UserChange(BaseModel):
    role: Literal['admin', 'viewer'] | None = None
    active: bool | None = None
    password: str | None = Field(default=None, max_length=128)


class PasswordChange(BaseModel):
    current_password: str = Field(min_length=1, max_length=128)
    new_password: str = Field(min_length=12, max_length=128)


def install_user_auth(app, root, data):
    root = Path(root)
    accounts = Accounts(Path(data)/'auth')
    app.state.accounts = accounts
    secure = os.environ.get('AUTH_ALLOW_HTTP', '') != '1'
    public_assets = {'/login': 'login.html', '/auth.js': 'auth.js', '/auth.css': 'auth.css'}
    public = set(public_assets) | {'/api/auth/login', '/api/health'}
    device_routes = {('POST','/api/device/upload'), ('GET','/api/device/event-status'), ('POST','/api/device/audio'), ('GET','/api/device/talk'), ('POST','/api/device/talk/ack')}
    device_routes |= {('POST','/api/device/upload-v2'), ('GET','/api/device/commands'), ('POST','/api/device/identity-state')}

    def origin_check(request):
        if request.headers.get('sec-fetch-site') == 'cross-site':
            return False
        origin = request.headers.get('origin')
        # Proxy HTTPS is trusted by Uvicorn only from configured trusted proxy IPs.
        return origin is None or origin == str(request.base_url).rstrip('/')

    @app.middleware('http')
    async def authenticate(request: Request, call_next):
        path, method = request.url.path, request.method
        raw = request.cookies.get(COOKIE, '')
        user = accounts.session(raw)
        request.state.user = user
        is_device = (method, path) in device_routes or (method=='POST' and path in ('/api/push/refresh','/api/push/unregister')) or (method=='GET' and re.fullmatch(r'/api/push/event/[a-f0-9]{64}',path) is not None)
        is_device = is_device or (method=='POST' and re.fullmatch(r'/api/device/commands/[A-Za-z0-9_-]{1,128}/ack',path) is not None)
        if not is_device:
            if path not in public and user is None:
                if path in ('/', '/account', '/admin', '/members') and method=='GET':
                    return RedirectResponse('/login', status_code=303, headers={'Cache-Control':'no-store'})
                return JSONResponse({'detail':'Sign in to continue.'}, 401, headers={'Cache-Control':'no-store'})
            if user and (path.startswith('/api/admin/') or path in ('/admin','/admin.js')) and user['role']!='admin':
                return JSONResponse({'detail':'Administrator access required.'}, 403)
            if method not in ('GET','HEAD','OPTIONS'):
                if not origin_check(request):
                    return JSONResponse({'detail':'Request origin rejected.'},403)
                if path=='/api/auth/login':
                    # Custom header + JSON prevents form/login CSRF. No cross-origin CORS allowed.
                    if request.headers.get('x-sentinel-request')!='1':
                        return JSONResponse({'detail':'Use the sign-in page.'},403)
                else:
                    if not user or not hmac.compare_digest(request.headers.get('x-csrf-token','').encode(),accounts.csrf(raw).encode()):
                        return JSONResponse({'detail':'Session verification failed. Reload the page.'},403)
                    family_write = method in ('POST','PATCH','DELETE') and re.fullmatch(r'/api/family/members(?:/[A-Za-z0-9_-]{1,128}(?:/enrollments)?)?',path) is not None
                    private_delete = method=='DELETE' and re.fullmatch(r'/api/family/events/[a-f0-9]{64}',path) is not None
                    if user['role']!='admin' and not family_write and not private_delete and not (method=='POST' and (path=='/api/audio/talk/start' or re.fullmatch(r'/api/audio/talk/[a-f0-9]{32}(?:/stop)?',path))) and path not in ('/api/auth/logout','/api/auth/password','/api/push/register') and not (method=='POST' and re.fullmatch(r'/api/review/[a-f0-9]{64}',path)):
                        return JSONResponse({'detail':'Administrator access required.'},403)
        if path=='/api/health' and user is None:
            return JSONResponse({'server':'running','login_required':True},headers={'Cache-Control':'no-store'})
        response = await call_next(request)
        if not is_device:
            response.headers['Cache-Control']='no-store'
            response.headers['X-Content-Type-Options']='nosniff'
            response.headers['X-Frame-Options']='DENY'
            response.headers['Referrer-Policy']='same-origin'
        return response

    @app.get('/login')
    def login_page():
        return FileResponse(root/'login.html')

    @app.get('/auth.js')
    def auth_js():
        return FileResponse(root/'auth.js', media_type='text/javascript')

    @app.get('/auth.css')
    def auth_css():
        return FileResponse(root/'auth.css', media_type='text/css')

    @app.get('/account')
    def account_page():
        return FileResponse(root/'account.html')

    @app.post('/api/auth/login')
    def login(body: Login, request: Request):
        raw = accounts.login(body.username, body.password, request.client.host if request.client else 'unknown')
        response = JSONResponse({'ok':True})
        response.set_cookie(COOKIE, raw, max_age=TTL, httponly=True, secure=secure, samesite='strict', path='/')
        return response

    @app.get('/api/auth/me')
    def me(request: Request):
        u=request.state.user
        return {'id':u['id'],'username':u['username'],'role':u['role'],'csrf':accounts.csrf(request.cookies[COOKIE])}

    @app.post('/api/auth/logout')
    def logout(request: Request):
        with accounts.db() as c:
            c.execute('DELETE FROM sessions WHERE digest=?',(accounts.digest(request.cookies[COOKIE]),))
            from push_api import revoke
            revoke(c,session=accounts.digest(request.cookies[COOKIE]))
        response=JSONResponse({'ok':True})
        response.delete_cookie(COOKIE, path='/', secure=secure, httponly=True, samesite='strict')
        return response

    @app.post('/api/auth/password')
    def change_password(body: PasswordChange, request: Request):
        uid=request.state.user['id']
        accounts.limit('password-change:'+str(uid), request.client.host if request.client else 'unknown')
        with accounts.db() as c:
            row=c.execute('SELECT password_hash FROM users WHERE id=?',(uid,)).fetchone()
        if not password_matches(body.current_password,row['password_hash']):
            raise HTTPException(401,'Current password is incorrect.')
        accounts.change_user(uid,None,None,body.new_password,uid)
        if request.state.user['username']=='admin':
            (accounts.folder/'initial-admin.txt').unlink(missing_ok=True)
        response=JSONResponse({'ok':True,'message':'Password changed. Sign in again on each device.'})
        response.delete_cookie(COOKIE,path='/',secure=secure,httponly=True,samesite='strict')
        return response

    @app.get('/api/admin/users')
    def users():
        with accounts.db() as c:
            return {'users':[dict(r) for r in c.execute('SELECT id,username,role,active,created FROM users ORDER BY id')]}

    @app.post('/api/admin/users',status_code=201)
    def add_user(body: NewUser, request: Request):
        uid=accounts.create(body.username,body.password,body.role,request.state.user['id'])
        return {'id':uid}

    @app.patch('/api/admin/users/{uid}')
    def update_user(uid:int, body: UserChange, request: Request):
        accounts.change_user(uid,body.role,body.active,body.password,request.state.user['id'])
        return {'ok':True}

    return accounts
