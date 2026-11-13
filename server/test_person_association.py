import unittest


class AssociationTests(unittest.TestCase):
    def associate(self, detections, identities):
        from person_association import associate_people
        frames=[{'photo_index':i} for i in sorted(detections)]
        return associate_people(frames,detections,identities)

    def identity(self, faces):
        return {'faces':faces,'authorized_faces':[f['face_id'] for f in faces],
                'frame_identity_status':'complete','recognizer_version':'r1','database_version':'d1'}

    def face(self, bbox, member='mom'):
        return {'face_id':'f1','bbox':bbox,'status':'matched','member_id':member,'member_version':1}

    def test_family_does_not_spread_to_other_people_or_missing_observations(self):
        detections={1:[[0,0,40,100],[60,0,100,100]],2:[[1,0,41,100],[60,0,100,100]]}
        result=self.associate(detections,{1:self.identity([self.face([10,5,20,20])]),2:self.identity([])})
        tracks=result['tracks']; self.assertEqual(len(tracks),2)
        family=[t for t in tracks if t['observations'][0]['identity'] is not None][0]
        self.assertIsNone(family['observations'][1]['identity'])
        self.assertEqual(sum(o['identity'] is not None for t in tracks for o in t['observations']),1)

    def test_ambiguous_overlapping_people_do_not_get_identity(self):
        result=self.associate({1:[[0,0,40,100],[5,0,45,100]]},
                              {1:self.identity([self.face([10,5,20,20])])})
        self.assertTrue(all(o['identity'] is None for t in result['tracks'] for o in t['observations']))

    def test_conflicting_faces_clear_identity(self):
        a=self.face([5,5,15,15]); b=dict(self.face([20,5,30,15],'dad'),face_id='f2')
        result=self.associate({1:[[0,0,40,100]]},{1:self.identity([a,b])})
        self.assertIsNone(result['tracks'][0]['observations'][0]['identity'])

    def test_gap_never_propagates_track(self):
        result=self.associate({1:[[0,0,40,100]],3:[[0,0,40,100]]},{})
        self.assertEqual(len(result['tracks']),2)

    def test_bad_actor_references_rejected(self):
        from person_association import validate_actor_assessment
        tracks=self.associate({1:[[0,0,40,100]]},{})
        value={'coverage':'complete','actions':[{'verdict':'suspicious','actor_track_id':'missing',
            'target_track_ids':[],'evidence_frame_indices':[1]}]}
        with self.assertRaises(ValueError):validate_actor_assessment(value,tracks,[1])


if __name__=='__main__':unittest.main()
