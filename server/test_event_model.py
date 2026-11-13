"""No GPU: exercise actual multi-image message builder and result validation."""
import json
import os
import tempfile
import unittest
from pathlib import Path

temporary=tempfile.TemporaryDirectory(prefix='event-model-tests-')
os.environ['CAMERA_DATA_DIR']=temporary.name
import worker

class EventModelTests(unittest.TestCase):
    def model(self, answer):
        self.assertTrue(callable(getattr(worker.Models,'assess_event',None)), 'Event inference is missing')
        model=worker.Models.__new__(worker.Models)
        self.messages=[]
        def generate(messages):
            self.messages.append(messages)
            return json.dumps(answer)
        model._generate=generate
        return model

    def test_all_images_and_order_sent_in_one_model_request(self):
        model=self.model({'verdict':'suspicious','description':'Visible action sequence.',
                          'activity_type':'violence','evidence_frame_indices':[2,4]})
        paths=[Path(temporary.name)/(str(i)+'.jpg') for i in range(1,6)]
        frames=[{'photo_index':i,'captured_at':None} for i in range(1,6)]
        result=model.assess_event(paths,frames)
        self.assertEqual(result,('suspicious','Visible action sequence.','violence',[2,4]))
        self.assertEqual(len(self.messages),1)
        content=self.messages[0][1]['content']
        images=[x for x in content if x['type']=='image']
        self.assertEqual([x['image'] for x in images],[p.resolve().as_uri() for p in paths])
        labels=[x['text'] for x in content if x['type']=='text']
        for i in range(1,6):self.assertTrue(any('Frame '+str(i) in t for t in labels))
        self.assertTrue(all(x['max_pixels']>0 for x in images))

    def test_evidence_is_available_integer_index_only(self):
        for evidence in ([6],[True],[-1],[1,1],'2',None):
            with self.subTest(evidence=evidence):
                model=self.model({'verdict':'suspicious','description':'Action','evidence_frame_indices':evidence})
                with self.assertRaises(ValueError):
                    model.assess_event([Path('one.jpg')],[{'photo_index':1,'captured_at':None}])

    def test_missing_evidence_is_compatible_and_capture_time_preserved(self):
        model=self.model({'verdict':'normal','description':'Walking.'})
        result=model.assess_event([Path('one.jpg')],[{'photo_index':1,'captured_at':'2026-11-09T10:00:00+00:00'}])
        self.assertEqual(result,('normal','Walking.',None,[]))
        self.assertIn('2026-11-09T10:00:00+00:00',json.dumps(self.messages))

if __name__=='__main__':
    try:unittest.main()
    finally:temporary.cleanup()
