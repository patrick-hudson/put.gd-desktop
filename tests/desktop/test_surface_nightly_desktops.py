"""Controller policy tests; these do not claim graphical desktop acceptance."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab import nightly_desktops as nightly
from tools.lab.surface_driver.collect import mark_terminal

BUILD = 'a'*40


class FakeGitHub:
    repo = 'owner/repo'
    pinned = dict(number=1, title='Nightly desktop results', node_id='PIN', html_url='https://example.test/1')

    def __init__(self, failure=None):
        self.failure, self.writes = failure, []

    def issues(self): return [self.pinned]+([self.failure] if self.failure else [])

    def issue(self, title, body, existing=None):
        self.writes.append(('issue', title, existing, body))
        return dict(existing or self.pinned, html_url='https://example.test/issue')

    def api(self, path, method='GET', body=None):
        self.writes.append((method, path, body))
        if method == 'GET': return dict(node_id='REPO')
        return dict(html_url='https://example.test/receipt')

    def gh(self, *args):
        return json.dumps(dict(data=dict(node=dict(pinnedIssues=dict(nodes=[dict(issue=dict(id='PIN'))])))))


class NightlyTests(unittest.TestCase):
    def receipt(self, root, *, functional='passed', review=True, cleanup=None, note='pixel oracle mismatch'):
        value = dict(environment=dict(build=BUILD, observed_build=dict(gitCommit=BUILD,dirty=False),
                coverage_export=dict(published=True,scanned_before_copy=True,copy_scan_passed=True),
                cleanup_errors=cleanup), secret_scan=dict(passed=True),
            interactions=[dict(recipe='outcomes.region_upload',functional=functional,review=review,visual='frames',note=note)])
        (root/'receipt.json').write_text(json.dumps(value))
        (root/'coverage.json').write_text('{}')
        mark_terminal(root, 0 if functional == 'passed' else 2)

    def test_review_does_not_gate_automatic_oracles(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.receipt(Path(tmp))
            self.assertEqual(nightly.verdict(tmp,'outcomes.region_upload')['status'],'green')

    def test_only_verified_app_oracle_failure_is_red(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.receipt(root,functional='failed')
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'red')
            for kwargs in (dict(functional='blocked'),dict(cleanup=['owned process retained']),
                           dict(functional='failed',note='driver error: OSError'),
                           dict(functional='failed',note='recipe deadline exceeded')):
                self.receipt(root,**kwargs)
                self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')
            self.receipt(root)
            self.assertEqual(nightly.verdict(root,'outcomes.history_delete')['status'],'not_run')
            (root/'receipt.json').write_text('{}')
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')

    def test_unverified_installed_build_is_grey(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);self.receipt(root,functional='failed')
            value=json.loads((root/'receipt.json').read_text())
            value['environment']['observed_build']['gitCommit']='b'*40
            (root/'receipt.json').write_text(json.dumps(value));mark_terminal(root,2)
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')

    def test_grey_reason_is_one_table_line_and_disabled_is_explicit(self):
        result=dict(desktops=[dict(desktop='Aurora',host='aurora-amd',notRunReason='load at core count',cells={
            'region_upload':nightly.cell(assertion='load at core count'),
            'history_delete':nightly.cell('green')})])
        table=nightly.table(result)
        self.assertIn('⚪',table);self.assertNotIn('🔴',table)
        self.assertEqual(table.count('load at core count'),1)
        self.assertIn('🟢 | — | —',table)

    def test_daily_skips_tested_latest_release_and_now_forces_it(self):
        releases=[dict(tag_name=f'v26.1031.{i}-nightly',draft=False,published_at=f'2026-10-31T0{i}:00:00Z') for i in (1,2,3)]
        latest=releases[-1]['tag_name']
        self.assertEqual(nightly.select_release(releases)['tag_name'],latest)
        self.assertIsNone(nightly.select_release(releases,[latest]))
        self.assertEqual(nightly.select_release(releases,[latest],True)['tag_name'],latest)
        timer=(nightly.ROOT/'tools/lab/putgd-nightly-desktops.timer').read_text()
        self.assertIn('OnCalendar=*-*-* 07:30:00 UTC',timer)
        self.assertNotIn('*:00,30',timer)

    def test_all_five_hosts_are_dispatched_concurrently(self):
        barrier=threading.Barrier(5)
        seen=[]
        def dispatch(host,builder,*args):
            seen.append(host);barrier.wait(timeout=3)
            return [dict(desktop=host,host=host,cells={},totalSeconds=0)]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);artifacts=root/'artifacts';artifacts.mkdir()
            result=dict(desktops=[])
            nightly.run_hosts({h:dict(rows=[{}]) for h in nightly.HOSTS},'tag',artifacts,root,result,dispatch)
            self.assertCountEqual(seen,nightly.HOSTS)
            self.assertEqual(len(result['desktops']),5)

    def test_capacity_wait_has_twenty_minute_default_and_holds_no_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock=[0.0]
            def sleep(seconds):clock[0]+=seconds
            config=dict(host='m74',rows=[dict(live=True)])
            with patch.object(nightly.Path,'home',return_value=Path(tmp)),patch('tools.lab.desktop_lock.Desktop') as lock,patch.object(nightly,'preflight',side_effect=nightly.NotRun('RAM short')):
                lock.return_value.status.return_value=dict(state='free')
                with self.assertRaisesRegex(nightly.NotRun,'1200s'):
                    nightly.await_host(config,Path(tmp),clock=lambda:clock[0],sleep=sleep)
                self.assertEqual(clock[0],1200)
                lock.return_value.acquire.assert_not_called()

    def test_capacity_wait_proceeds_once_resources_return(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock=[0.0]
            def sleep(seconds):clock[0]+=seconds
            with patch.object(nightly.Path,'home',return_value=Path(tmp)),patch('tools.lab.desktop_lock.Desktop') as lock,patch.object(nightly,'preflight',side_effect=[nightly.NotRun('load high'),dict(ready=True)]):
                lock.return_value.status.return_value=dict(state='free')
                waited=nightly.await_host(dict(host='d69',rows=[{}]),Path(tmp),clock=lambda:clock[0],sleep=sleep)
                self.assertEqual(waited,30)

    def test_one_grouped_failure_issue_per_desktop_and_grey_does_not_close(self):
        failure=dict(number=2,title='nightly: Cinnamon failing',state='open')
        github=FakeGitHub(failure)
        row=dict(desktop='Cinnamon',host='m74',evidence='/owned/run',cells={
            'region_upload':nightly.cell('red','pixel mismatch'),
            'history_delete':nightly.cell('red','object still exists'),
            'link_browser':nightly.cell(assertion='browser unavailable')})
        result=dict(release='v26.1031.3384-nightly',build=BUILD,date='2026-10-31',desktops=[row])
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,result,Path(tmp))
            updates=[w for w in github.writes if w[0]=='issue' and w[1]==failure['title']]
            self.assertEqual(len(updates),1);self.assertIs(updates[0][2],failure)
            self.assertIn('Region',updates[0][3]);self.assertIn('History delete',updates[0][3])
            github.writes.clear()
            for v in row['cells'].values():v.update(status='not_run')
            nightly.publish(github,result,Path(tmp))
            self.assertFalse(any(w[0]=='issue' and w[1]==failure['title'] for w in github.writes))
            self.assertFalse(any(w[0]=='PATCH' for w in github.writes))
            github.writes.clear()
            for v in row['cells'].values():v.update(status='green')
            nightly.publish(github,result,Path(tmp))
            self.assertTrue(any(w[0]=='PATCH' and w[1].endswith('/2') and w[2]['state']=='closed' for w in github.writes))

    def test_grey_infrastructure_creates_no_failure_issue(self):
        github=FakeGitHub()
        row=nightly.not_run_rows('f44',dict(rows=[dict(name='Fedora GNOME')]),'unreachable',Path('/owned'))[0]
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,dict(release='tag',date='today',build='unverified',desktops=[row]),Path(tmp))
        self.assertEqual([w[1] for w in github.writes if w[0]=='issue'],['Nightly desktop results'])

    def test_timer_qualification_requires_three_verified_real_desktops(self):
        result=dict(build=BUILD,desktops=[])
        for _ in range(2):result['desktops'].append(dict(cells=dict(region_upload=nightly.cell('green',terminal={'proof':True},observedBuild=BUILD))))
        self.assertEqual(nightly.eligible_desktops(result),2)
        result['desktops'].append(dict(cells=dict(region_upload=nightly.cell('red',terminal={'proof':True},observedBuild=BUILD))))
        self.assertEqual(nightly.eligible_desktops(result),3)
        result['desktops'][0]['cells']['region_upload'].update(status='not_run')
        self.assertEqual(nightly.eligible_desktops(result),2)

    def test_secret_file_permissions(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'token';path.write_text('synthetic-only');path.chmod(0o644)
            with self.assertRaises(RuntimeError):nightly.private_file(path)
            path.chmod(0o600);self.assertEqual(nightly.private_file(path),path)
            link=Path(tmp)/'link';link.symlink_to(path)
            with self.assertRaises(RuntimeError):nightly.private_file(link)

    def test_checksum_failure_blocks_before_guest_launch(self):
        class GitHub:
            repo='owner/repo'
            def gh(self,*args):
                target=Path(args[args.index('--dir')+1]);name=args[args.index('--pattern')+1]
                (target/name).write_text('0'*64+'  package.deb\n' if name=='SHA256SUMS' else '{}')
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError,'checksum mismatch'):
                nightly.release_assets(GitHub(),'v26.1031.3384-nightly',Path(tmp)/'packages',[dict(asset='ubuntu26.04-x86_64.deb')])


if __name__=='__main__':unittest.main()
