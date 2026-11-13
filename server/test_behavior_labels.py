"""Behavior labels regression tests. Run in an isolated source copy."""
import json
import logging
import os
import tempfile
import unittest
import uuid
from pathlib import Path

temporary = tempfile.TemporaryDirectory(prefix='behavior-tests-')
os.environ['CAMERA_DATA_DIR'] = temporary.name
os.environ.pop('GOOGLE_APPLICATION_CREDENTIALS', None)
import store
import worker
from manual_review import decide
logging.disable(logging.CRITICAL)

class Models:
    def __init__(self, result, person=True):
        self.result = result
        self.person = person
    def has_person(self, path):
        return self.person
    def assess(self, path):
        if not self.person:
            raise AssertionError('No-person branch must skip Qwen')
        return self.result

class BehaviorLabelTests(unittest.TestCase):
    def setUp(self):
        self.assertEqual(store.DATA, Path(temporary.name).resolve())
        with store.db() as connection:
            connection.execute('DELETE FROM jobs')
            connection.execute('DELETE FROM push_events')
            connection.execute('DELETE FROM push_deliveries')
        for folder in (store.INCOMING, store.PHOTOS):
            for path in folder.glob('*.jpg'):
                path.unlink()

    def seed(self, state='queued'):
        key = uuid.uuid4().hex * 2
        with store.db() as connection:
            connection.execute(
                'INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id) VALUES(?,?,?,?,?,?,?,?,?)',
                (key, key, 'camera', key, 1, 0, store.now(), state, 7))
        (store.INCOMING / (key + '.jpg')).write_bytes(b'synthetic test fixture')
        return key
    def row(self, key):
        with store.db() as connection:
            return dict(connection.execute('SELECT * FROM jobs WHERE id=?', (key,)).fetchone())
    def pushed(self, key):
        with store.db() as connection:
            return connection.execute('SELECT count(*) FROM push_events WHERE id=?', (key,)).fetchone()[0]
    def assert_alert(self, key, label):
        row = self.row(key)
        self.assertEqual(row['state'], 'suspicious')
        self.assertEqual(json.loads(row['report'])['activity_type'], label)
        self.assertEqual(store.report_for(row)['status'], 'suspicious')
        self.assertTrue((store.PHOTOS / (key + '.jpg')).is_file())
        self.assertFalse((store.INCOMING / (key + '.jpg')).exists())
        self.assertEqual(self.pushed(key), 1)
    def test_worker_preserves_three_subtypes_and_alerts(self):
        for label in ('forced_entry', 'suspected_theft', 'violence'):
            with self.subTest(label=label):
                key = self.seed()
                worker.process_one(Models(('suspicious', 'Visible test action', label)))
                self.assert_alert(key, label)
    def test_other_and_missing_subtype_still_alert(self):
        for result in (('suspicious', 'Damage to property'),
                       ('suspicious', 'Damage to property', 'suspicious'),
                       ('suspicious', 'Damage to property', 'property_damage'),
                       ('suspicious', 'Damage to property', None)):
            with self.subTest(result=result):
                key = self.seed()
                worker.process_one(Models(result))
                self.assert_alert(key, 'suspicious')
    def test_parser_and_legacy_two_value_interface(self):
        for label in ('forced_entry', 'suspected_theft', 'violence', 'suspicious'):
            payload = json.dumps({'verdict':'suspicious', 'description':'Visible action', 'activity_type':label})
            self.assertEqual(worker.parse_assessment(payload), ('suspicious', 'Visible action', label))
            self.assertEqual(worker.parse_answer(payload), ('suspicious', 'Visible action'))
        old = '{"verdict":"suspicious","description":"Visible action"}'
        self.assertEqual(worker.parse_assessment(old)[2], 'suspicious')
        self.assertEqual(worker.parse_assessment('```json\n' + old + '\n```')[2], 'suspicious')
        unknown = '{"verdict":"suspicious","description":"Damage","activity_type":"property_damage"}'
        self.assertEqual(worker.parse_assessment(unknown)[2], 'suspicious')
    def test_parser_rejects_bad_or_contradictory_outputs(self):
        for value in [
            {'verdict':'normal','description':'Walking','activity_type':'violence'},
            {'verdict':'uncertain','description':'Obscured','activity_type':'forced_entry'},
            {'verdict':'suspicious','description':'Action','activity_type':['violence']},
            {'verdict':'safe','description':'Walking'},
            {'verdict':'suspicious','description':' '},
            ['suspicious', 'Action'],
        ]:
            with self.subTest(value=value), self.assertRaises((ValueError, TypeError)):
                worker.parse_answer(json.dumps(value))
    def test_normal_and_no_person_have_no_subtype(self):
        for person in (True, False):
            key = self.seed()
            worker.process_one(Models(('normal', 'Walking'), person))
            row = self.row(key)
            self.assertEqual(row['state'], 'discarded')
            self.assertNotIn('activity_type', store.report_for(row))
            self.assertFalse((store.INCOMING / (key + '.jpg')).exists())
            self.assertEqual(self.pushed(key), 0)
    def test_uncertain_and_invalid_combinations_keep_image_for_review(self):
        for result in [('uncertain','Action obscured',None),
                       ('normal','Walking','violence'),
                       ('suspicious','Action',[])]:
            key = self.seed()
            worker.process_one(Models(result))
            row = self.row(key)
            self.assertEqual(row['state'], 'review')
            self.assertTrue(row['error'])
            self.assertTrue((store.INCOMING / (key + '.jpg')).is_file())
            self.assertFalse((store.PHOTOS / (key + '.jpg')).exists())
            self.assertEqual(self.pushed(key), 0)
    def test_old_reports_get_generic_label_without_rewriting(self):
        original = '{"status":"suspicious","description":"Old report"}'
        row = {'report':original}
        self.assertEqual(store.report_for(row)['activity_type'], 'suspicious')
        self.assertEqual(row['report'], original)
        for status in ('normal', 'uncertain'):
            self.assertNotIn('activity_type', store.report_for({'report':json.dumps({'status':status})}))
        self.assertIsNone(store.report_for({'report':None}))
    def test_old_finish_call_keeps_generic_alert(self):
        key = self.seed()
        store.finish(self.row(key), 'suspicious', 'Damage')
        self.assert_alert(key, 'suspicious')
    def test_finish_rejects_contradiction_before_photo_deletion(self):
        key = self.seed()
        with self.assertRaises(ValueError):
            store.finish(self.row(key), 'normal', 'Walking', 'violence')
        self.assertTrue((store.INCOMING / (key + '.jpg')).is_file())
        self.assertEqual(self.row(key)['state'], 'queued')
    def test_manual_keep_dismiss_retry_labels(self):
        for action in ('keep', 'dismiss', 'retry'):
            key = self.seed('review')
            with store.db() as connection:
                connection.execute('UPDATE jobs SET report=? WHERE id=?',
                    (json.dumps({'status':'suspicious','activity_type':'violence'}), key))
            decide(key, action, 'Human review', owner_id=7)
            row = self.row(key)
            if action == 'keep':
                self.assert_alert(key, 'suspicious')
            elif action == 'dismiss':
                self.assertEqual(row['state'], 'discarded')
                self.assertNotIn('activity_type', json.loads(row['report']))
            else:
                self.assertEqual(row['state'], 'queued')
                self.assertIsNone(row['report'])
                worker.process_one(Models(('suspicious', 'Forced door', 'forced_entry')))
                self.assert_alert(key, 'forced_entry')

if __name__ == '__main__':
    try:
        unittest.main()
    finally:
        temporary.cleanup()
