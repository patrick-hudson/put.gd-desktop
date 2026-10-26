"""Multi-output review regressions. Synthetic files/protocols only; no desktop/host."""
import ast
import copy
import datetime
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import headless_output
import multi_output as driver
import multi_output_fixture as fixture
import multi_output_native as native
import multi_output_vm_wall as vmwall
import record_run
import wall_probe


def monitor(x=0, y=0):
    return dict(x=x, y=y, width=1920, height=1080, allocatedWidth=1920, allocatedHeight=1080,
                placedRect=[x,y,1920,1080], fullscreen=True, draws=2)


class Fixtures(unittest.TestCase):
    def test_same_basename_and_real_home_do_not_alias_and_native_stays_in_run(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            for name in ['first','retry']: (root/name).mkdir()
            with mock.patch.object(driver.lab,'ssh',return_value=json.dumps({'home':'/var/home/lab','sockets':['/run/user/1000/putgd/session.sock']})):
                a=driver.Campaign(SimpleNamespace(run=root/'vm',output=root/'first/trial'))
                b=driver.Campaign(SimpleNamespace(run=root/'vm',output=root/'retry/trial',remote_root=root/'owned'))
            self.assertTrue(a.remote.startswith('/var/home/lab/putgd-lab/'))
            self.assertNotEqual(a.remote,b.remote)
            self.assertTrue(Path(b.remote).is_relative_to(root/'owned'))
            self.assertNotEqual(a.fixture_script,b.fixture_script)

    def test_readiness_identity_fullscreen_draw_and_placement(self):
        ready={'attempt':'ours','pid':42,'backend':'GdkWaylandDisplay','monitors':[monitor(),monitor(1920)]}
        self.assertTrue(driver.fixture_ready(ready,'ours',42,2,'wayland'))
        for key,value in [('attempt','old'),('pid',43)]:
            wrong=copy.deepcopy(ready);wrong[key]=value
            with self.assertRaisesRegex(RuntimeError,'different attempt/process'):
                driver.fixture_ready(wrong,'ours',42,2,'wayland')
        for key,value in [('fullscreen',False),('draws',0),('allocatedWidth',900),('placedRect',[0,0,2,2])]:
            delayed=copy.deepcopy(ready);delayed['monitors'][1][key]=value
            self.assertFalse(driver.fixture_ready(delayed,'ours',42,2,'wayland'))
            self.assertFalse(fixture.geometry_ready(delayed['monitors']))
        self.assertFalse(driver.fixture_ready(ready,'ours',42,3,'wayland'))

    def campaign(self, root):
        c=driver.Campaign.__new__(driver.Campaign)
        c.remote='/home/lab/putgd-lab/mh-ours';c.fixture_script='/tmp/fixture-ours.py';c.out=root
        c.run=root;c.profile={'session':'wayland'};c.stop_fixture=mock.Mock();c.shots=mock.Mock()
        return c

    def test_dead_new_process_never_fetches_old_ready(self):
        with tempfile.TemporaryDirectory() as folder:
            c=self.campaign(Path(folder));c.session=mock.Mock(side_effect=[json.dumps({'pid':42,'start':'100'}),'False'])
            c.fetch_json=mock.Mock()
            with mock.patch.object(driver.lab,'put'):
                with self.assertRaisesRegex(RuntimeError,'exited before readiness'):c.fixture()
            c.fetch_json.assert_not_called()

    def test_incomplete_ready_retries_then_each_generation_is_new(self):
        with tempfile.TemporaryDirectory() as folder:
            c=self.campaign(Path(folder));scripts=[];seen=[]
            def session(script):
                scripts.append(script)
                # Parse every generated Python heredoc without executing input or GUI.
                ast.parse(script.split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0])
                subprocess.run(['sh','-n'],input=script,text=True,check=True)
                return json.dumps({'pid':42,'start':'100'}) if script.startswith('nohup') else 'True'
            c.session=session
            def fetch(remote,local):
                seen.append(remote);r={'attempt':c.fixture_attempt,'pid':42,'backend':'GdkWaylandDisplay','monitors':[monitor(),monitor(1920)]}
                if len(seen)==1:r['monitors'][1]['fullscreen']=False
                local.write_text(json.dumps(r));return r
            c.fetch_json=fetch
            with mock.patch.object(driver.lab,'put'),mock.patch.object(driver.time,'sleep'):
                c.fixture();first=c.fixture_remote;c.fixture()
            self.assertNotEqual(first,c.fixture_remote)
            self.assertEqual(len(seen),3)


