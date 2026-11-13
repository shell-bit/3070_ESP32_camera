"""Event-specific alarm status for the existing SQLite-backed AI dashboard."""
import os
import re
import secrets


def event_status(rows, expected):
    """An empty/missing/error result must never be interpreted as normal."""
    if not rows:
        return "not_found"
    if any(row["state"] == "suspicious" for row in rows):
        return "suspicious"
    by_index = {int(row["photo_index"]): row["state"] for row in rows}
    if any(i not in by_index for i in range(1, expected + 1)):
        return "pending"
    states = [by_index[i] for i in range(1, expected + 1)]
    if any(state in ("queued", "processing") for state in states):
        return "pending"
    if any(state not in ("discarded", "normal") for state in states):
        return "review"
    return "normal"


def install_device_alarm_api(app, db):
    from fastapi import HTTPException, Query, Request
    from fastapi.responses import PlainTextResponse

    @app.get("/api/device/event-status", response_class=PlainTextResponse)
    def status(request: Request, device_id: str, event_id: str,
               expected: int = Query(5, ge=1, le=5)):
        from ownership import device_owner
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", device_id):
            raise HTTPException(400, "Invalid device ID")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,48}", event_id):
            raise HTTPException(400, "Invalid event ID")
        with db() as connection:
            owner=device_owner(request,device_id,connection)
            if connection.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='events'").fetchone():
                from event_queue import expire_events
                expire_events(connection)
                event=connection.execute('SELECT state FROM events WHERE device=? AND event=? AND owner_id IS ?',
                                         (device_id,event_id,owner)).fetchone()
                if event:
                    result={'collecting':'pending','queued':'pending','processing':'pending',
                            'suspicious':'suspicious','discarded':'normal','review':'review','exempted':'exempted'}.get(event['state'],'review')
                    return PlainTextResponse(result,headers={'Cache-Control':'no-store, max-age=0'})
            rows = connection.execute(
                "SELECT photo_index,state FROM jobs WHERE device=? AND event=? AND owner_id IS ?",
                (device_id, event_id, owner),
            ).fetchall()
        return PlainTextResponse(event_status(rows, expected), headers={
            "Cache-Control": "no-store, max-age=0",
        })
