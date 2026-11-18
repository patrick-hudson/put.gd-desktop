# Leaving comments on GitHub

Every comment an agent leaves on GitHub for Patrick starts with an attribution note. That
covers top-level pull request and issue comments, inline review comments, replies to bots or
reviewers, and the explanation written before resolving or dismissing feedback. It does not
apply to pull request or issue descriptions, commit messages, or text Patrick asked to have
posted word for word without attribution. The `leaving-pr-comments` skill (Claude Code,
`~/.claude/skills/leaving-pr-comments/`) runs this document.

## Format

```markdown
> [!NOTE]
> 🤖 **<model> responding on behalf of Patrick Hudson**

Actual comment starts here.
```

- `<model>` is the verified public name of the model that wrote the comment, for example
  Claude Opus 5.5 or GPT-6.1 Sol. If the harness doesn't expose the model name, use the
  harness name. Don't guess.
- The attribution stays inside the note and the reply goes below it. The note's line starts
  with the robot emoji.
- No horizontal rule, Setext underline, heading, signature footer or second attribution.

## Writing the comment

- Lead with the conclusion.
- Cite the code, behaviour, check or reproduction it rests on (file:line, test, command output,
  commit).
- Keep it short, with no filler, canned thanks or status narration.
- Keep verified facts apart from judgment.
- Plain English, as in [PR and issue writing](pr-and-issue-writing.md).

## Before and after posting

1. Check the whole comment, including quoted text and attachments, for secrets and private
   details: credentials, tokens, private hostnames and addresses, email addresses, file
   contents from test accounts.
2. Write it to a file and preview the Markdown, so quoting and blank lines render as intended.
   Post from that file, never an inline `--body` with shell quoting: `gh pr comment` and
   `gh issue comment` take `--body-file <file>`, and inline review comments and replies go
   through `gh api` with `-F body=@<file>` (or `--input <json-file>`).
3. Check that GitHub returned the comment or review URL.
4. Resolve a review thread only after its reply was posted successfully.