class InputsAndWall(unittest.TestCase):
    def test_diagonal_span_starts_and_ends_on_real_heads(self):
        heads=[monitor(0,1080),monitor(1920,0)]
        points=driver.drag_points('span',heads)
        self.assertEqual(points,[(960,1620),(2880,540)])
        self.assertEqual(driver.selection('span',heads),[960,540,1920,1080])
        c=driver.Campaign.__new__(driver.Campaign);c.monitors=heads;c.run='owned';c.current='span';c.shots=mock.Mock()
        with mock.patch.object(driver.lab,'pointer') as pointer,mock.patch.object(driver.time,'sleep'):
            c.drag(points)
        self.assertEqual(pointer.call_args_list[0].args[1:3],points[0])
        self.assertEqual(pointer.call_args_list[-1].args[1:3],points[1])

    def test_right_click_keeps_last_pointer_in_staggered_layout(self):
        control=SimpleNamespace(width=4000,height=2500,buttons=0,pointer=mock.Mock())
        p=native.PortalPointer(control,mock.Mock())
        with self.assertRaisesRegex(RuntimeError,'preceding pointer'):p.input_events(None,[{'type':'btn','data':{'button':'right','down':True}}])
        p.pointer(None,2000,1700,4000,2500)
        for down in [True,False]:p.input_events(None,[{'type':'btn','data':{'button':'right','down':down}}])
        self.assertEqual(control.pointer.call_args_list[-1].args,(2000,1700))
        self.assertEqual(control.buttons,0)

    def frame(self,changes=(),black=False):
        data=bytearray(bytes([0,0,0,0] if black else [40,30,220,0])*10000)
        for x in changes:data[x*4:x*4+4]=bytes([255,255,255,0])
        return (100,100,bytes(data))

    def test_one_pixel_and_mostly_frozen_head_fail(self):
        layout=[dict(name='head',x=0,y=0,width=100,height=100)]
        for n in [1,99]:self.assertFalse(wall_probe.check_frames(self.frame(),self.frame(range(n)),layout)['ok'])
        self.assertTrue(wall_probe.check_frames(self.frame(),self.frame(range(100)),layout)['ok'])

    def test_crop_cannot_choose_its_own_extent_and_rotation_needs_native_and_qmp(self):
        r=vmwall.wall_rect('V2',{'size':[2560,1440],'rotated':False},[2560,1440])
        result=wall_probe.check_frames((1920,1080,b''),(1920,1080,b''),[r])
        self.assertFalse(result['ok']);self.assertEqual(result['expected'],[2560,1440])
        with self.assertRaisesRegex(RuntimeError,'native mode/rotation'):vmwall.wall_rect('V2',{'size':[2560,1440],'rotated':False},[1440,2560])
        self.assertEqual(vmwall.wall_rect('V2',{'size':[2560,1440],'rotated':True},[1440,2560])['height'],2560)
        with self.assertRaises(RuntimeError):vmwall.wall_rect('V2',{'size':[2560,1440],'rotated':True},[720,1280])

    def test_invalid_final_screenshot_is_deleted(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'final.png'
            def screenshot(p):p.write_bytes(b'not retained');return self.frame(black=True)
            c=SimpleNamespace(screenshot=screenshot)
            result=wall_probe.save_checked_frame(c,self.frame(),[dict(name='head',x=0,y=0,width=100,height=100)],path)
            self.assertFalse(result['ok']);self.assertFalse(path.exists())

    def test_real_native_data_home_is_collected_and_used(self):
        env=b'XDG_SESSION_TYPE=x11\0XDG_DATA_HOME=/native/custom-data\0DISPLAY=:0\0'
        with mock.patch.object(record_run.subprocess,'run',return_value=SimpleNamespace(stdout='42')),mock.patch.object(record_run.Path,'read_bytes',return_value=env):
            actual,_=record_run.session_environment()
        self.assertEqual(native.applications_directory(actual),Path('/native/custom-data/applications'))


class Recording(unittest.TestCase):
    def render(self,path,index=1,size=(160,120),motion=False,offset=0,phase=120):
        from PIL import Image
        im=Image.new('RGB',size)
        for y in range(size[1]):
            for x in range(size[0]):
                gx,gy=80+x+offset,80+y
                color=driver.COLORS[(index*4+gx//128%2+2*(gy//128%2))%8]
                im.putpixel((x,y),(255,255,255) if motion and (gx-phase)%128<32 else color)
        im.save(path)

    def test_video_size_head_and_spatial_colors_not_just_motion(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'sample.png';heads=[monitor(),monitor(1920)];rect=[2000,80,160,120]
            self.render(p,motion=True)
            self.assertTrue(driver.recording_oracle([p]*8,heads,rect,[1,1],True)['ok'])
            self.render(p,motion=True,phase=80)  # two bars can occupy 40% of this crop
            self.assertTrue(driver.recording_oracle([p]*8,heads,rect,[1,1],True)['ok'])
            self.render(p,index=0,motion=True)
            self.assertFalse(driver.recording_oracle([p]*8,heads,rect,[1,1],True)['ok'])
            self.render(p,size=(320,240))
            self.assertEqual(driver.recording_oracle([p]*8,heads,rect,[1,1])['reason'],'video-size')
            self.render(p,offset=128)
            self.assertFalse(driver.recording_oracle([p]*8,heads,rect,[1,1])['ok'])
            self.assertEqual(driver.recording_oracle([p]*8,heads,rect,[1,1.5])['reason'],'scaled-or-spanning-video-reference-unproved')
            self.render(p)
            self.assertTrue(driver.recording_oracle([p]*8,heads,rect,[1,1])['ok'])
            self.assertFalse(driver.recording_oracle([p]*7,heads,rect,[1,1])['ok'])

    def test_recording_always_restores_static_fixture_before_next_image_case(self):
        for fail in [False,True]:
            with self.subTest(fail=fail),tempfile.TemporaryDirectory() as folder:
                c=driver.Campaign.__new__(driver.Campaign);c.results=[];c.out=Path(folder);c.shots=mock.Mock(return_value=[])
                state={'motion':False}
                def fixture(motion=False):state['motion']=motion
                c.fixture=mock.Mock(side_effect=fixture)
                def clip(label):
                    if fail and label=='moving':raise RuntimeError('encoder failed')
                    return dict(motion=2 if state['motion'] else 0,placementOk=True,content={'ok':True})
                c.record_clip=clip;c.recording()
                self.assertFalse(state['motion'])
                self.assertEqual(c.results[0]['ok'],not fail)
                self.assertEqual(c.fixture.call_args_list,[mock.call(motion=True),mock.call()])


class Rollback(unittest.TestCase):
    def test_accuracy_and_restore_margin_in_expiry_admission(self):
        with mock.patch.object(headless_output.time,'time',return_value=100):
            with self.assertRaises(headless_output.Refused):headless_output.rollback_deadline(190,30)
            self.assertEqual(headless_output.rollback_deadline(191,30),131)

    def test_slow_snapshot_rechecks_expiry_before_arming_or_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);edid=root/'head.edid';edid.write_bytes(headless_output.edid())
            args=SimpleNamespace(card='card0',connector='HDMI-A-2',keep='HDMI-A-1',edid=edid,run=root/'run',session='ours',seconds=30)
            state={'outputs':{'HDMI-A-1':{'enabled':'enabled'},'HDMI-A-2':{'enabled':'disabled','status':'disconnected'}},'connectors':{'HDMI-A-1':{'encoders':[98]},'HDMI-A-2':{'encoders':[114]}}}
            clock=[100]
            def snapshot(_):clock[0]=150;return state
            expiry=datetime.datetime.fromtimestamp(200,datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
            with mock.patch.object(headless_output,'lease',return_value={'expires':expiry}),mock.patch.object(headless_output.time,'time',side_effect=lambda:clock[0]),mock.patch.object(headless_output,'snapshot',side_effect=snapshot),mock.patch.object(headless_output,'command') as command,mock.patch.object(headless_output,'root_write') as write:
                with self.assertRaises(headless_output.Refused):headless_output.enable(args)
                command.assert_not_called();write.assert_not_called()

    def test_timer_coalescing_is_explicit_before_any_kernel_write(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);edid=root/'head.edid';edid.write_bytes(headless_output.edid())
            args=SimpleNamespace(card='card0',connector='HDMI-A-2',keep='HDMI-A-1',edid=edid,run=root/'run',session='ours',seconds=30)
            state={'outputs':{'HDMI-A-1':{'enabled':'enabled'},'HDMI-A-2':{'enabled':'disabled','status':'disconnected'}},'connectors':{'HDMI-A-1':{'encoders':[98]},'HDMI-A-2':{'encoders':[114]}}}
            expiry=(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(minutes=5)).strftime('%Y-%m-%dT%H:%M:%SZ')
            def arm(command):
                self.assertIn('--timer-property=AccuracySec=1s',command)
                self.assertIn('--timer-property=RandomizedDelaySec=0',command)
                raise OSError('intentional stop before any write')
            with mock.patch.object(headless_output,'lease',return_value={'expires':expiry}),mock.patch.object(headless_output,'snapshot',return_value=state),mock.patch.object(headless_output,'command',side_effect=arm),mock.patch.object(headless_output,'root_write') as write:
                with self.assertRaises(OSError):headless_output.enable(args)
                write.assert_not_called()


if __name__=='__main__':unittest.main()
