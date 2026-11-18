# Sol review loop

Every pull request gets an independent code review by OpenAI's Sol before it merges, and the
result goes on the pull request as a comment. CodeRabbit and cubic are rate-limited and often
skip or stall, so they are optional extras; this loop is the required review. The `sol-loop`
skill (Claude Code, `~/.claude/skills/sol-loop/`) runs this document.

The loop makes the change more correct with the fewest edits. Watch for scope creep and push
back on findings that are wrong.

## Who runs it

- The session that will merge the pull request runs it: in a coordinated campaign, the
  coordinator, before each merge. A top-level session that owns its own pull request runs it
  before asking for a merge. A delegated worker does not start reviewers unless its brief says
  to; it hands back and the coordinator runs the loop.
- One Sol reviewer per pull request per round by default. Sol must not start subagents or
  other reviewers.
- Run in parallel when it saves time and you judge it worth the cost:
  - **Different pull requests:** review several at once, each with its own loop, ledger,
    worktree and comment. Pull requests that touch the same files are merged one at a time,
    and each later one gets a short round on its rebased diff.
  - **One large or risky pull request:** up to three reviewers in the same round, each with a
    separate focus written into its brief (for example correctness, security and credentials,
    concurrency and recovery). Merge their findings into one ledger with distinct ids, and
    count the round once against the budget. Later rounds go back to one reviewer unless a
    focus still has open findings.

## Budget

- Three rounds by default. Round 1 reviews the whole change. Later rounds check only the
  code changed since the last round and the findings still open.
- Up to five rounds only for security, permissions, credentials, data loss, or
  state/concurrency, while a blocker or should stays open with evidence, or when Patrick asks.
- Stop early when a round returns only nits. A declined nit needs no new round. Every fix,
  documentation included, is reviewed before approval: a small fix gets a short round on just
  the lines it changed.
- Keep a ledger: id, severity, file:line, claim, outcome, and usage if the tool reports it.
  Never guess usage.

## 1. Scope

1. `git fetch origin`, then
   `gh pr view <n> --json baseRefName,headRefName,headRefOid,title,body,files`.
2. Make a worktree of your own at the PR head on a local branch, outside the shared checkout,
   and work inside it from then on:

   ```sh
   git worktree add -b review/pr-<n> /home/patrick/work/put.gd-review-<n> <headRefOid>
   cd /home/patrick/work/put.gd-review-<n>
   git rev-parse HEAD   # must print <headRefOid>
   ```

   Never review or fix in `/home/patrick/work/put.gd`; other sessions switch it. Fixes are
   pushed with `git push origin HEAD:<headRefName>`.
3. Base: `git merge-base HEAD origin/<baseRefName>`. Use the same diff command in every round:
   `git diff <base>..<head>`.
4. State the intent and what is out of scope in one or two sentences, from the PR body and
   its linked issue.
5. Note the checks: the PR's CI result at this head (`gh pr checks <n>`) and any contract
   groups or tests already run. Don't rerun heavy suites on the workstation; builds and ctest
   run on build servers.

## 2. Start Sol

1. Pick the newest Sol model from an `orchestrator_capabilities` result taken in this thread,
   never from a remembered or hardcoded id: a newer Sol can appear at any time. T3 allows one
   capability lookup per thread, and another only after an error or when Patrick names a
   different provider; take the thread's lookup before its first loop. When Patrick names a
   newer Sol model, use the id he gave; if `delegate_task` rejects it, that error permits a
   fresh lookup. A long-running thread can therefore lag a release; the model named in each
   findings comment shows which one reviewed. Keep the chosen model for every round of the
   loop. Use `reasoningEffort: high`. Record provider, model and effort.
2. Every round is a new `delegate_task` with `role: "review"`, `mode: "async"`,
   `isolation: "worktree"`, a new `clientRequestId` and `target: {providerInstanceId: "codex",
   model: <Sol>, options: {reasoningEffort: "high"}}`. Save the `taskId`. T3 prepares the
   reviewer's checkout, but from the caller's commit, which may be stale, so the brief has the
   reviewer fetch and move to the reviewed head and check it. Never prepare the reviewer's
   checkout in the shell, and never use `t3_thread_send`, `create_threads` or
   `t3_thread_launch` for a review round.
3. Don't edit the scoped files while a round runs. Wait for the completion notice; don't poll,
   and don't start another reviewer beyond the parallel reviews allowed under "Who runs it".
   When a round has several reviewers, wait for all of them before merging their findings,
   editing the scoped files or declaring approval. A timeout, error or empty reply is not approval: cancel
   it, check it stopped, and restart with the ledger.

Round 1 brief (a delegated child has no history):

