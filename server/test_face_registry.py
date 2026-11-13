"""Real SQLite registry state-machine tests; never touches live camera data."""
import importlib.util
import sqlite3
import unittest


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('face_registry'), 'Registry implementation is missing')
        import face_registry
        self.r = face_registry
        self.c = sqlite3.connect(':memory:')
        self.c.row_factory = sqlite3.Row
        self.c.execute('CREATE TABLE devices(device TEXT PRIMARY KEY,owner_id INTEGER,token_digest TEXT)')
        self.c.executemany('INSERT INTO devices VALUES(?,?,?)', [('a',1,'x'),('b',1,'x'),('other',2,'x')])
        self.r.schema(self.c)
        self.member = self.r.create_member(self.c,1,'Family',now=100)

    def tearDown(self):
        if hasattr(self,'c'): self.c.close()

    def enroll(self,device='a',request='request-1',now=100):
        return self.r.create_enrollment(self.c,1,self.member['id'],device,request,now=now)

    def ack(self,command,**kwargs):
        return self.r.ack_command(self.c,1,command['device_id'],command['id'],command['member_version'],
                                  kwargs.pop('result','committed'),now=kwargs.pop('now',101),
                                  database_version=kwargs.pop('database_version','db-1'),
                                  recognizer_version=kwargs.pop('recognizer_version','model-1'),**kwargs)

    def active(self,device='a',version=1,model='model-1',database='db-1',owner=1):
        return self.r.is_registration_active(self.c,owner,device,self.member['id'],version,model,database)

    def test_commit_requires_both_persisted_versions_and_is_per_device(self):
        first=self.enroll(); self.enroll('b','request-b')
        self.assertFalse(self.active())
        for field in ['database_version','recognizer_version']:
            with self.assertRaises(self.r.RegistryError): self.ack(first,**{field:None})
        self.ack(first,result='sampling',database_version=None,recognizer_version=None)
        self.assertFalse(self.active())
        self.ack(first)
        self.assertTrue(self.active()); self.assertFalse(self.active('b'))
        self.assertFalse(self.active(model='wrong')); self.assertFalse(self.active(database='wrong'))
        self.assertFalse(self.active(owner=2))

    def test_request_id_replay_never_extends_deadline_or_changes_target(self):
        first=self.enroll()
        self.assertEqual(self.enroll(now=110)['id'],first['id'])
        self.assertEqual(self.enroll(now=110)['expires_at'],220)
        with self.assertRaises(self.r.RegistryError): self.enroll('b')
        another=self.r.create_member(self.c,1,'Second',now=100)
        with self.assertRaises(self.r.RegistryError):
            self.r.create_enrollment(self.c,1,another['id'],'a','different',now=110)

    def test_expiry_boundary_and_old_ack_cannot_activate(self):
        command=self.enroll()
        result=self.ack(command,now=220)
        self.assertEqual(result['state'],'failed'); self.assertFalse(self.active())
        newer=self.enroll(request='new',now=221)
        self.assertEqual(newer['member_version'],2)
        with self.assertRaises(self.r.RegistryError): self.ack(command,now=222)
        self.assertFalse(self.active(version=2))

    def test_revocation_and_tombstone_are_immediate_and_old_ack_cannot_restore(self):
        command=self.enroll(); self.ack(command)
        self.r.revoke_member(self.c,1,self.member['id'],now=102)
        self.assertFalse(self.active())
        with self.assertRaises(self.r.RegistryError): self.ack(command,now=103)
        revoke=self.r.commands_for_device(self.c,1,'a',now=103)[0]
        self.assertEqual(revoke['operation'],'revoke')
        self.assertEqual(self.ack(revoke,now=104)['state'],'revoked')
        self.r.update_member(self.c,1,self.member['id'],enabled=True,now=105)
        self.assertFalse(self.active())
        new=self.enroll(request='new',now=106); self.ack(new,now=107,database_version='db-2')
        self.assertTrue(self.active(version=3,database='db-2'))
        self.r.revoke_member(self.c,1,self.member['id'],now=108,delete=True)
        self.assertFalse(self.active(version=3,database='db-2'))
        row=self.c.execute('SELECT * FROM family_members WHERE id=?',(self.member['id'],)).fetchone()
        self.assertEqual(row['deleted_at'],108)
        with self.assertRaises(self.r.RegistryError): self.enroll(request='deleted',now=109)

    def test_owner_device_and_member_cannot_be_crossed(self):
        for owner,device in [(2,'other'),(1,'other'),(1,'unregistered')]:
            with self.assertRaises(self.r.RegistryError):
                self.r.create_enrollment(self.c,owner,self.member['id'],device,'bad',now=100)
        first=self.enroll()
        with self.assertRaises(self.r.RegistryError):
            self.r.ack_command(self.c,1,'b',first['id'],1,'committed','db-1','model-1',now=101)
        self.ack(first)
        self.c.execute("UPDATE devices SET owner_id=2 WHERE device='a'")
        self.assertFalse(self.active())

    def test_reset_or_incompatible_model_revokes_and_requires_new_enrollment(self):
        command=self.enroll(); self.ack(command)
        self.r.report_device_state(self.c,1,'a','model-1','empty',reset=True,now=103)
        self.assertFalse(self.active())
        with self.assertRaises(self.r.RegistryError): self.ack(command,now=104)
        new=self.enroll(request='new',now=105); self.ack(new,now=106,database_version='db-2')
        self.r.report_device_state(self.c,1,'a','model-2','db-2',compatible=False,now=107)
        self.assertFalse(self.active(version=new['member_version'],database='db-2'))

    def test_ack_is_idempotent_but_changed_ack_rejected(self):
        command=self.enroll(); first=self.ack(command)
        self.assertEqual(self.ack(command,now=300),first)
        with self.assertRaises(self.r.RegistryError): self.ack(command,database_version='different')

    def test_migration_preserves_existing_data_and_name_changes_preserve_registration(self):
        self.r.schema(self.c)
        command=self.enroll(); self.ack(command)
        self.r.update_member(self.c,1,self.member['id'],display_name='<img src=x onerror=alert(1)>',now=103)
        self.assertTrue(self.active())
        self.assertEqual(self.c.execute('SELECT COUNT(*) FROM devices').fetchone()[0],3)

    def test_new_commit_updates_library_version_for_existing_members(self):
        self.ack(self.enroll())
        second=self.r.create_member(self.c,1,'Second',now=102)
        cmd=self.r.create_enrollment(self.c,1,second['id'],'a','second',now=102)
        self.ack(cmd,database_version='db-2',now=103)
        self.assertTrue(self.active(database='db-2'))
        self.assertFalse(self.active(database='db-1'))

    def test_revoke_commit_advances_library_for_other_members_and_failed_revoke_never_activates(self):
        self.ack(self.enroll())
        second=self.r.create_member(self.c,1,'Second',now=102)
        command=self.r.create_enrollment(self.c,1,second['id'],'a','second',now=102)
        self.ack(command,database_version='db-2',now=103)
        self.r.revoke_member(self.c,1,second['id'],now=104)
        revoke=self.r.commands_for_device(self.c,1,'a',now=105)[0]
        self.assertEqual(revoke['operation'],'revoke')
        self.assertEqual(self.ack(revoke,database_version='db-3',now=106)['state'],'revoked')
        self.assertTrue(self.active(database='db-3'))
        self.assertFalse(self.active(database='db-2'))
        self.r.revoke_member(self.c,1,self.member['id'],now=107)
        revoke=self.r.commands_for_device(self.c,1,'a',now=108)[0]
        self.assertEqual(self.ack(revoke,result='failed',error='Flash error',now=109)['state'],'revoked')
        self.assertFalse(self.active(version=revoke['member_version'],database='db-3'))

    def test_credential_rotation_even_back_to_same_owner_never_restores_registration(self):
        command=self.enroll(); self.ack(command)
        self.c.execute("UPDATE devices SET owner_id=2,token_digest='new' WHERE device='a'")
        self.assertFalse(self.active())
        self.c.execute("UPDATE devices SET owner_id=1 WHERE device='a'")
        self.assertFalse(self.active())
        with self.assertRaises(self.r.RegistryError):self.ack(command)

    def test_expired_delete_cleanup_is_redelivered_without_extending_enrollment(self):
        command=self.enroll(); self.ack(command)
        self.r.revoke_member(self.c,1,self.member['id'],now=102,delete=True)
        original=self.r.commands_for_device(self.c,1,'a',now=103)[0]
        self.r.expire_commands(self.c,now=500)
        commands=self.r.commands_for_device(self.c,1,'a',now=500)
        self.assertEqual(len(commands),1,'Expired current cleanup must be delivered after offline recovery')
        renewed=commands[0]
        self.assertEqual(renewed['id'],original['id'])
        self.assertEqual(renewed['member_version'],2)
        self.assertEqual(renewed['expires_at'],620)
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=501)[0]['expires_at'],620)
        self.assertEqual(self.ack(renewed,now=502,database_version='db-clean')['state'],'revoked')
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=800),[])
        other=self.r.create_member(self.c,1,'Other',now=900)
        enrollment=self.r.create_enrollment(self.c,1,other['id'],'a','other',now=900)
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=1020),[])
        self.assertEqual(self.c.execute('SELECT expires_at FROM device_commands WHERE id=?',(enrollment['id'],)).fetchone()[0],1020)

    def test_failed_revoke_gets_new_attempt_id_same_target_and_keeps_old_ack(self):
        self.ack(self.enroll())
        self.r.revoke_member(self.c,1,self.member['id'],now=102)
        original=self.r.commands_for_device(self.c,1,'a',now=103)[0]
        self.ack(original,result='failed',error='Flash busy',now=104)
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=221),[])
        commands=self.r.commands_for_device(self.c,1,'a',now=222)
        self.assertEqual(len(commands),1,'Failed current cleanup needs a fresh retry attempt')
        retried=commands[0]
        self.assertNotEqual(retried['id'],original['id'])
        self.assertEqual(retried['member_version'],2)
        self.assertEqual(retried['expires_at'],342)
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=223)[0]['id'],retried['id'])
        old=self.c.execute('SELECT status,ack_payload FROM device_commands WHERE id=?',(original['id'],)).fetchone()
        self.assertEqual(old['status'],'failed'); self.assertIsNotNone(old['ack_payload'])
        with self.assertRaises(self.r.RegistryError):self.ack(original,now=224)
        self.assertEqual(self.ack(retried,now=224,database_version='db-clean')['state'],'revoked')
        self.assertFalse(self.active(version=2,database='db-clean'))

    def test_reenrollment_retires_cleanup_before_its_deadline_and_never_renews_it(self):
        self.ack(self.enroll())
        self.r.revoke_member(self.c,1,self.member['id'],now=102)
        cleanup=self.r.commands_for_device(self.c,1,'a',now=103)[0]
        self.r.update_member(self.c,1,self.member['id'],enabled=True,now=104)
        replacement=self.enroll(request='replacement',now=105)
        commands=self.r.commands_for_device(self.c,1,'a',now=106)
        self.assertEqual([command['id'] for command in commands],[replacement['id']])
        self.ack(replacement,now=107,database_version='db-new')
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=500),[])
        self.assertEqual(self.c.execute('SELECT expires_at FROM device_commands WHERE id=?',(cleanup['id'],)).fetchone()[0],222)
        self.assertTrue(self.active(version=3,database='db-new'))

    def test_cleanup_not_renewed_for_changed_owner_or_credentials(self):
        self.ack(self.enroll())
        self.r.revoke_member(self.c,1,self.member['id'],now=102)
        cleanup=self.r.commands_for_device(self.c,1,'a',now=103)[0]
        self.c.execute("UPDATE devices SET owner_id=2 WHERE device='a'")
        self.assertEqual(self.r.commands_for_device(self.c,2,'a',now=500),[])
        self.c.execute("UPDATE devices SET owner_id=1,token_digest='rotated' WHERE device='a'")
        self.assertEqual(self.r.commands_for_device(self.c,1,'a',now=501),[])
        self.assertEqual(self.c.execute('SELECT expires_at FROM device_commands WHERE id=?',(cleanup['id'],)).fetchone()[0],222)


if __name__=='__main__': unittest.main(verbosity=2)
