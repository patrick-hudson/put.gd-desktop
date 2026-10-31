from ..api import recipe
from ..fixtures.live_outcomes import admit
from . import outcomes

@recipe('region.drag', destinations=('s3',), desktop_drawn=True, timeout=180)
def guard_only(ctx):
    admit(ctx)
    ctx.fixture('app-profile')
    outcomes._pattern(ctx)
    ctx.note('Pattern-only diagnostic: no capture, credential import or upload attempted')
