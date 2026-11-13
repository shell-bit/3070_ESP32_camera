import os
import uuid
import unittest
from pathlib import Path

TEST_DATA=Path(__file__).resolve().parent.parent/('manual-review-test-'+uuid.uuid4().hex)
os.environ['CAMERA_DATA_DIR']=str(TEST_DATA)
import store
from manual_review import decide,ReviewConflict

class ManualReviewTests(unittest.TestCase):
    def seed(self,state='review',target=False):
        key=uuid.uuid4().hex*2
        with store.db() as connection:
            connection.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,error) VALUES(?,?,?,?,?,?,?,?,?)',
                (key,'digest','camera','event',1,0,store.now(),state,'Qwen invalid JSON'))
        ((store.PHOTOS if target else store.INCOMING)/(key+'.jpg')).write_bytes(b'test fixture')
        return key
    def row(self,key):
        with store.db() as connection:
            return dict(connection.execute('SELECT * FROM jobs WHERE id=?',(key,)).fetchone())
    def test_keep_and_conflicting_decision(self):
        key=self.seed();decide(key,'keep','Visible damage to door')
        row=self.row(key);self.assertEqual(row['state'],'suspicious')
        report=store.report_for(row)
        self.assertEqual(report['review_source'],'human')
        self.assertEqual(report['description'],'Visible damage to door')
        self.assertTrue((store.PHOTOS/(key+'.jpg')).is_file())
        self.assertFalse((store.INCOMING/(key+'.jpg')).exists())
        with self.assertRaises(ReviewConflict):decide(key,'dismiss','second reviewer')
        self.assertTrue((store.PHOTOS/(key+'.jpg')).is_file())
    def test_dismiss_and_audit(self):
        key=self.seed();decide(key,'dismiss','Normal activity')
        self.assertEqual(self.row(key)['state'],'discarded')
        self.assertFalse((store.INCOMING/(key+'.jpg')).exists())
        with store.db() as connection:
            audit=connection.execute('SELECT * FROM manual_decisions WHERE job_id=?',(key,)).fetchone()
            self.assertEqual(audit['original_error'],'Qwen invalid JSON')
    def test_retry_preserves_image(self):
        key=self.seed();decide(key,'retry','Try again')
        self.assertEqual(self.row(key)['state'],'queued')
        self.assertTrue((store.INCOMING/(key+'.jpg')).exists())
    def test_processing_not_mutable(self):
        key=self.seed('processing')
        with self.assertRaises(ReviewConflict):decide(key,'dismiss','no')
        self.assertTrue((store.INCOMING/(key+'.jpg')).exists())
    def test_missing_and_recovered_target(self):
        key=self.seed();(store.INCOMING/(key+'.jpg')).unlink()
        with self.assertRaises(FileNotFoundError):decide(key,'keep','test')
        self.assertEqual(self.row(key)['state'],'review')
        key=self.seed(target=True);decide(key,'keep','Recovered photo')
        self.assertEqual(self.row(key)['state'],'suspicious')

if __name__=='__main__':unittest.main()
