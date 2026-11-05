# put.gd documentation

Find what you are trying to do. Each folder's `README.md` lists its standing
guarantees and every document in it.

| You want to | Go to |
| --- | --- |
| Use the app | [user/](user/README.md): user guide, troubleshooting, failure codes, S3, B2 and Proton Drive destinations |
| Understand the design | [design/](design/README.md): start with [overview](design/overview.md) and [architecture](design/architecture.md) |
| Look up a desktop's behavior | [desktops/](desktops/README.md): Cinnamon, GNOME and Plasma adapters |
| Drive it from an agent or the CLI | [automation/](automation/README.md): agent maps, `putgd-cli` and the MCP helper |
| Build or release it | [release/](release/README.md): build instructions, versions, CI, channels and the release runbook |
| Test and accept it | [testing/](testing/README.md): test suites, acceptance matrix, recorded acceptance and evidence rules |
| Run the lab | [lab/](lab/README.md): physical builders, VMs, goldens and Boat |
| Read retired designs and plans | [history/](history/README.md) |

Standing project rules are in [AGENTS.md](../AGENTS.md). README artwork and its
sources are in [readme/sources.md](readme/sources.md).

Evidence records live in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence), not in this
tree; see [evidence retention](testing/evidence-retention.md). If you have an old
path from an evidence record or an earlier commit, the
[layout change](history/2026-11-docs-layout.md) maps it to the current one.

Six old paths stay as short stubs. Shipped software links to
[package-repositories.md](package-repositories.md),
[verification.md](verification.md) and [release.md](release.md); the put.gd
website links to [cli.md](cli.md), [mcp.md](mcp.md) and
[user-guide.md](user-guide.md).
