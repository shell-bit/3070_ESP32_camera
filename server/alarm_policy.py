"""The only event alarm decision; never grants identity from a language model."""
import json
import os
from person_association import validate_actor_assessment

POLICY_VERSION='family-actor-v1'


def configured_devices():
    try: days=int(os.environ.get('FAMILY_EXEMPTION_RETENTION_DAYS','0'))
    except ValueError: return set()
    versions={v.strip() for v in os.environ.get('FACE_RECOGNIZER_VERSIONS','').split(',') if v.strip()}
    return {d.strip() for d in os.environ.get('FAMILY_EXEMPTION_DEVICES','').split(',') if d.strip()} if days>0 and versions else set()


def evaluate_alarm(model_verdict,actions,tracks,registrations,policy_version=POLICY_VERSION,
                   *,enabled=False,evidence_frame_indices=None):
    base={'suspicious':'alert','normal':'no_alert','uncertain':'review'}.get(model_verdict,'review')
    result={'decision':base,'model_verdict':model_verdict,'policy_version':policy_version,
            'reason':'Original behavior verdict; no family exemption','exempted_actions':[],'unexempted_actions':[]}
    if model_verdict=='uncertain' or model_verdict not in ('normal','suspicious'):return result
    if not actions or not tracks:return result
    try:
        indices={o['frame_index'] for t in tracks['tracks'] for o in t['observations']}
        validate_actor_assessment(actions,tracks,indices)
    except (ValueError,TypeError,KeyError):
        result['reason']='Invalid actor extension; original verdict retained'
        return result
    suspicious=[a for a in actions['actions'] if a['verdict']=='suspicious']
    if model_verdict=='normal' and suspicious:
        lookup={t['track_id']:{o['frame_index']:o.get('identity') for o in t['observations']} for t in tracks['tracks']}
        unexempted=any(not enabled or any(not lookup[a['actor_track_id']].get(i) or
            not registrations(lookup[a['actor_track_id']][i]) for i in a['evidence_frame_indices']) for a in suspicious)
        result.update(decision='alert' if unexempted else 'review',reason='Contradictory verdicts; explicit unexempted suspicious actions retained' if unexempted else 'Contradictory event and action verdicts')
        return result
    if model_verdict=='normal':return result
    if not enabled:
        result['reason']='Identity recording mode; family exemption disabled'
        return result
    if actions['coverage']!='complete' or not suspicious or any(a['verdict']=='uncertain' for a in actions['actions']):
        result['reason']='Actor assessment does not cover all suspicious behavior'
        return result
    evidence=set(evidence_frame_indices or [])
    covered={i for a in suspicious for i in a['evidence_frame_indices']}
    if not evidence or not evidence<=covered:
        result['reason']='Event evidence is not fully attributed'
        return result
    track_by_id={t['track_id']:t for t in tracks['tracks']}
    for action in suspicious:
        observations={o['frame_index']:o.get('identity') for o in track_by_id[action['actor_track_id']]['observations']}
        identities=[observations.get(i) for i in action['evidence_frame_indices']]
        accepted=all(identity and registrations(identity) for identity in identities)
        if accepted and len({(i['member_id'],i['member_version']) for i in identities})!=1:accepted=False
        result['exempted_actions' if accepted else 'unexempted_actions'].append(action)
        if accepted:result.setdefault('identity_evidence',[]).append({'actor_track_id':action['actor_track_id'],
            'evidence_frame_indices':action['evidence_frame_indices'],'identity':identities[0]})
    if not result['unexempted_actions']:
        result.update(decision='exempted',reason='All suspicious actors have current authorized family identities')
    else:result['reason']='At least one suspicious actor is unknown, unregistered or ambiguous'
    return result


def decide_in_transaction(c,event,frames,verdict,evidence,actor_assessment=None,tracks=None):
    from face_registry import is_registration_active
    protocol=c.execute('SELECT supports_exempted FROM event_identity_protocol WHERE event_key=?',(event['id'],)).fetchone()
    enabled=bool(protocol and protocol['supports_exempted'] and event['device'] in configured_devices())
    versions={v.strip() for v in os.environ.get('FACE_RECOGNIZER_VERSIONS','').split(',') if v.strip()}
    def active(identity):
        return identity['recognizer_version'] in versions and is_registration_active(c,event['owner_id'],event['device'],
            identity['member_id'],identity['member_version'],identity['recognizer_version'],identity['database_version'])
    decision=evaluate_alarm(verdict,actor_assessment,tracks,active,enabled=enabled,evidence_frame_indices=evidence)
    decision['analysis_version']=1+c.execute('SELECT count(*) FROM alarm_decisions WHERE event_key=?',(event['id'],)).fetchone()[0]
    from store import now
    assessment={'model_verdict':verdict,'actor_assessment':actor_assessment,'tracks':tracks,'evidence_frame_indices':evidence}
    c.execute('INSERT INTO alarm_decisions(event_key,created,assessment,decision) VALUES(?,?,?,?)',
              (event['id'],now(),json.dumps(assessment),json.dumps(decision)))
    return decision
