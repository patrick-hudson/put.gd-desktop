#!/usr/bin/env python3
"""Local-only final receipt: actual native oracles plus explicit image review; no guest access."""
import argparse, importlib.util, json, os
from pathlib import Path
os.umask(0o077)
P=Path(__file__).resolve().parent
sp=importlib.util.spec_from_file_location('c2_driver',P/'driver.py');d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
def main():
 a=argparse.ArgumentParser();a.add_argument('--matrix',required=True);a.add_argument('--quiet',required=True);a.add_argument('--visual-review',required=True);args=a.parse_args();pin=d.validate_pin();digest=d.sha(d.ROOT/'candidate.json')
 def owned(path):
  q=Path(path).resolve();d.check(q.is_relative_to(d.ROOT),'receipt_outside_private_root');return q
 def receipt(path):
  q=owned(path);v=d.read(q);d.check(v.get('passed') is True and v['candidatePinSha256']==digest and v['resident']==quiet['resident'],'receipt_not_current_pass');return v
 quiet=d.read(owned(args.quiet));d.check(quiet['phase']=='quiet' and quiet['passed'] and quiet['candidatePinSha256']==digest,'quiet_not_current_pass');d.check(quiet['after']['ui']['pid']==0 and quiet['after']['diagnostics']['activeOperations']==0 and quiet['noFurtherInputDuringParentMemory'],'quiet_not_drained')
 matrix=receipt(args.matrix);d.check(matrix['phase']=='matrix' and matrix['nativeMatrixPassed'],'matrix_not_passed');d.check(len(matrix['cases'])==12 and {(x['scenario'],x['client']) for x in matrix['cases']}==set(d.CASES),'matrix_incomplete');d.check(all(x['exitCode']==0 and x['result']['ok'] and x['result']['runningApp']==quiet['resident'] for x in matrix['cases']),'matrix_case_failed')
 visual=d.read(owned(args.visual_review));d.check(visual['candidatePinSha256']==digest and visual['actualImagesInspected'] is True,'visual_review_required')
 required={'tray-rendered','tray-menu','tray-dismissal','settings-controls','native-folder-dialog','settings-cancel-discard','history-selected-inspector','activity-rendered'}
 d.check(required==set(visual['cases']),'visual_case_set_incomplete')
 for name,v in visual['cases'].items():
  d.check(v['passed'] is True and v['finding'],'visual_case_not_passed_'+name)
  q=owned(v['witness']['path']);d.check(d.sha(q)==v['witness']['sha256'],'witness_changed_'+name)
 settings=receipt(visual['settingsProofReceipt']);d.check('settingsDiscardProof' in settings,'settings_discard_oracle_missing')
 d.check(set(visual['normalViews'])=={'settings','history','activity'},'view_lifecycle_missing')
 for view,v in visual['normalViews'].items():
  opened=receipt(v['openReceipt']);closed=receipt(v['closeReceipt']);d.check(opened['phase']=='show' and opened['view']==view and closed['phase']=='close','wrong_view_receipt');d.check(closed['before']['ui']['pid']==opened['helperIdentity']['pid'] and closed['normalExit']['pid']==0 and closed['normalExit']['lastExit']['status']=='normal' and closed['normalExit']['lastExit']['code']==0,'view_not_normally_closed')
 fixture=receipt(visual['fixtureCloseReceipt']);d.check(fixture['phase']=='fixture-close' and fixture['normalFixtureClose'],'fixture_close_missing')
 result={'schemaVersion':1,'candidateId':'s3-transport-diagnosis','candidateBuild':pin['build'],'candidatePinSha256':digest,'packageSha256':pin['packageSha256'],'installedBinarySha256':pin['installedBinarySha256'],'functionalPassed':True,'memoryExecuted':False,'siblingsExecuted':True,'desktop':'kde-wayland','physicalPreviewTouched':False,'resident':quiet['resident'],'matrix':{'path':str(owned(args.matrix)),'sha256':d.sha(args.matrix)},'quiet':{'path':str(owned(args.quiet)),'sha256':d.sha(args.quiet)},'visualReview':{'path':str(owned(args.visual_review)),'sha256':d.sha(args.visual_review)},'finalizedUtc':d.now(),'scope':'Current-version real kde-wayland synthetic local-only functional checks. Parent owns separate live-S3 workload, fixed memory gates, and any cross-desktop expansion.'}
 out=d.ROOT/'kde-wayland-functional.json';d.save(out,result);out.chmod(0o400);print(json.dumps({'receipt':str(out),'sha256':d.sha(out),'functionalPassed':True,'memoryExecuted':False}))
if __name__=='__main__':main()
