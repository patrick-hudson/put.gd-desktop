"""Local campaign adapter; real provider adapters deliberately fail closed."""
from ..api import RecipeBlocked
from .base import Fixture
from .geometry import LiveFixture


def provider_adapter(scope):
    raise RecipeBlocked('live Proton/S3 adapter is intentionally deferred to the authorized builder lane; this slice is loopback-only',issue=669)


def local(scope):
    ctx=scope.ctx
    if ctx.destination not in ('local','s3'):return provider_adapter(scope)
    if ctx.destination=='s3':
        raise RecipeBlocked('live S3 requires verified sensitive_transport; loopback fixtures do not provide it',issue=669)
    service=ctx.fixture('loopback-s3')
    scene=ctx.fixture('selector-desktop')
    image=ctx.fixture('image')
    x,y,w,h=scene.screens[0]
    return LiveFixture(ctx,configured=True,isolated=True,local_fixture=True,profile_id=service.profile_id,
                   profile_label=service.profile['label'],object_prefix=service.profile['settings']['objectPrefix'],
                   folder_name='campaign-'+scope.token[:12],expected_host='127.0.0.1',image_path=image.path,
                   region_start=[x+w//4,y+h//4],region_end=[x+3*w//4,y+3*h//4],window_focus=[x+w//2,y+h//2],
                   region_shortcut='Ctrl+Shift+1',window_shortcut='Ctrl+Shift+2',owned_history_ids=[],
                   sensitive_transport=False,redacted_frames=False,
                   cleanup=lambda:ctx._cleanup_fixtures() if hasattr(ctx,'_cleanup_fixtures') else ctx.manager.close())
