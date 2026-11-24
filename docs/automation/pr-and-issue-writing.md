# Writing pull requests and issues

Every pull request and issue on put.gd, from any agent or person, follows this rule.

Write PRs and issues in plain English. Give each one a title that describes the behavior
someone would notice. Keep it focused on one problem. For issues, say what happened, what
you expected, and how to reproduce it. For PRs, explain the problem in one or two
sentences, then say what you changed and how you checked it.

## Titles

Name the action and the behavior someone would notice. Use a conventional commit prefix
for PR titles. For issue titles, describe the symptom so someone can recognize it and
search for it.

Good PR titles:

- `fix(chat): keep unsent drafts when switching threads`
- `fix(auth): return users to the page they opened after signing in`
- `feat(settings): let users turn off email notifications`
- `refactor(billing): use one rounding rule for invoice totals`

Good issue titles:

- Switching threads clears an unsent draft
- Signing in from a shared link opens the home page
- Saving notification settings resets the email toggle

Replace broad titles like `fix(chat): improve state handling` or `Draft bug` with the
specific behavior. For internal changes, name the specific rule or operation that changed.

## PR body

```markdown
**Problem.** Switching threads clears the message you were still typing.

**Change.** Save each draft with its thread and restore it when you return.

**Verification.** Typed different drafts in two threads, switched between them, and
confirmed each thread restored its own draft.
```

Required pictures, linked documentation PRs and closing keywords still go in the body,
after these three parts.

## Issue body

```markdown
Open a thread, type a message without sending it, switch to another thread, then return.
The message box is empty. I expected my draft to still be there.
```

Add the stack, logs, seed or input after that when you have them.
