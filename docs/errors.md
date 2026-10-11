# Structured failures and recovery

Every service/CLI failure has `code`, a source-free `message` and bounded `details`.
Ordinary diagnostics contain versions/stages/capability status, never source contents,
filenames, history URLs, authorization values or credential references.

| Code/state | User action |
|---|---|
| `destination_required` | Configure a validated destination explicitly |
| `source_unsupported` | Inspect native support and choose an available source |
| `busy_selecting` | Finish or cancel that interactive selection |
| `queue_full`, `input_excessive` | Finish queued work or adjust documented resource limits |
| `source_unavailable` | Fix the reported item index; invoke again; nothing was published |
| `archive_failed` | Fix the original local folder, then retry local save; upload is independent |
| `artifact_missing`, `artifact_changed` | Restore access or invoke a new action; mutable local copies are not retry sources |
| `provider_unavailable` | Restore the adapter/credential access for the recorded destination |
| `unsupported_capability` | Choose a provider/policy that supports the required safeguards |
| `existence_unknown`, `collision_exhausted`, `precondition_changed` | Inspect the saved candidate and retry deliberately |
| `unknown` upload / `publication_unknown` | Reconcile the saved publication identity before another PUT |
| copy `suppressed` / `failed` | Explicitly copy the existing result; no new PUT is needed |
| `delete_confirmation_changed`, `delete_target_changed` | Review the exact original targets/effects again |
| remote `changed` | Replacement is preserved; no destructive request was sent |
| remote `unknown` / `hidden_version_retained` | Explicitly retry the recorded deletion/progress |
| `history_migration`, `history_version` | Preserve the database and restore a supported version; no partial migration is committed |
| `config_invalid`, `config_missing`, `config_too_large` | Fix the original file; accepted settings remain available |
| `config_busy`, `config_conflict`, `config_unwritable` | Reload/rebase or fix filesystem access; prior bytes remain intact |
| `invalid_json`, `invalid_request`, `unsupported_version`, `wrong_session`, `request_too_large` | Correct the versioned session request; no operation was accepted |
| `session_unavailable` | Start the resident app in the same user session |

Unknown publication/deletion, failed local save and successful remote upload are
independent states. Cancelling a job cannot promise rollback after a network mutation
was sent. Origin deletion does not invalidate browser/CDN caches or remove local files.
