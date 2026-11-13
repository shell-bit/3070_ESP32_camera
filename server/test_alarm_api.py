import os
import sqlite3
import unittest
from contextlib import contextmanager
from unittest.mock import patch

from fastapi import FastAPI
from fastapi.testclient import TestClient
from device_alarm_api import install_device_alarm_api


class AlarmApiTests(unittest.TestCase):
    def setUp(self):
        self.connection = sqlite3.connect(':memory:', check_same_thread=False)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute('CREATE TABLE jobs (device TEXT, event TEXT, photo_index INTEGER, state TEXT, owner_id INTEGER)')
        self.connection.execute('CREATE TABLE devices (device TEXT PRIMARY KEY, owner_id INTEGER, token_digest TEXT)')
        @contextmanager
        def db():
            yield self.connection
        app = FastAPI()
        install_device_alarm_api(app, db)
        self.client = TestClient(app)
        self.env = patch.dict(os.environ, {'UPLOAD_TOKEN': ''})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.client.close()
        self.connection.close()

    def add(self, index, state, device='camera', event='current'):
        self.connection.execute('INSERT INTO jobs(device,event,photo_index,state) VALUES (?,?,?,?)', (device, event, index, state))

    def get(self, **kwargs):
        return self.client.get('/api/device/event-status', params={
            'device_id': 'camera', 'event_id': 'current', 'expected': 5,
        }, **kwargs)

    def test_old_event_and_other_device_cannot_trigger(self):
        self.add(1, 'suspicious', event='old')
        self.add(1, 'suspicious', device='other')
        self.assertEqual(self.get().text, 'not_found')

    def test_current_suspicious_triggers_before_other_photos_finish(self):
        self.add(1, 'processing')
        self.add(2, 'suspicious')
        self.assertEqual(self.get().text, 'suspicious')

    def test_missing_photos_do_not_mean_normal(self):
        self.add(1, 'discarded')
        self.assertEqual(self.get().text, 'pending')

    def test_all_normal(self):
        for i in range(1, 6):
            self.add(i, 'discarded')
        result = self.get()
        self.assertEqual(result.text, 'normal')
        self.assertIn('no-store', result.headers['cache-control'])
        self.assertEqual(result.headers['content-length'], '6')

    def test_wait_for_queued_or_processing(self):
        for state in ['queued', 'processing']:
            with self.subTest(state=state):
                self.connection.execute('DELETE FROM jobs')
                for i in range(1, 6):
                    self.add(i, state if i == 5 else 'discarded')
                self.assertEqual(self.get().text, 'pending')

    def test_review_failure_unknown_do_not_trigger(self):
        for state in ['review', 'failed', 'unexpected']:
            with self.subTest(state=state):
                self.connection.execute('DELETE FROM jobs')
                for i in range(1, 6):
                    self.add(i, state if i == 5 else 'discarded')
                self.assertEqual(self.get().text, 'review')

    def test_authentication(self):
        with patch.dict(os.environ, {'UPLOAD_TOKEN': 'test-secret'}):
            self.assertEqual(self.get().status_code, 401)
            self.assertEqual(self.get(headers={'Authorization': 'Bearer wrong'}).status_code, 401)
            self.assertEqual(self.get(headers={'Authorization': 'Bearer test-secret'}).status_code, 200)

    def test_invalid_query(self):
        result = self.client.get('/api/device/event-status', params={
            'device_id': 'camera', 'event_id': "x' OR 1=1--", 'expected': 5})
        self.assertEqual(result.status_code, 400)
        result = self.client.get('/api/device/event-status', params={
            'device_id': 'camera', 'event_id': 'current', 'expected': 6})
        self.assertEqual(result.status_code, 422)


if __name__ == '__main__':
    unittest.main()
