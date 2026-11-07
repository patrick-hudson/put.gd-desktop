"""Audit transitive project headers across the S3 process/module boundary."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
cm=(root/'cmake/S3.cmake').read_text()
def closure(targets):
 pending=[]
 for target in targets:
  for match in re.finditer(r'(?:add_library|add_executable|target_sources)\(\s*'+re.escape(target)+r'\s+([^)]*)\)',cm):
   pending += [root/p for p in re.findall(r'src/[\w/.-]+\.(?:cpp|h)',match[1])]
 seen=set()
 while pending:
  path=pending.pop().resolve()
  if path in seen:continue
  seen.add(path)
  for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"',path.read_text(),re.M):
   for p in (path.parent/include,root/'src'/include):
    if p.is_file():pending.append(p);break
 return {p.relative_to(root).as_posix() for p in seen if p.suffix=='.h'}
groups=[closure(t) for t in [('putgd_s3','putgd_s3_wire'),('putgd_s3_module',),('putgd-s3-worker','putgd_s3_worker_host','putgd_s3_wire')]]
shared={p for p in set.union(*groups) if sum(p in g for g in groups)>1}
listed=set(re.findall(r'[\w/.-]+\.(?:h|cpp|json)',cm.split('foreach(s3_contract_file',1)[1].split(')',1)[0]))
import unittest

class S3ContractTest(unittest.TestCase):
 def test_shared_headers_are_hashed(self):
  self.assertIn('src/storage/s3/S3Multipart.h', shared)
  self.assertEqual(sorted(shared-listed), [],
                   'Headers shared by resident/module/worker must enter PUTGD_S3_BUILD_ID')

if __name__ == '__main__':
 unittest.main()
