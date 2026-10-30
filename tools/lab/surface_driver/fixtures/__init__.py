"""Shared real-state fixture registry. Nothing starts or mutates on import."""
from functools import partial
import time

from ..api import RecipeBlocked
from .base import Fixture, Scope
from . import destinations, dialogs, events, history, images, live, notifications, profile, recordings, settings, uploads


def blocked(reason, issue):
    def create(scope):raise RecipeBlocked(reason,issue=issue)
    return create


FACTORIES={
    'app-profile':profile.app_profile,
    'profile':profile.app_profile,
    'image':images.image,
    'fake-keyring':destinations.fake_keyring,
    'loopback-s3':destinations.loopback,
    'upload':uploads.upload,
    'linked-upload':uploads.upload,
    'notification_upload':notifications.notification_upload,
    'history':uploads.local_history,
    'history-row':uploads.local_history,
    'local-history':uploads.local_history,
    'history_rows':history.rows,
    'history_pair':history.pair,
    'history_paging':history.paging,
    'history_retry_upload':partial(uploads.submit,remote=True,failure=True),
    'history_retry_local':partial(uploads.submit,local_failure=True),
    'history_active_uploads':history.active,
    'history_delete':uploads.upload,
    'history_saved_copy':history.saved_copy,
    'upload-failed':partial(uploads.submit,remote=True,failure=True),
    'upload-cancelled':partial(uploads.submit,remote=True,cancelled=True),
    'upload-incomplete':events.incomplete,
    'upload-in-flight':partial(uploads.submit,remote=True,active=True),
    'destinations':destinations.destinations,
    'destinations-s3':destinations.destinations,
    'destinations-generic':partial(destinations.destinations,kind='generic'),
    'destinations-b2':partial(destinations.destinations,kind='b2'),
    'destinations-conflict':events.conflict,
    'settings-conflict':events.conflict,
    'events.config-change':events.conflict,
    'destinations-empty':partial(destinations.destinations,kind='empty'),
    'destination':destinations.destination,
    'recording':recordings.recording,
    'recording-review':recordings.recording,
    'review-controls':recordings.controls,
    'recording-export-failure':blocked('export-failure fixture requires a recoverable production export fault',672),
    'recording-budget-stop':blocked('budget-stop recording fixture requires the production byte-budget capture controller',672),
    'recording-preview-error':recordings.preview_error,
    'file-dialog-portal':partial(dialogs.backend,name='portal'),
    'file-dialog-fallback':partial(dialogs.backend,name='fallback'),
    'chooser-failure':dialogs.failure,
    'settings':settings.settings,
    'settings-no-output':partial(settings.settings,state='no-output'),
    'settings-multi-profile':partial(settings.settings,state='multi-profile'),
    'diagnostics-upload':settings.diagnostics_upload,
    'events.slow-upload':events.slow_upload,
    'events.update-feed':events.feed,
    'events.older-update-feed':partial(events.feed,older=True),
    'events.update-notification':events.feed,
    'events.recording-source':recordings.pattern,
    'selector-desktop':recordings.pattern,
    'events.record-storage':events.storage,
    'events.import-storage':events.storage,
    'events.incomplete-multipart':events.incomplete,
    'events.clipboard':events.clipboard,
    'events.controllers':events.controllers,
    'events.browser-handler':blocked('browser handler needs links lane guarded browser lifecycle',682),
    'events.software-updates-handler':blocked('software-updates handler needs an isolated desktop MIME handler with launch witness',682),
    'release':notifications.release,
    'live':live.local,
    'live-provider':live.provider_adapter,
    'tray-offline':partial(uploads.submit,remote=True,active=True),
    'tray-failed':partial(uploads.submit,remote=True,failure=True),
    'tray-cancelled':partial(uploads.submit,remote=True,cancelled=True),
    'tray-proton-busy':blocked('local S3 cannot emit proton_account_busy; provider fault adapter belongs to the builder lane',670),
    'history_window':blocked('history window chrome/geometry belongs to core 2a',675),
    'launcher':lambda scope:Fixture(executable=scope.ctx.target.app_executable),
    'mcp':lambda scope:Fixture(python=scope.ctx.profile.get('mcp_python'),helper=scope.ctx.profile.get('mcp_helper')),
}


class FixtureManager:
    def __init__(self, ctx):
        self.ctx=ctx
        self.scope=Scope(ctx)
        self.errors=[]
        # Outer fallback for setup failure, cancellation or exceptions outside
        # run_recipes. Target.close still quits the owned resident afterwards.
        close=getattr(ctx.target,'close',None)
        if close:
            def close_with_fixtures():
                errors=self.close()
                return errors+list(close() or [])
            ctx.target.close=close_with_fixtures

    def get(self,name,**options):
        if name not in FACTORIES:raise RecipeBlocked('unknown fixture: '+name)
        if name in self.scope.values:
            if options:raise ValueError('fixture options must be supplied on first resolution')
            return self.scope.values[name]
        # Lazy notification descriptor deliberately does not publish in needs.
        try:
            value=FACTORIES[name](self.scope,**options)
            self.scope.values[name]=value
            return value
        except BaseException:
            self.errors.extend(self.scope.close())
            raise

    def close(self):
        errors=self.errors+self.scope.close()
        self.errors=[]
        self.scope=Scope(self.ctx)
        self.ctx._fixtures.clear()
        return errors
