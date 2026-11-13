import copy
import unittest


class AlarmPolicyTests(unittest.TestCase):
    def setUp(self):
        self.tracks={'tracks':[{'track_id':'p1','observations':[{'frame_index':1,'bbox':[0,0,40,100],
            'identity':{'member_id':'mom','member_version':1,'recognizer_version':'r1','database_version':'d1'}}]},
            {'track_id':'p2','observations':[{'frame_index':1,'bbox':[60,0,100,100],'identity':None}]}]}
        self.assessment={'coverage':'complete','actions':[{'verdict':'suspicious','actor_track_id':'p1',
            'target_track_ids':[],'evidence_frame_indices':[1]}]}

    def evaluate(self, verdict='suspicious', assessment=None, enabled=True, registered=True):
        from alarm_policy import evaluate_alarm
        return evaluate_alarm(verdict,self.assessment if assessment is None else assessment,self.tracks,
            lambda identity:registered,enabled=enabled,evidence_frame_indices=[1])['decision']

    def test_confirmed_family_actor_only_is_exempted(self):
        self.assertEqual(self.evaluate(),'exempted')
    def test_unknown_actor_with_family_target_alerts(self):
        self.assessment['actions'][0].update(actor_track_id='p2',target_track_ids=['p1'])
        self.assertEqual(self.evaluate(),'alert')
    def test_mixed_actions_alert(self):
        self.assessment['actions'].append(dict(self.assessment['actions'][0],actor_track_id='p2'))
        self.assertEqual(self.evaluate(),'alert')
    def test_uncertain_does_not_become_normal_or_exempt(self):
        self.assertEqual(self.evaluate('uncertain'),'review')
    def test_disabled_revoked_or_missing_identity_keeps_alert(self):
        self.assertEqual(self.evaluate(enabled=False),'alert')
        self.assertEqual(self.evaluate(registered=False),'alert')
        self.tracks['tracks'][0]['observations'][0]['identity']=None
        self.assertEqual(self.evaluate(),'alert')
    def test_incomplete_malformed_or_missing_actions_keep_alert(self):
        for value in ({}, {'coverage':'partial','actions':self.assessment['actions']},
                      {'coverage':'complete','actions':[]},
                      {'coverage':'complete','actions':[dict(self.assessment['actions'][0],actor_track_id='p99')]}):
            self.assertEqual(self.evaluate(assessment=value),'alert')
    def test_normal_with_conflicting_action_is_review(self):
        self.assertEqual(self.evaluate('normal'),'review')
    def test_normal_with_explicit_unknown_suspicious_actor_keeps_alert(self):
        self.assessment['actions'][0]['actor_track_id']='p2'
        self.assertEqual(self.evaluate('normal'),'alert')
    def test_evidence_frame_without_current_identity_not_exempt(self):
        self.tracks['tracks'][0]['observations'].append({'frame_index':2,'bbox':[0,0,40,100],'identity':None})
        self.assessment['actions'][0]['evidence_frame_indices']=[1,2]
        self.assertEqual(self.evaluate(),'alert')


if __name__=='__main__':unittest.main()
