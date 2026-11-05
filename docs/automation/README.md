# Agents, CLI and MCP

Driving put.gd from an agent or a script.

## Guarantees

- Every interactive capability stays controllable through session methods, `putgd-cli` and the MCP helper.
- `putgd-cli` connects to a session socket in an owner-only runtime directory. No TCP listener exists.
- The MCP helper forwards only to the production CLI. It owns no capture or storage pipeline, credentials, model, TCP listener or shell tool.
- Capabilities state actual adapter availability; the helper cannot enable a missing backend.
- CLI and MCP configuration accepts credential references only; neither interface accepts raw keys.

## Documents

- [Working as an agent](agents.md): the source, workflow and acceptance maps.
- [Resident JSON CLI](cli.md): `putgd-cli` methods, parameters and the request envelope.
- [Optional local MCP helper](mcp.md): locked setup, tools and the separate integration suite.
