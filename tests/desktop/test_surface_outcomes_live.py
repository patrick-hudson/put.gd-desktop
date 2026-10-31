"""Synthetic safety/oracle checks; these do not establish desktop acceptance."""
import io
import base64
import contextlib
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
import tempfile
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.fixtures.base import Fixture
from tools.lab.surface_driver.fixtures.live_outcomes import key_set_two, admit, verify_s3, reconcile_refused_s3, preflight_s3
from tools.lab.surface_driver.run import discover, validate_recipes
from tools.lab.surface_driver.recipes import outcomes_live
from tools.lab.surface_driver import browser


class LiveOutcomes(unittest.TestCase):
    def test_download_list_drops_source_and_error_messages_and_restores_context(self):
        raw={'available':True,'entries':[{'target':'/tmp/capture.png','sourceUrl':'https://invalid.example/private-token',
             'error':{'result':123,'message':'https://invalid.example/private-token','becauseTargetFailed':True}}]}
        client=Mock();client.script.return_value=raw
        value=browser._firefox_downloads(client,'/dev/shm/test-downloads','capture.png')
        self.assertNotIn('private-token',repr(value))
        self.assertEqual(value['entries'][0]['error']['result'],123)
        self.assertTrue(value['entries'][0]['error']['becauseTargetFailed'])
        self.assertEqual(client.call.call_args_list[-1].args,('Marionette:SetContext',{'value':'content'}))
        client.script.side_effect=RuntimeError('synthetic API failure')
        with self.assertRaises(RuntimeError):browser._firefox_downloads(client,'/dev/shm/test-downloads','capture.png')
        self.assertEqual(client.call.call_args_list[-1].args,('Marionette:SetContext',{'value':'content'}))

    def test_completed_download_hash_uses_browser_target_and_rejects_wrong_bytes(self):
        with tempfile.TemporaryDirectory(dir='/dev/shm') as td:
            p=Path(td)/'capture.png';p.write_bytes(b'synthetic original')
            record={'entries':[{'succeeded':True,'stopped':True,'error':None,'target':str(p)}]}
            self.assertTrue(browser._completed_recipient_file(record,b'synthetic original','capture.png')['matches'])
            self.assertFalse(browser._completed_recipient_file(record,b'other original','capture.png')['matches'])
            self.assertIsNone(browser._completed_recipient_file(record,b'synthetic original','other.png'))

    def test_preview_compares_decoded_pixels_with_zero_tolerance(self):
        try:from PyQt6.QtGui import QImage
        except ImportError:self.skipTest('real Qt PNG decoder required on the builder')
        from PIL import Image
        stream=io.BytesIO();Image.new('RGBA',(2,1),(10,20,30,255)).save(stream,format='PNG')
        rgba=bytes((10,20,30,255))*2
        client=Mock()
        def result():return {'width':2,'height':1,'naturalWidth':2,'naturalHeight':1,'rgba':base64.b64encode(rgba).decode()}
        client.script.side_effect=lambda *a,**k:result()
        self.assertTrue(browser._recipient_preview_pixels(client,stream.getvalue())['matches'])
        rgba=bytes((11,20,30,255))*2
        proof=browser._recipient_preview_pixels(client,stream.getvalue())
        self.assertFalse(proof['matches']);self.assertEqual(proof['tolerance'],0)

    def test_scoped_missing_head_requires_404_and_rejects_root_prefix(self):
        class ServiceError(Exception):
            def __init__(self,status):self.response={'ResponseMetadata':{'HTTPStatusCode':status}}
        client=Mock();client.exceptions.ClientError=ServiceError
        ctx=SimpleNamespace(note=Mock());prefix='acceptance/tier1-live/'+'a'*32+'/'
        with self.assertRaises(RecipeBlocked):preflight_s3(ctx,client,'put.gd','putgd-tier1-unscoped/')
        client.head_object.assert_not_called()
        client.head_object.side_effect=ServiceError(404)
        preflight_s3(ctx,client,'put.gd',prefix)
        client.head_object.assert_called_once_with(Bucket='put.gd',Key=prefix+'missing-preflight.png')
        ctx.note.assert_called_once_with('s3_missing_head_404_count=1')
        client.head_object.side_effect=ServiceError(403)
        with self.assertRaises(RecipeBlocked):preflight_s3(ctx,client,'put.gd',prefix)

    def test_cloudfront_link_is_bound_to_the_exact_authenticated_key(self):
        def expect(ok,note):
            if not ok:raise RecipeFailed(note)
        client=Mock();fx=Fixture(client=client,bucket='put.gd',prefix='acceptance/tier1-live/owned/',region='us-east-1')
        data=b'synthetic png';key=fx.prefix+'capture.png'
        detail={'receipt':{'objectReference':{'key':key},'destination':{'settings':{'bucket':'put.gd'}},
                           'directUrl':'https://put.gd/'+key}}
        client.head_object.return_value={'ContentLength':len(data)}
        client.get_object.return_value={'Body':io.BytesIO(data)}
        verify_s3(SimpleNamespace(expect=expect),fx,detail,data)
        detail['receipt']['directUrl']='https://s3.us-east-1.amazonaws.com/put.gd/'+key
        with self.assertRaises(RecipeFailed):verify_s3(SimpleNamespace(expect=expect),fx,detail,data)
        detail['receipt']['directUrl']='https://put.gd/foreign/capture.png'
        with self.assertRaises(RecipeFailed):verify_s3(SimpleNamespace(expect=expect),fx,detail,data)

    def test_key_two_only_and_explicit_shared_bucket(self):
        pair='AKIA'+'A'*16+' '+'b'*40+'\n'
        first='Set 1\nAccess key ID: first-synthetic\nSecret access key: first-secret\nBucket: synthetic-bucket\nEndpoint / Region:\nAWS Region ID: us-east-2\nS3 Endpoint: https://s3.us-east-2.amazonaws.com\n\n'
        other='Backblaze B2\napplicationKey: unrelated\nCloudflare R2\nAccess key ID: r2-synthetic\nSecret access key: r2-secret\nRegion: auto\n'
        parsed=key_set_two(first+pair+other)
        self.assertEqual(parsed['credentials'],dict(accessKey='AKIA'+'A'*16,secretKey='b'*40,sessionToken=''))
        self.assertEqual(parsed['settings'],{'bucket':'synthetic-bucket','region':'us-east-2'})
        for text in (first+other, first+pair+pair+other, first+other+pair,
                     first+pair+pair.replace('b'*40,'b'*39)+other, first+pair.replace('b'*40,'b'*39)+other,
                     first.replace('AWS Region ID: us-east-2\n','')+pair+other, 'unlabelled text'):
            with self.assertRaises(RecipeBlocked):key_set_two(text)

    def test_live_rejects_boat_native_wrong_desktop_before_secret_read(self):
        for placement,host,desktop in [('boat','boat','mint-cinnamon-x11'),('native','f44','kubuntu-kde-wayland'),
                                        ('guest','d69','kubuntu-kde-wayland')]:
            ctx=SimpleNamespace(destination='s3',placement=placement,host=host,desktop=desktop,
                                target=SimpleNamespace(physical=None),profile={'guest':'unit'},_live_admitted=True)
            with self.assertRaises(RecipeBlocked):admit(ctx)

    def test_s3_oracle_rejects_foreign_key_without_network_and_closes_mismatch(self):
        def expect(ok,note):
            if not ok:raise RecipeFailed(note)
        ctx=SimpleNamespace(expect=expect)
        client=Mock();fx=Fixture(client=client,bucket='put.gd',prefix='owned/',region='us-east-1')
        detail={'receipt':{'objectReference':{'key':'foreign/object'},'destination':{'settings':{'bucket':'put.gd'}},
                           'directUrl':'https://put.gd/owned/object'}}
        with self.assertRaises(RecipeFailed):verify_s3(ctx,fx,detail,b'expected')
        client.get_object.assert_not_called();client.head_object.assert_not_called()
        detail['receipt']['objectReference']['key']='owned/object'
        client.head_object.return_value={'ContentLength':8}
        body=io.BytesIO(b'tampered');client.get_object.return_value={'Body':body}
        with self.assertRaises(RecipeFailed):verify_s3(ctx,fx,detail,b'expected')
        self.assertTrue(body.closed)

    def test_destinations_claim_only_supported_cleanup_interactions(self):
        recipes=discover('outcomes_live');validate_recipes(recipes)
        self.assertEqual(len(recipes),2)
        for r in recipes:
            self.assertEqual(len(r.destinations),1)
            self.assertIn('region.to-upload',r.interactions)
            self.assertIn('confirm.history-delete' if r.destinations==('s3',) else 'confirm.revoke-link',r.interactions)

    def test_refused_put_cannot_clear_pending_without_owned_key_and_get_404(self):
        def expect(ok,note):
            if not ok:raise RecipeFailed(note)
        class Missing(Exception):pass
        client=Mock();client.exceptions.NoSuchKey=Missing
        fx=Fixture(client=client,bucket='put.gd',prefix='owned/',pending={'remote':True})
        ctx=SimpleNamespace(expect=expect,note=Mock())
        item=dict(error={'code':'s3_permission_denied','details':{'httpStatus':403,'requestStage':'upload_put'}},
                  candidate='capture.png',objectReference={'key':'foreign/capture.png'})
        with self.assertRaises(RecipeFailed):reconcile_refused_s3(ctx,fx,{'items':[item]},{'uploadStatus':'failed'})
        client.get_object.assert_not_called();self.assertTrue(fx.pending['remote'])
        item['objectReference']['key']='owned/capture.png';client.get_object.side_effect=Missing()
        self.assertTrue(reconcile_refused_s3(ctx,fx,{'items':[item]},{'uploadStatus':'failed'}))
        self.assertFalse(fx.pending['remote']);ctx.note.assert_called_once_with('failed_upload_absence_count=1')

    def test_proton_revoke_uses_message_box_geometry_and_restores_profile(self):
        previous={'confirmAction':{'accessible':{'role':'dialog'}}}
        ctx=SimpleNamespace(destination='proton',profile={'windows':previous},click=Mock(),focus=Mock(),scroll=Mock(),
                            geometry=Mock(return_value=dict(x=0,y=0,width=1280,height=760)),frame=Mock(),note=Mock(),
                            wait=lambda fn,*a,**kw:fn(),call=lambda *a,**kw:{'shareStatus':'revoked','linkAvailable':False},
                            expect=lambda ok,note:self.assertTrue(ok,note))
        def observed(*args):
            spec=ctx.profile['windows']['confirmAction']
            self.assertEqual(spec['accessible'],{'role':'alert'})
            self.assertEqual(spec['title'],'^Revoke share link$')
        with patch.object(outcomes_live.outcomes,'_history'),patch.object(outcomes_live.history,'_wait_window',side_effect=observed),patch.object(outcomes_live.history,'_confirm_button'):
            outcomes_live._cleanup(ctx,{'id':'owned-history'})
        self.assertIs(ctx.profile['windows'],previous)
        ctx.click.assert_any_call('text:Revoke link…',window='history')
        ctx.note.assert_called_once_with('revocation_count=1')

    def test_cleanup_ownership_rejects_foreign_receipts_before_any_input(self):
        def expect(ok,note):
            if not ok:raise RecipeFailed(note)
        ctx=SimpleNamespace(destination='s3',expect=expect)
        service=Fixture(profile_id='owned-profile',credential_reference='owned-reference',prefix='owned/')
        detail={'operationId':'owned-operation','receipt':{'destination':{'profileId':'owned-profile',
          'providerId':'s3','credentials':{'reference':'owned-reference'},'settings':{'objectPrefix':'owned/'}},
          'objectReference':{'key':'foreign/object'}}}
        with self.assertRaises(RecipeFailed):outcomes_live._owned(ctx,service,detail,'owned-operation')
        detail['receipt']['objectReference']['key']='owned/object'
        outcomes_live._owned(ctx,service,detail,'owned-operation')
        with self.assertRaises(RecipeFailed):outcomes_live._owned(ctx,service,detail,'different-operation')

    def test_s3_caption_binding_still_refuses_foreign_prepared_target(self):
        def expect(ok,note):
            if not ok:raise RecipeFailed(note)
        ctx=SimpleNamespace(destination='s3',profile={},click=Mock(),focus=Mock(),scroll=Mock(),
                            geometry=Mock(return_value=dict(x=0,y=0,width=1280,height=760)),frame=Mock(),
                            call=Mock(),clipboard_text=lambda:json.dumps({'targets':[{'historyId':'foreign'}],
                                                                         'acknowledgement':'synthetic'}),expect=expect)
        with patch.object(outcomes_live.outcomes,'_history'),patch.object(outcomes_live.history,'_wait_window'),patch.object(outcomes_live.history,'_confirm_button') as confirm:
            with self.assertRaises(RecipeFailed):outcomes_live._cleanup(ctx,{'id':'owned'})
        ctx.click.assert_called_once_with('text:Delete from destination…',window='history')
        confirm.assert_not_called()

    def test_saved_hash_cannot_read_a_foreign_file_or_symlink(self):
        def execute(code):
            output=io.StringIO()
            with contextlib.redirect_stdout(output):exec(code,{})
            return json.loads(output.getvalue())
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)/'owned';root.mkdir()
            data=b'\x89PNG\r\n\x1a\nsynthetic test bytes'
            payload=root/'capture.png';payload.write_bytes(data)
            foreign=Path(td)/'foreign.png';foreign.write_bytes(data)
            link=root/'link.png';link.symlink_to(payload)
            scope=SimpleNamespace(json=execute,ctx=SimpleNamespace(target=SimpleNamespace(root=str(root))))
            self.assertEqual(outcomes_live._saved_digest(scope,str(payload)),hashlib.sha256(data).hexdigest())
            for path in (foreign,link):
                with self.assertRaises(AssertionError):outcomes_live._saved_digest(scope,str(path))

    def test_frame_transport_retries_one_timeout_and_closes_both_clients(self):
        from tools.lab.surface_driver.target import Target
        from tools.lab.surface_driver import target
        ctx=SimpleNamespace(physical=None,info={'vnc_display':20})
        first,second=Mock(),Mock()
        first.frame.side_effect=TimeoutError()
        second.frame.return_value=(1,1,b'\x00\x00\x00\x00')
        with patch.object(target.rfb,'RFB',side_effect=[first,second]) as connect:
            image=Target.capture_image.__wrapped__(ctx)
        self.assertEqual(image.size,(1,1));self.assertEqual(connect.call_count,2)
        first.close.assert_called_once();second.close.assert_called_once()

    def test_recipient_comparison_rejects_wrong_bytes_and_foreign_saved_paths(self):
        from tools.lab.surface_driver import browser
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);app=root/'app';app.mkdir();state=root/'firefox';state.mkdir()
            downloads=state/'downloads';downloads.mkdir()
            saved=app/'saved.png';saved.write_bytes(b'\x89PNG\r\n\x1a\noriginal')
            foreign=root/'foreign.png';foreign.write_bytes(b'private unrelated input')
            link=app/'link.png';link.symlink_to(saved)
            (state/'transaction.json').write_text(json.dumps({'directory':str(state)}))
            client=Mock()
            def receive(*a,**kw):
                (downloads/'received.png').write_bytes(b'\x89PNG\r\n\x1a\nwrong bytes')
                return None if 'role=menu' in a[0] else {'element-6066-11e4-a52e-4f735466cecf':'synthetic-control'}
            client.script.side_effect=receive
            args=dict(directory=str(state),port=1,kind='proton',saved_path=str(saved),app_root=str(app),filename='received.png')
            observation=lambda *a,**k:{'available':True,'entries':[{'succeeded':True,'stopped':True,'error':None,'target':str(downloads/'received.png')}]}
            with patch.object(browser,'_recipient_preview_pixels',return_value={'matches':True}),patch.object(browser,'_firefox_downloads',side_effect=observation),patch.object(browser,'_Marionette',return_value=client),patch.object(browser.subprocess,'run',return_value=SimpleNamespace(stdout='tmpfs\n')):
                result=browser._target('download-digest',args)
                self.assertFalse(result['matches'])
                self.assertEqual(set(result),{'bytes','sha256','matches','preview','downloadObservations'})
                client.call.assert_any_call('WebDriver:ElementClick',{'id':'synthetic-control'})
                for path in (foreign,link):
                    (downloads/'received.png').unlink(missing_ok=True)
                    with self.assertRaises(browser._TargetError):
                        browser._target('download-digest',dict(args,saved_path=str(path)))
                client.script.side_effect=None
                for digest,matched in ((hashlib.sha256(saved.read_bytes()).hexdigest(),True),('0'*64,False)):
                    client.script.return_value={'bytes':saved.stat().st_size,'sha256':digest}
                    result=browser._target('download-digest',dict(args,kind='s3'))
                    self.assertEqual(result['matches'],matched)


if __name__=='__main__':unittest.main()
