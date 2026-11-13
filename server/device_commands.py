"""Camera command API. Legacy anonymous upload access is deliberately inapplicable."""
from typing import Literal
from fastapi import HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field
from ownership import device_owner
from member_api import registry_transaction, public_command, public_registration
import face_registry as registry


class CommandAck(BaseModel):
    model_config=ConfigDict(extra='forbid')
    device_id: str=Field(pattern=r'^[A-Za-z0-9_-]{1,40}$')
    member_version: int=Field(ge=1,strict=True)
    result: Literal['sampling','committed','failed']
    database_version: str | None=Field(default=None,min_length=1,max_length=128)
    recognizer_version: str | None=Field(default=None,min_length=1,max_length=128)
    error: str | None=Field(default=None,min_length=1,max_length=500)


class DeviceState(BaseModel):
    model_config=ConfigDict(extra='forbid')
    device_id: str=Field(pattern=r'^[A-Za-z0-9_-]{1,40}$')
    recognizer_version: str=Field(min_length=1,max_length=128)
    database_version: str=Field(min_length=1,max_length=128)
    compatible: bool=True
    reset: bool=False


def registered_device_owner(request,device_id,connection):
    row=connection.execute('SELECT owner_id FROM devices WHERE device=?',(device_id,)).fetchone()
    if row is None or row['owner_id'] is None:
        raise HTTPException(403,'Register and assign this camera before using family recognition.')
    return device_owner(request,device_id,connection)


def install_device_commands(app):
    @app.get('/api/device/commands')
    def commands(request:Request,device_id:str):
        with registry_transaction() as c:
            owner=registered_device_owner(request,device_id,c)
            return {'commands':[public_command(row) for row in registry.commands_for_device(c,owner,device_id)]}

    @app.post('/api/device/commands/{command_id}/ack')
    def ack(command_id:str,body:CommandAck,request:Request):
        with registry_transaction() as c:
            owner=registered_device_owner(request,body.device_id,c)
            registration=registry.ack_command(c,owner,body.device_id,command_id,body.member_version,
                body.result,body.database_version,body.recognizer_version,body.error)
            return {'registration':public_registration(registration)}

    @app.post('/api/device/identity-state')
    def identity_state(body:DeviceState,request:Request):
        with registry_transaction() as c:
            owner=registered_device_owner(request,body.device_id,c)
            return registry.report_device_state(c,owner,body.device_id,body.recognizer_version,
                                                body.database_version,body.compatible,body.reset)
