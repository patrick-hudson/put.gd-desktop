#!/usr/bin/env python3
"""Static numeric projection/verifier contracts, no product or desktop runtime."""
import copy,hashlib,importlib.util,json,math,shutil,sys,tempfile,unittest
from pathlib import Path
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent

def module(name):
    s=importlib.util.spec_from_file_location(name,HERE/(name+'.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
v=module('verify');e=module('export')

class Contracts(unittest.TestCase):
    def test_nearest_rank_not_old_index27(self):
        self.assertEqual(v.percent95(list(range(30))),28)
        with self.assertRaises(ValueError):v.percent95([])
    def test_coverage_short_sparse_reversed_gap_rejected(self):
        good=[{'elapsedSeconds':i}for i in range(300)]
        self.assertGreater(v.coverage(good,295,3,101)['spanSeconds'],295)
        for rows in (good[:90],good[::4],good[::-1],good[:-10]):
            with self.assertRaises(ValueError):v.coverage(rows,295,3,101)
    def test_nonfinite_boolean_not_number(self):
        for x in (math.inf,math.nan,True,'1'):
            with self.assertRaises(ValueError):v.number(x)
    def test_projection_omits_payload_config_argv_environment_mappings(self):
        d={'schemaVersion':1,'warmupSeconds':5,'appSha256':'a'*64,'elapsedSeconds':300,
          'idleCpuPercentOneCore':0,'initialRssBytes':1,'finalRssBytes':1,'peakSampledRssBytes':1,
          'budgetPassed':True,'samples':[{'elapsedSeconds':0,'cpuTicks':0,'rssBytes':1,'payload':'PRIVATE'}],
          'initialFootprint':[{'mapping':'PRIVATE','rssBytes':1}], 'finalFootprint':[{'mapping':'PRIVATE','rssBytes':1}],'configuration':'PRIVATE','command':['PRIVATE'],'environment':'PRIVATE'}
        out=e.primary_cold(d);self.assertNotIn('PRIVATE',json.dumps(out));self.assertEqual(len(out['samples']),1)
    def test_build_dirty_stale_and_unknown_metadata_rejected(self):
        b={'schemaVersion':1,'version':'0.1.1-dev.53+g1ee42f32c61c','baseVersion':'0.1.1',
          'packageVersion':'0.1.1~dev.53+g1ee42f32c61c','gitCommit':e.COMMIT,
          'gitTree':'e6584c6e2e002c2296855ad42b5dcd73e69a8ad7','commitCount':53,'dirty':False,'sourceProvenance':'git'}
        self.assertEqual(e.build(b),b)
        for k,val in [('dirty',True),('gitCommit','f'*40),('commitCount',54),('extra','PRIVATE')]:
            d=dict(b);d[k]=val
            with self.assertRaises(ValueError):e.build(d)
    def test_supplement_missing_crash_wronggeneration_peer_ops_tray_rejected(self):
        d=json.loads((HERE.parent/'view-exits-1.json').read_text())['data']
        measured=json.loads((HERE.parent/'views-supplement-1.json').read_text())['data']['residentIdentity']
        self.assertEqual(v.exits(d,d['rawFrozenResult']['sha256'],measured),[1,2,3])
        for mode in ('missing','crash','generation','peer','ops','tray'):
            r=copy.deepcopy(d)
            if mode=='missing':r['cycles'].pop()
            if mode=='crash':r['cycles'][2]['snapshot']['ui']['lastExit']['status']='crash'
            if mode=='generation':r['cycles'][2]['snapshot']['ui']['lastExit']['generation']=2
            if mode=='peer':r['cycles'][2]['socketPeer']['pid']+=1
            if mode=='ops':r['cycles'][2]['snapshot']['activeOperations']=1
            if mode=='tray':r['cycles'][2]['snapshot']['trayAvailable']=False
            with self.assertRaises(ValueError):v.exits(r,d['rawFrozenResult']['sha256'],measured)
    def test_matching_reported_overbudget_or_negative_cpu_rejected(self):
        primary=json.loads((HERE.parent/'cold-1.json').read_text())
        family=json.loads((HERE.parent/'family-1.json').read_text())['data']
        p=primary['data'];p['_originalSha256']=primary['original']['sha256']
        for value in (-1,.5,1):
            raw=copy.deepcopy(p);f=copy.deepcopy(family)
            raw['idleCpuPercentOneCore']=value;f['primaryEvidence']['idleCpuPercentOneCore']=value
            with self.assertRaisesRegex(ValueError,'reported_cpu_budget_failed'):v.cold(raw,f)
    def test_self_matching_foreign_resident_peer_is_not_measured_proof(self):
        d=json.loads((HERE.parent/'view-exits-1.json').read_text())['data']
        measured=json.loads((HERE.parent/'views-supplement-1.json').read_text())['data']['residentIdentity']
        for key,value in [('pid',999),('startTicks',999),('uid',999),
                          ('device',999),('inode',999),('sha256','b'*64)]:
            r=copy.deepcopy(d);r['cycles'][2]['resident'][key]=value
            if key in ('pid','uid'):r['cycles'][2]['socketPeer'][key]=value
            with self.assertRaisesRegex(ValueError,'supplement_measured_resident_join_failed'):
                v.exits(r,d['rawFrozenResult']['sha256'],measured)

    def test_current_export_passes_original_gap_remains(self):
        r=v.verify(HERE.parent);self.assertTrue(r['verified'])
        self.assertEqual(r['p95PaintMilliseconds'],95.400159)
        self.assertTrue(all(x['missingNormalZeroGenerations']==[3]for x in r['views']))
    def test_tampered_projection_or_false_failed_promotion_cannot_verify(self):
        with tempfile.TemporaryDirectory(prefix='pg53-export-contract-')as tmp:
            root=Path(tmp)/'copy';shutil.copytree(HERE.parent,root)
            p=root/'timing.json';p.write_text(p.read_text()+' ')
            with self.assertRaises(ValueError):v.verify(root)
        with tempfile.TemporaryDirectory(prefix='pg53-export-contract-')as tmp:
            root=Path(tmp)/'copy';shutil.copytree(HERE.parent,root)
            p=root/'failed-source52.json';d=json.loads(p.read_text());d['data']['accepted']=True;p.write_text(json.dumps(d))
            manifest=json.loads((root/'manifest.json').read_text())
            for row in manifest['exports']:
                if row['file']==p.name:row.update(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
            (root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):v.verify(root)

if __name__=='__main__':unittest.main(verbosity=2)
