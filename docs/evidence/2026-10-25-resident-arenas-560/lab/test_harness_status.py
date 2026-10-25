"""Status propagation of resident_burst_o3 r3 without launching anything: fake Run objects only."""
import importlib.util, os, sys
spec=importlib.util.spec_from_file_location('h',sys.argv[1]);h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
assert h.cleanup_failure(0,False,[]) is None and h.cleanup_failure(None,False,[]) is None
assert h.cleanup_failure(1,False,[])=='resident_exit_1' and h.cleanup_failure(-15,False,[])=='resident_exit_-15'
assert h.cleanup_failure(0,True,[])=='forced_cleanup_fallback' and h.cleanup_failure(0,False,[{'pid':1}])=='tracked_processes_remaining'
class Fake:
    def __init__(self,fail=None,problem=None):self.fail=fail;self.problem=problem;self.events=[];self.closed=False
    def run(self):
        if self.fail:raise self.fail
    def event(self,kind,**v):self.events.append(kind)
    def close(self):self.closed=True;return self.problem
def outcome(fake):
    try:h.main(lambda:fake);return 'ok'
    except SystemExit as e:return f'exit {e.code}'
    except Exception as e:return type(e).__name__
cases=[(Fake(),'ok'),(Fake(problem='resident_exit_1'),'exit 3'),(Fake(problem='forced_cleanup_fallback'),'exit 3'),
       (Fake(fail=RuntimeError('workload')),'RuntimeError'),(Fake(fail=RuntimeError('workload'),problem='tracked_processes_remaining'),'RuntimeError')]
for fake,expected in cases:
    got=outcome(fake);assert got==expected and fake.closed,(got,expected)
print(f'harness r3 status: 6 cleanup_failure cases + {len(cases)} propagation cases pass')
