"""Compile the production C++ serializer and validate its exact bytes in Python.

No handwritten metadata fixtures: native input uses production FrameIdentity and
encodeIdentity, and the same synthetic JPEG supplies the dimensions and SHA.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import cv2
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
_data=tempfile.TemporaryDirectory(prefix='firmware-contract-data-')
os.environ['CAMERA_DATA_DIR']=_data.name
sys.path.insert(0,str(ROOT/'server'))
from fastapi import HTTPException
from identity_ingest import validate_metadata


class FirmwareMetadataContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='firmware-contract-build-')
        cls.addClassCleanup(cls.build.cleanup)
        compiler=os.environ.get('CXX') or shutil.which('g++')
        if not compiler:raise AssertionError('A native g++ compiler is required for the firmware contract test')
        cls.executable=Path(cls.build.name)/('identity_json.exe' if os.name=='nt' else 'identity_json')
        camera=ROOT/'firmware/camera'
        command=[compiler,'-std=c++17','-static','-static-libgcc','-static-libstdc++',
                 '-I'+str(camera/'vendor/cjson'),str(camera/'tests/test_identity_json.cpp'),
                 str(camera/'main/project_identity_json.cpp'),str(camera/'vendor/cjson/cJSON.c'),
                 '-o',str(cls.executable)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=90)
        if result.returncode:raise AssertionError('Native fixture build failed:\n'+result.stdout+result.stderr)
        cls.jpeg=cv2.imencode('.jpg',np.full((240,320,3),73,dtype=np.uint8))[1].tobytes()
        cls.sha=hashlib.sha256(cls.jpeg).hexdigest()
        result=subprocess.run([str(cls.executable),cls.sha,'320','240'],capture_output=True,text=True,timeout=15)
        if result.returncode:raise AssertionError('Native fixture execution failed:\n'+result.stdout+result.stderr)
        cls.fixtures={}
        for line in result.stdout.splitlines():
            parsed=json.loads(line)
            status=parsed['frame_identity_status']
            case=('no_faces' if not parsed['faces'] else parsed['faces'][0]['status']) if status=='complete' else status
            if case in cls.fixtures:raise AssertionError('Native generator emitted duplicate case '+case)
            cls.fixtures[case]=line.encode('utf-8')

    def fixture(self,case):
        self.assertTrue(case in self.fixtures,'Native generator must exercise '+case)
        return self.fixtures[case]

    def test_all_production_status_variants_pass_server_validation(self):
        for case in ('matched','unknown','no_faces','timeout','error','not_run'):
            with self.subTest(case=case):
                metadata=validate_metadata(self.fixture(case),self.jpeg)
                self.assertEqual(metadata['device_id'],'contract_camera')
                self.assertEqual(metadata['event_id'],'contract_event')
                self.assertEqual(metadata['expected_frames'],5)
                self.assertEqual((metadata['width'],metadata['height']),(320,240))
                self.assertEqual(metadata['jpeg_sha256'],self.sha)
                self.assertEqual(metadata['photo_index'],1)
                self.assertEqual(metadata['schema_version'],1)
                self.assertIs(metadata['night_capture'],False)
                self.assertIsNone(metadata['captured_at'])

    def test_unknown_and_unfinished_frames_keep_complete_unknown_face_fields(self):
        for case in ('unknown','timeout','error','not_run'):
            with self.subTest(case=case):
                metadata=validate_metadata(self.fixture(case),self.jpeg)
                self.assertEqual(metadata['frame_identity_status'],'complete' if case=='unknown' else case)
                self.assertEqual(len(metadata['faces']),1)
                face=metadata['faces'][0]
                self.assertEqual(set(face),{'face_id','bbox','status','member_id','member_version','match_score'})
                self.assertEqual(face['status'],'unknown')
                self.assertIsNone(face['member_id']);self.assertIsNone(face['member_version'])
                self.assertEqual(face['match_score'],0)
                self.assertEqual(face['bbox'],[10,20,70,90])

    def test_committed_match_and_no_faces_keep_distinct_meanings(self):
        metadata=validate_metadata(self.fixture('matched'),self.jpeg)
        face=metadata['faces'][0]
        self.assertEqual(face['status'],'matched')
        self.assertEqual(face['member_id'],'member_alice')
        self.assertEqual(face['member_version'],2)
        self.assertAlmostEqual(face['match_score'],0.9,places=5)
        self.assertTrue(metadata['recognizer_version']);self.assertTrue(metadata['database_version'])
        empty=validate_metadata(self.fixture('no_faces'),self.jpeg)
        self.assertEqual(empty['frame_identity_status'],'complete')
        self.assertEqual(empty['faces'],[])

    def test_production_metadata_remains_bound_to_its_exact_jpeg(self):
        different=cv2.imencode('.jpg',np.full((240,320,3),74,dtype=np.uint8))[1].tobytes()
        with self.assertRaises(HTTPException) as caught:
            validate_metadata(self.fixture('matched'),different)
        self.assertEqual(caught.exception.status_code,422)
        self.assertIn('digest',caught.exception.detail)

    def test_server_rejects_corrupted_bounds_dimensions_or_frame_count(self):
        for change in ('bbox','width','expected_frames'):
            with self.subTest(change=change):
                metadata=json.loads(self.fixture('matched'))
                if change=='bbox':metadata['faces'][0]['bbox']=[10,20,321,90]
                elif change=='width':metadata['width']=321
                else:metadata['expected_frames']=4
                with self.assertRaises(HTTPException) as caught:
                    validate_metadata(json.dumps(metadata).encode(),self.jpeg)
                self.assertEqual(caught.exception.status_code,422)


if __name__=='__main__':unittest.main(verbosity=2)
