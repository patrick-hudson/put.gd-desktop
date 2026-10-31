"""Authorization boundaries of the dated native-local lab patch."""
import unittest
from types import SimpleNamespace
from tools.lab.surface_driver.native_local import admitted
from tools.lab.surface_driver.api import RecipeBlocked

class NativeAdmission(unittest.TestCase):
    def context(self, **changes):
        self.heartbeats=[]
        target=SimpleNamespace(physical=True,_lock=('m74','owned-session'),
            clipboard_report={'ready':True},heartbeat=lambda **kw:self.heartbeats.append(kw))
        value=SimpleNamespace(destination='local',placement='native',target=target)
        for key,item in changes.items():setattr(value,key,item)
        return value

    def test_held_local_lease(self):
        self.assertTrue(admitted(self.context()))
        self.assertEqual(self.heartbeats,[{'force':True}])

    def test_live_never_admitted(self):
        for destination in ('s3','proton'):
            self.assertFalse(admitted(self.context(destination=destination)))
            self.assertEqual(self.heartbeats,[])

    def test_missing_lock_or_keeper_refused(self):
        for field,value in (('_lock',None),('clipboard_report',{}),('physical',None)):
            ctx=self.context();setattr(ctx.target,field,value)
            self.assertFalse(admitted(ctx));self.assertEqual(self.heartbeats,[])

    def test_expired_lease_refused(self):
        ctx=self.context()
        def reject(**_):raise RecipeBlocked('lease expired')
        ctx.target.heartbeat=reject
        with self.assertRaises(RecipeBlocked):admitted(ctx)

if __name__=='__main__':unittest.main()
