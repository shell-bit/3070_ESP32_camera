"""V2 upload tests use synthetic JPEGs and private temporary storage."""
import copy
import hashlib
import json
import os
import tempfile
import unittest
import cv2
import numpy as np
from fastapi.testclient import TestClient

temporary = tempfile.TemporaryDirectory(prefix='identity-ingest-')
os.environ['CAMERA_DATA_DIR'] = temporary.name
os.environ.pop('GOOGLE_APPLICATION_CREDENTIALS', None)
import main
import store


class IdentityIngestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.owner = main.app.state.accounts.create('identity-owner', 'Identity-owner-password!', 'viewer')
        cls.client = TestClient(main.app, base_url='https://testserver')
        cls.jpeg = cv2.imencode('.jpg', np.zeros((24, 32, 3), dtype=np.uint8))[1].tobytes()

    def setUp(self):
        with store.db() as c:
            for table in ('frame_identities', 'event_identity_protocol', 'jobs', 'events', 'devices'):
                if c.execute("SELECT 1 FROM sqlite_master WHERE name=?", (table,)).fetchone():
                    c.execute('DELETE FROM ' + table)
            c.execute('INSERT INTO devices VALUES(?,?,?)', ('camera', self.owner, hashlib.sha256(b'camera-token').hexdigest()))

    def metadata(self, index=1):
        return dict(schema_version=1, device_id='camera', event_id='event', photo_index=index,
                    expected_frames=5, jpeg_sha256=hashlib.sha256(self.jpeg).hexdigest(),
                    width=32, height=24, captured_at=None, night_capture=False,
                    recognizer_version='test-v1', database_version='db-v1',
                    frame_identity_status='complete', faces=[], supports_exempted=True)

    def upload(self, metadata=None, token='camera-token'):
        return self.client.post('/api/device/upload-v2', headers={'Authorization': 'Bearer ' + token},
            files={'photo': ('frame.jpg', self.jpeg, 'image/jpeg'),
                   'metadata': (None, json.dumps(metadata or self.metadata()), 'application/json')})

    def test_saves_bound_identity_and_duplicate_conflicts(self):
        response = self.upload()
        self.assertEqual(response.status_code, 201, response.text)
        self.assertTrue(response.json()['saved'])
        self.assertTrue(self.upload().json()['duplicate'])
        changed = self.metadata(); changed['frame_identity_status'] = 'not_run'
        self.assertEqual(self.upload(changed).status_code, 409)
        with store.db() as c:
            row = c.execute('SELECT metadata FROM frame_identities').fetchone()
            self.assertEqual(json.loads(row['metadata'])['jpeg_sha256'], hashlib.sha256(self.jpeg).hexdigest())

    def test_rejects_bad_digest_size_status_and_unbounded_faces(self):
        for field, value in [('jpeg_sha256', 'f'*64), ('width', 33), ('height', True),
                             ('schema_version', 2), ('frame_identity_status', 'family'),
                             ('night_capture', 'false'), ('expected_frames', 1)]:
            metadata = self.metadata(); metadata[field] = value
            self.assertEqual(self.upload(metadata).status_code, 422, (field, value))
        metadata = self.metadata(); metadata['faces'] = [{}] * 9
        self.assertEqual(self.upload(metadata).status_code, 422)

    def test_rejects_out_of_range_boxes_and_nonfinite_scores(self):
        face = dict(face_id='f1', bbox=[1, 1, 12, 12], status='unknown', member_id=None,
                    match_score=0.1, member_version=None)
        for box in ([-1, 1, 12, 12], [1, 1, 33, 12], [12, 1, 1, 12], [1, 1, float('nan'), 12]):
            metadata = self.metadata(); metadata['faces'] = [dict(face, bbox=box)]
            self.assertEqual(self.upload(metadata).status_code, 422)
        metadata = self.metadata(); metadata['faces'] = [dict(face, match_score=float('nan'))]
        self.assertEqual(self.upload(metadata).status_code, 422)

    def test_requires_registered_owned_device_and_no_cookie_auth(self):
        self.assertEqual(self.upload(token='wrong').status_code, 401)
        metadata = self.metadata(); metadata['device_id'] = 'unregistered'
        self.assertEqual(self.upload(metadata).status_code, 403)

    def test_protocol_mixing_rejected_both_directions(self):
        self.assertEqual(self.upload().status_code, 201)
        headers = {'Content-Type':'image/jpeg', 'Authorization':'Bearer camera-token',
                   'X-Device-ID':'camera', 'X-Event-ID':'event', 'X-Photo-Index':'2'}
        self.assertEqual(self.client.post('/api/device/upload', headers=headers, content=self.jpeg).status_code, 409)
        headers['X-Event-ID'] = 'legacy'
        self.assertEqual(self.client.post('/api/device/upload', headers=headers, content=self.jpeg).status_code, 201)
        metadata = self.metadata(); metadata['event_id'] = 'legacy'
        self.assertEqual(self.upload(metadata).status_code, 409)

    def test_deadline_and_multipart_duplicate_fields(self):
        self.assertEqual(self.upload().status_code, 201)
        with store.db() as c: c.execute('UPDATE events SET deadline=0')
        self.assertEqual(self.upload(self.metadata(2)).status_code, 409)
        self.assertTrue(self.upload().json()['duplicate'])
        response = self.client.post('/api/device/upload-v2', headers={'Authorization':'Bearer camera-token'},
            files=[('photo',('a.jpg',self.jpeg,'image/jpeg')), ('metadata',(None,json.dumps(self.metadata()))),
                   ('metadata',(None,json.dumps(self.metadata())))])
        self.assertEqual(response.status_code, 422)


if __name__ == '__main__': unittest.main()