```
Review only. Do not edit, commit, push, comment on GitHub, or start subagents.
PR: <url>  Base: <sha>  Head: <sha>
First, in your own checkout: git fetch origin && git switch --detach <head> && git rev-parse HEAD
(it must print <head>; stop and say so if it doesn't). Touch no other checkout.
Intent: <intent>  Out of scope: <scope>  Checks: <results>
See: git diff <base>..<head>; read AGENTS.md and the docs the change touches.
Read the surrounding code. Find bugs, broken callers, and mismatches with the intent and with
put.gd's documented design (docs/design/). Skip style unless it hides a bug. Hold to the
smallest change: flag code, tests or guards the intent does not need. Judge severity by
impact, evidence and scope, not by how often a case happens: a rare path that leaks a
credential, loses data or corrupts state is still a blocker. Other paths with minor impact,
small performance costs and extra compatibility are nits.
blocker = the intended case is wrong, or any security, credential, data-loss or corruption bug
the change introduces. should = a real bug callers will hit, in scope. nit = the rest.
Reply in plain English:
VERDICT: APPROVE or CHANGES
S1 [blocker|should|nit] <file>:<line> <claim>. Fix: <suggestion>   (stable ids across rounds)
```

## 3. Answer the findings

Check each finding and its severity yourself, by impact, evidence and scope. Another path or
a small cost with minor impact is a nit whatever label it came with. A rare case that leaks a
credential, loses data or corrupts state keeps its severity.

- **Agree:** make the smallest fix. Prefer deleting or narrowing code over a new lock, cache,
  helper or option. Fix the case that happened. Add one focused test that fails without the
  fix when the bug would be silent and costly (`docs/testing/README.md`).
- **Disagree:** say why, with evidence (file:line, a test, output).
- **Out of scope or nit:** decline in one line. A real bug outside the change becomes a
  GitHub issue, and whoever files it fixes it.

Commit fixes on the PR branch in the worktree, run the checks the fix needs (contract groups
per AGENTS.md, on a build server), and keep documentation fixes in the separate docs pull
request. Check `git diff --shortstat <base>..HEAD`; if review grew the change, cut first. Don't
agree just to end the loop.

## 4. Next round

Start one only if budget remains and a blocker or should is open, behaviour code changed, or
the approval doesn't cover the current diff. Send the round 1 brief with the new head, the
ledger (`fixed: <id> in <sha>`, `disagree: <id> <evidence>`, `declined: <id> <reason>`), and the
change since the last round. Tell Sol to look only at open ids and changed code and to mark
each old id `resolved` or `still open: <reason>`.

## 5. Stop, comment and merge

- **Agreed:** APPROVE on the current diff, no blocker or should open, and no concern of your
  own.
- **Stuck:** an item stays disputed after two replies, or the budget ends. Budget end is not
  approval. Show both sides in two or three lines each; Patrick decides.

Push fixes to the PR branch (`git push origin HEAD:<headRefName>`) and wait for CI. In T3, call
`watch_pull_request` and end the turn; the watch wakes you (no sleep, loops or `gh --watch`).
Outside T3, use `gh run watch <id> --interval 60 --exit-status`. Before merging, check that
the required `CI` summary passed at the reviewed head (`gh pr checks <n>`), then
`unwatch_pull_request`.

Always post one comment on the pull request when the loop ends, approved or not, with
`gh pr comment <n> --body-file <file>`. Write it in plain English
([PR and issue writing](pr-and-issue-writing.md)) and starting with the attribution note from
[Leaving comments on GitHub](github-comments.md). `<writer model>` is the model posting the
comment (usually the coordinator, not Sol); `<Sol model>` is the reviewer:

```markdown
> [!NOTE]
> 🤖 **<writer model> responding on behalf of Patrick Hudson**

**Sol review: approved** (or: **changes still open** / **disputed, needs a decision**)

Reviewed head `<sha>` against `<base>` in <N> rounds with <Sol model> on <effort>.

| ID | Severity | Where | Finding | Outcome |
|---|---|---|---|---|
| S1 | should | `src/app/X.cpp:120` | A cancelled retry marks an unknown upload failed. | Fixed in `<sha>`; added a test that fails without it. |
| S2 | nit | `tools/lab/y.py:40` | Unused import. | Declined: outside this change. |

**Checks.** CI passed at `<sha>`; lab and release contract groups passed on k72.

**Follow-ups.** #1234 (filed for S4, outside this change).
```

If no findings came back, say so in one line under the heading. If the loop is stuck, give
both sides of each disputed item.

A pull request merges only when its latest Sol review comment says approved for its current
head. A push after that comment starts a new loop: pick the model again as in step 2.1, review
the new commits, and post a new comment.
Docs-only pull requests start with a single round, plus a short round on any fix.

Remove the review worktree when the pull request is merged or closed.
