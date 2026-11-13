"""Event queue contracts; synthetic byte files only, no model/GPU dependencies."""
import importlib.util
import json
import os
import sqlite3
import tempfile
import unittest
import uuid
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from pathlib import Path
from unittest.mock import patch

temporary = tempfile.TemporaryDirectory(prefix='event-core-tests-')
os.environ['CAMERA_DATA_DIR'] = temporary.name
import store
import manual_review


class EventQueueTests(unittest.TestCase):
    def setUp(self):
        # A missing implementation is an assertion failure during the first red run.
        self.assertIsNotNone(importlib.util.find_spec('event_queue'), 'event queue implementation is missing')
        import event_queue
        self.queue = event_queue
        with store.db() as connection:
            for table in ('jobs', 'events', 'push_events', 'push_deliveries'):
                connection.execute('DELETE FROM ' + table)
        for folder in (store.INCOMING, store.PHOTOS):
            for path in folder.glob('*.jpg'):
                path.unlink()

    def seed(self, indices=(1, 2, 3, 4, 5), expected=5, owner=7, raw=None):
        raw = raw or uuid.uuid4().hex
        with store.db() as connection:
            connection.execute('BEGIN IMMEDIATE')
            event = self.queue.ensure_event(connection, 'camera', raw, owner, expected)
            for index in indices:
                key = uuid.uuid4().hex * 2
                connection.execute('''INSERT INTO jobs
                    (id,digest,device,event,photo_index,night,received,state,owner_id,event_key,captured_at)
                    VALUES(?,?,?,?,?,?,?,?,?,?,?)''',
                    (key, key, 'camera', raw, index, 0, store.now(), 'collecting', owner, event['id'], None))
                (store.INCOMING / (key + '.jpg')).write_bytes(b'synthetic frame ' + str(index).encode())
                self.queue.register_frame(connection, event['id'])
            return event['id']

    def rows(self, key):
        with store.db() as connection:
            event = dict(connection.execute('SELECT * FROM events WHERE id=?', (key,)).fetchone())
            return event, self.queue.frames_for(connection, key)

    def pushed(self):
        with store.db() as connection:
            return [dict(row) for row in connection.execute('SELECT * FROM push_events')]

    def claim(self):
        result = self.queue.claim_event()
        self.assertIsNotNone(result)
        return result

    def test_waits_for_all_indices_then_claims_in_index_order_once(self):
        key = self.seed((5, 3, 1, 4))
        self.assertEqual(self.rows(key)[0]['state'], 'collecting')
        self.assertIsNone(self.queue.claim_event())
        with store.db() as connection:
            frame_id = uuid.uuid4().hex * 2
            connection.execute('''INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,owner_id,event_key)
                VALUES(?,?,?,?,?,?,?,?,?,?)''', (frame_id, frame_id, 'camera', self.rows(key)[0]['event'], 2, 0, store.now(), 'collecting', 7, key))
            (store.INCOMING / (frame_id + '.jpg')).write_bytes(b'synthetic frame 2')
            self.assertEqual(self.queue.register_frame(connection, key), 'queued')
        event, frames = self.claim()
        self.assertEqual(event['id'], key)
        self.assertEqual([frame['photo_index'] for frame in frames], [1, 2, 3, 4, 5])
        self.assertEqual({frame['state'] for frame in frames}, {'processing'})
        self.assertIsNone(self.queue.claim_event())

    def test_first_upload_freezes_owner_expected_count_and_deadline(self):
        with patch.dict(os.environ, {'EVENT_WAIT_SECONDS': '12.5'}), patch('event_queue.time.time', return_value=100):
            key = self.seed((1,))
        event, _ = self.rows(key)
        self.assertEqual(event['deadline'], 112.5)
        with patch('event_queue.time.time', return_value=110), store.db() as connection:
            connection.execute('BEGIN IMMEDIATE')
            same = self.queue.ensure_event(connection, 'camera', event['event'], 7, 5)
            self.assertEqual(same['deadline'], 112.5)
            for owner, count in ((8, 5), (7, 4), (None, 5)):
                with self.assertRaises(self.queue.EventConflict):
                    self.queue.ensure_event(connection, 'camera', event['event'], owner, count)

    def test_rejects_invalid_configuration_and_out_of_range_indices(self):
        for value in ('0', '-1', 'nan', 'inf', 'bad'):
            with self.subTest(value=value), patch.dict(os.environ, {'EVENT_WAIT_SECONDS': value}):
                with self.assertRaises(ValueError):
                    self.seed(())
        for count in (0, 6, True, 1.5):
            with self.subTest(count=count), self.assertRaises(ValueError):
                self.seed((), expected=count)
        for index in (0, 6):
            with self.subTest(index=index), self.assertRaises(self.queue.EventConflict):
                self.seed((index,))

    def test_deadline_creates_private_incomplete_report_without_push(self):
        with patch('event_queue.time.time', return_value=100):
            key = self.seed((4, 2))
        with patch('event_queue.time.time', return_value=131):
            self.assertEqual(self.queue.expire_events(), 1)
            self.assertEqual(self.queue.expire_events(), 0)
        event, frames = self.rows(key)
        report = json.loads(event['report'])
        self.assertEqual(event['state'], 'review')
        self.assertEqual({frame['state'] for frame in frames}, {'review'})
        self.assertFalse(report['input_complete'])
        self.assertEqual(report['frame_count'], 2)
        self.assertEqual(report['missing_indices'], [1, 3, 5])
        self.assertEqual(report['evidence_frame_indices'], [])
        self.assertIn('/api/review/' + key + '/photo?frame_index=2', report['photo_url'])
        self.assertEqual(self.pushed(), [])
        self.assertTrue(all((store.INCOMING / (frame['id'] + '.jpg')).is_file() for frame in frames))
        self.assertIsNone(self.queue.claim_event())

    def test_suspicious_retains_every_frame_one_event_report_one_outbox(self):
        key = self.seed((5, 1, 3, 2, 4))
        event, frames = self.claim()
        report = self.queue.finish_event(event, frames, 'suspicious', 'Visible forced door', 'forced_entry', [4, 2])
        saved, saved_frames = self.rows(key)
        self.assertEqual(saved['state'], 'suspicious')
        self.assertEqual({frame['state'] for frame in saved_frames}, {'suspicious'})
        self.assertEqual(report['id'], key)
        self.assertEqual(report['event_id'], event['event'])
        self.assertEqual(report['frame_count'], 5)
        self.assertTrue(report['input_complete'])
        self.assertEqual(report['missing_indices'], [])
        self.assertEqual(report['evidence_frame_indices'], [4, 2])
        self.assertEqual(report['activity_type'], 'forced_entry')
        self.assertEqual(report['photo_url'], '/photos/' + frames[3]['id'] + '.jpg')
        self.assertEqual([frame['photo_index'] for frame in report['frames']], [1, 2, 3, 4, 5])
        self.assertTrue(all((store.PHOTOS / (frame['id'] + '.jpg')).is_file() for frame in frames))
        self.assertTrue(all(not (store.INCOMING / (frame['id'] + '.jpg')).exists() for frame in frames))
        self.assertEqual([row['id'] for row in self.pushed()], [key])
        with self.assertRaises(self.queue.EventConflict):
            self.queue.finish_event(event, frames, 'normal', 'Stale result')
        self.queue.fail_event(key, 'Stale error')
        self.assertEqual(self.rows(key)[0]['state'], 'suspicious')

    def test_normal_deletes_all_after_committing_discard(self):
        key = self.seed()
        event, frames = self.claim()
        original = Path.unlink
        seen = []
        def check_committed(path, *args, **kwargs):
            seen.append(self.rows(key)[0]['state'])
            return original(path, *args, **kwargs)
        with patch.object(Path, 'unlink', check_committed):
            report = self.queue.finish_event(event, frames, 'normal', 'Ordinary walking')
        self.assertTrue(seen)
        self.assertEqual(set(seen), {'discarded'})
        self.assertNotIn('activity_type', report)
        self.assertEqual(self.pushed(), [])
        self.assertTrue(all(not (store.INCOMING / (frame['id'] + '.jpg')).exists() for frame in frames))

    def test_uncertain_retains_frames_and_empty_evidence(self):
        key = self.seed()
        event, frames = self.claim()
        report = self.queue.finish_event(event, frames, 'uncertain', 'Actions obscured')
        self.assertEqual(self.rows(key)[0]['state'], 'review')
        self.assertEqual(report['evidence_frame_indices'], [])
        self.assertNotIn('activity_type', report)
        self.assertEqual(self.pushed(), [])
        self.assertTrue(all((store.INCOMING / (frame['id'] + '.jpg')).is_file() for frame in frames))

    def test_rejects_invalid_evidence_and_contradictions_without_deleting(self):
        key = self.seed()
        event, frames = self.claim()
        for evidence in ([True], [0], [6], [1, 1], [1.5], '1'):
            with self.subTest(evidence=evidence), self.assertRaises(ValueError):
                self.queue.finish_event(event, frames, 'suspicious', 'Visible action', evidence_frame_indices=evidence)
        with self.assertRaises(ValueError):
            self.queue.finish_event(event, frames, 'normal', 'Walking', 'violence')
        self.assertTrue(all((store.INCOMING / (frame['id'] + '.jpg')).is_file() for frame in frames))
        self.queue.fail_event(key, 'Invalid model answer')
        self.assertEqual(self.rows(key)[0]['state'], 'review')
        self.assertEqual(self.pushed(), [])

    def test_partial_move_failure_and_restart_can_finish_from_both_directories(self):
        key = self.seed()
        event, frames = self.claim()
        original = Path.replace
        moved = []
        def interrupt(path, target):
            if len(moved) == 2:
                raise OSError('synthetic interrupted move')
            moved.append(path)
            return original(path, target)
        with patch.object(Path, 'replace', interrupt), self.assertRaises(OSError):
            self.queue.finish_event(event, frames, 'suspicious', 'Visible damage')
        self.assertEqual(self.rows(key)[0]['state'], 'processing')
        self.assertEqual(self.pushed(), [])
        self.assertEqual(sum((store.PHOTOS / (frame['id'] + '.jpg')).is_file() for frame in frames), 2)
        store.recover()
        event, frames = self.claim()
        self.queue.finish_event(event, frames, 'suspicious', 'Visible damage')
        self.assertEqual(self.rows(key)[0]['state'], 'suspicious')
        self.assertEqual(len(self.pushed()), 1)

    def test_outbox_failure_rolls_back_verdict_and_recovery_preserves_files(self):
        key = self.seed()
        event, frames = self.claim()
        with patch('push_events.enqueue_event', side_effect=sqlite3.OperationalError('synthetic outbox failure'), create=True):
            with self.assertRaises(sqlite3.OperationalError):
                self.queue.finish_event(event, frames, 'suspicious', 'Visible damage')
        self.assertEqual(self.rows(key)[0]['state'], 'processing')
        self.assertEqual(self.pushed(), [])
        store.recover()
        event, frames = self.claim()
        self.queue.finish_event(event, frames, 'normal', 'Ordinary handling')
        self.assertEqual(self.rows(key)[0]['state'], 'discarded')
        self.assertTrue(all(not (store.PHOTOS / (frame['id'] + '.jpg')).exists() for frame in frames))

    def test_manual_keep_is_whole_event_and_owner_guarded(self):
        key = self.seed((2, 4))
        with store.db() as connection:
            connection.execute('UPDATE events SET deadline=0 WHERE id=?', (key,))
        self.queue.expire_events()
        with self.assertRaises(FileNotFoundError):
            manual_review.decide(key, 'keep', 'Human decision', owner_id=8)
        result = manual_review.decide(key, 'keep', 'Human decision', owner_id=7)
        self.assertEqual(result['state'], 'suspicious')
        event, frames = self.rows(key)
        report = json.loads(event['report'])
        self.assertEqual(report['review_source'], 'human')
        self.assertEqual(report['activity_type'], 'suspicious')
        self.assertFalse(report['input_complete'])
        self.assertEqual({frame['state'] for frame in frames}, {'suspicious'})
        self.assertEqual(len(self.pushed()), 1)
        with self.assertRaises(manual_review.ReviewConflict):
            manual_review.decide(key, 'dismiss', 'Second decision', owner_id=7)

    def test_manual_retry_requires_complete_existing_files_and_clears_error_report(self):
        incomplete = self.seed((1, 2))
        with store.db() as connection:
            connection.execute('UPDATE events SET deadline=0 WHERE id=?', (incomplete,))
        self.queue.expire_events()
        with self.assertRaises(manual_review.ReviewConflict):
            manual_review.decide(incomplete, 'retry', 'Retry', owner_id=7)
        key = self.seed()
        event, frames = self.claim()
        self.queue.fail_event(key, 'Temporary model error')
        missing = store.INCOMING / (frames[2]['id'] + '.jpg')
        missing.unlink()
        with self.assertRaises(FileNotFoundError):
            manual_review.decide(key, 'retry', 'Retry', owner_id=7)
        missing.write_bytes(b'synthetic restored fixture')
        manual_review.decide(key, 'retry', 'Retry', owner_id=7)
        event, frames = self.rows(key)
        self.assertEqual(event['state'], 'queued')
        self.assertIsNone(event['report'])
        self.assertIsNone(event['error'])
        self.assertEqual({frame['state'] for frame in frames}, {'queued'})
        self.assertEqual(self.pushed(), [])

    def test_frame_id_cannot_bypass_whole_event_manual_review(self):
        key = self.seed()
        event, frames = self.claim()
        self.queue.fail_event(key, 'Review required')
        result = manual_review.decide(frames[0]['id'], 'dismiss', 'Normal event', owner_id=7)
        self.assertEqual(result['job_id'], key)
        self.assertEqual(self.rows(key)[0]['state'], 'discarded')
        self.assertTrue(all(not (store.INCOMING / (frame['id'] + '.jpg')).exists() for frame in frames))

    def test_recovery_only_requeues_processing_groups_and_keeps_legacy_independent(self):
        key = self.seed()
        self.claim()
        legacy = uuid.uuid4().hex * 2
        with store.db() as connection:
            connection.execute('''INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state)
                VALUES(?,?,?,?,?,?,?,?)''', (legacy, legacy, 'camera', 'old burst', 1, 0, store.now(), 'processing'))
        store.recover()
        self.assertEqual(self.rows(key)[0]['state'], 'queued')
        self.assertEqual({frame['state'] for frame in self.rows(key)[1]}, {'queued'})
        with store.db() as connection:
            row = connection.execute('SELECT * FROM jobs WHERE id=?', (legacy,)).fetchone()
            self.assertEqual(row['state'], 'queued')
            self.assertIsNone(row['event_key'])
        self.assertEqual(self.claim()[0]['id'], key)

    def test_expiry_accepts_callers_transaction_and_ignores_pre_event_databases(self):
        with sqlite3.connect(':memory:') as connection:
            self.assertEqual(self.queue.expire_events(connection), 0)
        key = self.seed((1,))
        with store.db() as connection:
            connection.execute('BEGIN IMMEDIATE')
            connection.execute('UPDATE events SET deadline=0 WHERE id=?', (key,))
            self.assertEqual(self.queue.expire_events(connection), 1)
            self.assertEqual(connection.execute('SELECT state FROM events WHERE id=?', (key,)).fetchone()[0], 'review')
            connection.rollback()
        self.assertEqual(self.rows(key)[0]['state'], 'collecting')

    def test_concurrent_claims_cannot_analyze_the_same_event(self):
        key = self.seed()
        with ThreadPoolExecutor(max_workers=4) as executor:
            claimed = list(executor.map(lambda unused: self.queue.claim_event(), range(4)))
        nonempty = [value for value in claimed if value is not None]
        self.assertEqual([value[0]['id'] for value in nonempty], [key])

    def test_explicit_legacy_expected_counts_queue_only_at_configured_size(self):
        for expected in (1, 2, 3, 4):
            with self.subTest(expected=expected):
                key = self.seed(tuple(range(1, expected + 1)), expected=expected)
                event, frames = self.claim()
                self.assertEqual(event['id'], key)
                self.assertEqual(len(frames), expected)
                report = self.queue.finish_event(event, frames, 'suspicious', 'Visible damage', 'unknown_subtype')
                self.assertEqual(report['activity_type'], 'suspicious')
                self.assertEqual(report['evidence_frame_indices'], [])

    def test_recovery_cleans_a_post_commit_delete_failure(self):
        key = self.seed()
        event, frames = self.claim()
        with patch.object(Path, 'unlink', side_effect=OSError('synthetic interrupted delete')):
            with self.assertRaises(OSError):
                self.queue.finish_event(event, frames, 'normal', 'Ordinary walking')
        self.assertEqual(self.rows(key)[0]['state'], 'discarded')
        self.queue.fail_event(key, 'Cleanup error cannot overwrite committed verdict')
        self.assertEqual(self.rows(key)[0]['state'], 'discarded')
        store.recover()
        self.assertTrue(all(not (store.INCOMING / (frame['id'] + '.jpg')).exists() for frame in frames))

    def test_additive_migration_preserves_legacy_rows_reports_and_frame_semantics(self):
        with tempfile.TemporaryDirectory(prefix='event-legacy-migration-') as directory:
            data = Path(directory)
            with closing(sqlite3.connect(data / 'queue.sqlite3')) as connection:
                connection.execute('''CREATE TABLE jobs (
                    seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT UNIQUE NOT NULL,digest TEXT NOT NULL,
                    device TEXT NOT NULL,event TEXT NOT NULL,photo_index INTEGER NOT NULL,night INTEGER NOT NULL,
                    received TEXT NOT NULL,state TEXT NOT NULL,report TEXT,error TEXT)''')
                for index in (1, 2):
                    connection.execute('INSERT INTO jobs VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                        (index, str(index) * 64, 'digest', 'old camera', 'same old event', index, 0,
                         '2020-01-01T00:00:00+00:00', 'suspicious', '{"status":"suspicious","description":"Historical report"}', None))
                connection.commit()
            with patch.object(store, 'DATA', data), store.db() as connection:
                rows = connection.execute('SELECT * FROM jobs ORDER BY seq').fetchall()
                self.assertEqual(len(rows), 2)
                self.assertEqual([row['seq'] for row in rows], [1, 2])
                self.assertTrue(all(row['event_key'] is None and row['captured_at'] is None for row in rows))
                self.assertTrue(all(row['report'] == '{"status":"suspicious","description":"Historical report"}' for row in rows))
                self.assertEqual(connection.execute('SELECT count(*) FROM events').fetchone()[0], 0)

    def test_preflight_rejects_closed_and_expired_events_before_new_file_write(self):
        key = self.seed()
        event, _ = self.rows(key)
        with store.db() as connection, self.assertRaises(self.queue.EventConflict):
            self.queue.ensure_event(connection, 'camera', event['event'], 7)
        key = self.seed((1,))
        event, _ = self.rows(key)
        with store.db() as connection:
            connection.execute('UPDATE events SET deadline=0 WHERE id=?', (key,))
        with store.db() as connection, self.assertRaises(self.queue.EventConflict):
            self.queue.ensure_event(connection, 'camera', event['event'], 7)

    def test_preflight_does_not_mix_historical_frames_with_new_event(self):
        legacy = uuid.uuid4().hex * 2
        with store.db() as connection:
            connection.execute('''INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state)
                VALUES(?,?,?,?,?,?,?,?)''', (legacy, legacy, 'camera', 'historical burst', 1, 0, store.now(), 'review'))
        with store.db() as connection, self.assertRaises(self.queue.EventConflict):
            self.queue.ensure_event(connection, 'camera', 'historical burst', 7)
        with store.db() as connection:
            self.assertEqual(connection.execute('SELECT count(*) FROM events').fetchone()[0], 0)

    def test_legacy_finish_cannot_bypass_whole_event_verdict(self):
        key = self.seed()
        event, frames = self.claim()
        with self.assertRaises(ValueError):
            store.finish(frames[0], 'normal', 'Must assess the whole event')
        self.assertEqual(self.rows(key)[0]['state'], 'processing')
        self.assertEqual({frame['state'] for frame in self.rows(key)[1]}, {'processing'})
        self.assertTrue(all((store.INCOMING / (frame['id'] + '.jpg')).is_file() for frame in frames))

    def test_supplied_connection_expiry_cannot_overwrite_a_changed_event(self):
        key = self.seed((1,))
        with store.db() as connection:
            connection.execute('UPDATE events SET deadline=0 WHERE id=?', (key,))
        with store.db() as connection:
            class ExpirySnapshot:
                def execute(self, sql, parameters=()):
                    cursor = connection.execute(sql, parameters)
                    if sql.startswith('SELECT * FROM events WHERE state='):
                        snapshot = cursor.fetchall()
                        connection.execute("UPDATE events SET state='queued' WHERE id=?", (key,))
                        connection.execute("UPDATE jobs SET state='queued' WHERE event_key=?", (key,))
                        class SnapshotCursor:
                            def fetchall(self):
                                return snapshot
                        return SnapshotCursor()
                    return cursor
            self.assertEqual(self.queue.expire_events(ExpirySnapshot()), 0)
        event, frames = self.rows(key)
        self.assertEqual(event['state'], 'queued')
        self.assertEqual({frame['state'] for frame in frames}, {'queued'})


if __name__ == '__main__':
    try:
        unittest.main()
    finally:
        temporary.cleanup()
