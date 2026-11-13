import hashlib
import json
import os
import tempfile
import unittest
temporary=tempfile.TemporaryDirectory(prefix='cpu-review-')
os.environ['CAMERA_DATA_DIR']=temporary.name
import store
import worker
from event_queue import ensure_event,register_frame


class CpuReviewTests(unittest.TestCase):
    def test_cpu_mode_preserves_media_and_reports_unavailable(self):
        with store.db() as c:
            event=ensure_event(c,'camera','cpu-event',None,1)
            c.execute('INSERT INTO jobs(id,digest,device,event,photo_index,night,received,state,event_key) VALUES(?,?,?,?,?,?,?,?,?)',
                ('a'*64,'sha','camera','cpu-event',1,0,store.now(),'collecting',event['id']))
            register_frame(c,event['id'])
        path=store.INCOMING/('a'*64+'.jpg');path.write_bytes(b'preserved media')
        self.assertTrue(hasattr(worker,'process_review_one'),'CPU review mode is not implemented')
        self.assertTrue(worker.process_review_one())
        self.assertEqual(path.read_bytes(),b'preserved media')
        with store.db() as c:
            row=c.execute('SELECT * FROM events WHERE id=?',(event['id'],)).fetchone()
            self.assertEqual(row['state'],'review')
            self.assertIn('unavailable',row['error'].lower())
            report=json.loads(row['report'])
            self.assertEqual(report['alarm_decision']['decision'],'review')
            self.assertEqual(report['alarm_decision']['analysis_version'],1)
            self.assertEqual(c.execute('SELECT count(*) FROM push_events').fetchone()[0],0)


if __name__=='__main__':unittest.main()
