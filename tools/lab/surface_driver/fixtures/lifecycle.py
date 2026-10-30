"""Fresh recording capture prerequisites, shared by CLI and MCP walkthroughs."""
from ..api import RecipeBlocked
from .base import Fixture
from .uploads import track


def source(scope):
    """Stage only the distro GTK sink prerequisite in this disposable overlay."""
    ctx=scope.ctx
    ctx.fixture('app-profile')
    check="python3 -c \"import gi;gi.require_version('Gst','1.0');from gi.repository import Gst;Gst.init(None);assert Gst.ElementFactory.find('gtksink') or Gst.ElementFactory.find('gtkglsink')\""
    if ctx.shell(check)['returncode']:
        if ctx.placement not in ('boat','guest') or not ctx.profile.get('guest'):
            raise RecipeBlocked('synthetic motion needs a real GTK3 GStreamer sink (gstreamer1.0-gtk3)',issue=672)
        result=ctx.shell('sudo -n /usr/bin/apt-get install -y --no-install-recommends gstreamer1.0-gtk3 >/dev/null 2>&1',timeout=180)
        if result['returncode'] or ctx.shell(check)['returncode']:
            raise RecipeBlocked('synthetic motion prerequisite unavailable: gstreamer1.0-gtk3 (gtksink/gtkglsink)',issue=672)
    from .recordings import pattern
    return pattern(scope)


def recording(scope):
    ctx=scope.ctx
    destination=ctx.fixture('destination')
    scene=ctx.fixture('events.recording-source')
    jobs=[]
    def prepare(**overrides):
        params={**scene.prepare,**overrides}
        scene.command('raise')
        value=ctx.call('recording.prepare',params)
        identity=value.get('recordingId') or value.get('id')
        if not identity:raise RecipeBlocked('recording.prepare returned no identity',issue=672)
        jobs.append(identity);track(scope,'recording.cancel',identity)
        return value
    # The caller drives real production prepare/control/select/overlay methods,
    # with real portal consent supplied by its separate owner (#817).
    return Fixture(synthetic=True,fresh=True,scene=scene,prepare=scene.prepare,prepare_recording=prepare,
                   output_dir=scene.output_dir,destination=destination,recording_ids=jobs,
                   capabilities=ctx.call('recording.capabilities'))
