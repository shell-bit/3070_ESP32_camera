"""Conservative spatial association; missing faces never inherit earlier identities."""
import math


def _box(value):
    return (isinstance(value,(list,tuple)) and len(value)==4 and
            all(type(v) in (int,float) and math.isfinite(v) for v in value) and
            0<=value[0]<value[2] and 0<=value[1]<value[3])


def iou(a,b):
    intersection=max(0,min(a[2],b[2])-max(a[0],b[0]))*max(0,min(a[3],b[3])-max(a[1],b[1]))
    union=(a[2]-a[0])*(a[3]-a[1])+(b[2]-b[0])*(b[3]-b[1])-intersection
    return intersection/union if union else 0


def _contains_head(person,face):
    return (person[0]<=face[0]<face[2]<=person[2] and
            person[1]<=face[1]<face[3]<=person[3] and
            (face[1]+face[3])/2 <= person[1]+0.55*(person[3]-person[1]))


def associate_people(frames,person_detections,face_observations):
    tracks=[]; previous=[]; previous_index=None
    for frame in frames:
        index=frame['photo_index']
        boxes=person_detections.get(index,[])
        if len(boxes)>32 or any(not _box(box) for box in boxes):
            raise ValueError('Invalid person detections')
        identity=face_observations.get(index,{})
        faces=identity.get('faces',[]) if identity.get('frame_identity_status') in ('complete','partial') else []
        per_person=[[] for _ in boxes]
        for face in faces:
            candidates=[i for i,box in enumerate(boxes) if _contains_head(box,face['bbox'])]
            if len(candidates)==1:per_person[candidates[0]].append(face)
        links=[[j for j,(_,old) in enumerate(previous) if iou(box,old)>=0.4] for box in boxes]
        if previous_index is None or index!=previous_index+1:links=[[] for _ in boxes]
        current=[]
        for i,box in enumerate(boxes):
            link=links[i]
            if len(link)==1 and sum(link[0] in other for other in links)==1:
                track=previous[link[0]][0]
            else:
                track={'track_id':'p'+str(len(tracks)+1),'observations':[]};tracks.append(track)
            accepted=None
            # Multiple faces inside one detector box are not a reliable identity.
            if len(per_person[i])==1:
                face=per_person[i][0]
                if face['status']=='matched' and face['face_id'] in identity.get('authorized_faces',[]):
                    accepted={k:face[k] for k in ('member_id','member_version')}
                    accepted.update({k:identity[k] for k in ('recognizer_version','database_version')})
            track['observations'].append({'frame_index':index,'bbox':list(box),'identity':accepted})
            current.append((track,box))
        previous,previous_index=current,index
    # Conflicting identities across a spatial track invalidate the entire track.
    for track in tracks:
        identities={(o['identity']['member_id'],o['identity']['member_version']) for o in track['observations'] if o['identity']}
        if len(identities)>1:
            for observation in track['observations']:observation['identity']=None
    return {'association_version':'spatial-conservative-v1','tracks':tracks}


def validate_actor_assessment(value,track_set,frame_indices):
    if not isinstance(value,dict) or value.get('coverage') not in ('complete','partial','unknown'):
        raise ValueError('Missing actor coverage')
    actions=value.get('actions')
    if not isinstance(actions,list) or len(actions)>32:raise ValueError('Invalid actions')
    tracks={t['track_id']:{o['frame_index'] for o in t['observations']} for t in track_set['tracks']}
    for action in actions:
        if not isinstance(action,dict) or action.get('verdict') not in ('normal','suspicious','uncertain'):
            raise ValueError('Invalid action verdict')
        actor=action.get('actor_track_id'); evidence=action.get('evidence_frame_indices')
        if actor not in tracks:raise ValueError('Unknown action actor')
        if not isinstance(evidence,list) or not evidence or len(set(evidence))!=len(evidence) or any(type(i)is not int or i not in frame_indices or i not in tracks[actor] for i in evidence):
            raise ValueError('Invalid action evidence')
        targets=action.get('target_track_ids')
        if not isinstance(targets,list) or len(set(targets))!=len(targets) or any(t not in tracks or t==actor for t in targets):
            raise ValueError('Invalid target tracks')
        if any(not set(evidence)&tracks[t] for t in targets):raise ValueError('Target absent from action evidence')
    return value
