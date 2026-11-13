"""Account-private member management, using existing session and CSRF credentials."""
import hmac
import os
from contextlib import contextmanager
from fastapi import HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, ConfigDict, Field
from ownership import current_owner
from store import ROOT, db
from user_auth import COOKIE
import face_registry as registry


class NewMember(BaseModel):
    model_config=ConfigDict(extra='forbid')
    display_name: str=Field(min_length=1,max_length=80)


class MemberChange(BaseModel):
    model_config=ConfigDict(extra='forbid')
    display_name: str | None=Field(default=None,min_length=1,max_length=80)
    enabled: bool | None=None


class Enrollment(BaseModel):
    model_config=ConfigDict(extra='forbid')
    device_id: str=Field(pattern=r'^[A-Za-z0-9_-]{1,40}$')
    request_id: str=Field(min_length=1,max_length=128)


@contextmanager
def registry_transaction():
    try:
        with db() as c:
            registry.schema(c)
            c.execute('BEGIN IMMEDIATE')
            yield c
    except registry.RegistryError as error:
        raise HTTPException(error.status_code,str(error)) from error


def mutation_owner(request):
    uid=current_owner(request)
    origin=request.headers.get('origin')
    if request.headers.get('sec-fetch-site')=='cross-site' or (origin is not None and origin!=str(request.base_url).rstrip('/')):
        raise HTTPException(403,'Request origin rejected.')
    cookie=request.cookies.get(COOKIE,'')
    expected=request.app.state.accounts.csrf(cookie)
    if not cookie or not hmac.compare_digest(request.headers.get('x-csrf-token','').encode(),expected.encode()):
        raise HTTPException(403,'Session verification failed. Reload the page.')
    return uid


def public_command(command):
    return {key:value for key,value in command.items() if key not in ('credential_digest','ack_payload','owner_id')}


def public_registration(registration):
    return {key:value for key,value in registration.items() if key not in ('credential_digest','owner_id')}


def mode_for_devices(devices):
    allowlist={value.strip() for value in os.environ.get('FAMILY_EXEMPTION_DEVICES','').split(',') if value.strip()}
    models={value.strip() for value in os.environ.get('FACE_RECOGNIZER_VERSIONS','').split(',') if value.strip()}
    try: retention=int(os.environ.get('FAMILY_EXEMPTION_RETENTION_DAYS','0'))
    except ValueError: retention=0
    enabled=bool(allowlist.intersection(devices) and models and retention>0)
    return {'identity_mode':'exemption_enabled' if enabled else 'record_only',
            'exemptions_enabled':enabled,'exemption_devices':sorted(allowlist.intersection(devices)) if enabled else []}


def install_member_api(app):
    @app.get('/members')
    def member_page(request:Request):
        current_owner(request)
        return FileResponse(ROOT/'members.html')

    @app.get('/members.js')
    def member_script(request:Request):
        current_owner(request)
        return FileResponse(ROOT/'members.js',media_type='text/javascript')

    @app.get('/api/family/members')
    def members(request:Request):
        uid=current_owner(request)
        with registry_transaction() as c:
            devices=[row['device'] for row in c.execute('SELECT device FROM devices WHERE owner_id=? ORDER BY device',(uid,))]
            return {'members':registry.list_members(c,uid),'devices':devices,**mode_for_devices(devices)}

    @app.post('/api/family/members',status_code=201)
    def create(body:NewMember,request:Request):
        uid=mutation_owner(request)
        with registry_transaction() as c:
            return {'member':registry.create_member(c,uid,body.display_name)}

    @app.patch('/api/family/members/{member_id}')
    def update(member_id:str,body:MemberChange,request:Request):
        uid=mutation_owner(request)
        with registry_transaction() as c:
            return {'member':registry.update_member(c,uid,member_id,display_name=body.display_name,enabled=body.enabled)}

    @app.delete('/api/family/members/{member_id}')
    def delete(member_id:str,request:Request):
        uid=mutation_owner(request)
        with registry_transaction() as c:
            registry.revoke_member(c,uid,member_id,delete=True)
            return {'ok':True}

    @app.post('/api/family/members/{member_id}/enrollments',status_code=201)
    def enrollment(member_id:str,body:Enrollment,request:Request):
        uid=mutation_owner(request)
        with registry_transaction() as c:
            command=registry.create_enrollment(c,uid,member_id,body.device_id,body.request_id)
            return {'command':public_command(command)}
