# AI agent access (MCP)

AeroCAD can be driven by an AI agent such as Claude through the
[Model Context Protocol](https://modelcontextprotocol.io). The agent runs the same
commands as the user interface, on the project open in the AeroCAD window. Undoable
project edits enter the undo history; file writes, deletions and external programs do not. AeroCAD has no built-in chat: you
talk to the agent in its own app (Claude Desktop, Claude Code, ...).

```
Claude Desktop / Claude Code
      |  stdio (MCP, JSON-RPC)
aerocad-mcp                     bundled MCP server, started by the MCP client
      |  local socket (current OS user only)
AeroCAD                         runs the commands on the UI thread, logs every call
```

## 1. Turn on access in AeroCAD

Click **TOOLS > Agent** in the toolbar (or *Edit > AI Agent Access...*) and switch on
**Allow AI agents to use this project**. The panel names the project receiving access.
Consent lasts only for that open project in this application session. **New**, a successful
**Open**, and recovery turn access off and disconnect every agent, including when reopening
the same path. A failed Open leaves the current project's grant intact. Save As updates the
displayed name without ending the grant; switching variants stays within the same project.

Access starts **off on every launch**; the old remembered global setting is discarded.
`AeroCAD --mcp [project.aerocad]` explicitly grants access to the project loaded by that
invocation. A subsequent recovery or project replacement still requires fresh consent.

The panel shows the connected agents, every operation they perform (tool, outcome,
duration; hover for the project, declared effects and parameters) and a **Disconnect** button.
**Disconnect** turns access off and disconnects every agent, just like switching access off.
Access events remain in the activity log with the original project name. Buffered requests
from disconnected clients are discarded immediately and interrupted calls are never replayed.

After New, Open, recovery or Disconnect, switch **Allow AI agents to use this project** on
again when you want to grant access to the displayed project. The same running MCP server
can then connect with a fresh grant on the next tool call; **no MCP server restart is needed**.
While access is off, calls fail with instructions to ask you to enable it. A previous grant
cannot authorize a command in the new project, even if the agent missed the disconnect event.

The panel also shows the exact configuration for your installation (path of
`aerocad-mcp`) with copy buttons.

## 2. Connect Claude

`aerocad-mcp` is built next to the application: `build/<preset>/bin/aerocad-mcp`, and on
macOS also inside the bundle at `AeroCAD.app/Contents/MacOS/aerocad-mcp`.

### Claude Code

```sh
claude mcp add --scope user aerocad -- /path/to/aerocad-mcp
```

Then start `claude` and check with `/mcp` that `aerocad` is connected.

### Claude Desktop

Settings > Developer > Edit Config opens `claude_desktop_config.json`
(macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`). Add:

```json
{
  "mcpServers": {
    "aerocad": {
      "command": "/Users/you/AeroCAD/build/release/bin/AeroCAD.app/Contents/MacOS/aerocad-mcp"
    }
  }
}
```

Restart Claude Desktop. The AeroCAD tools appear in the tools menu of a new chat.

## 3. Try it

With AeroCAD running and access on, ask for example:

- "Import /Users/you/models/bracket.stl into AeroCAD and tell me its size and volume."
- "Scale the part so it is 120 mm long and check the result."
- "How far is the point (10, 0, 50) from the part? What is the clearance between part 1 and part 2?"
- "Undo the last change."

If AeroCAD is not running or access is off, the tools return a message telling the agent
to ask you to turn it on; the call can simply be repeated afterwards.

## Tools

Every command of the command layer is a tool; the name is the command name with `.`
replaced by `_` (`document.import` -> `document_import`). New commands (CAD tools, export,
...) become tools automatically. All lengths are millimetres, Z is up.

| Tool | What it does |
|---|---|
| `project_get_context` | Units, axes, document revision, parts (id, name, bounds, revision), undo state. The agent calls it first. |
| `geometry_measure` | `distance` between points, `part` (bounds, area, volume, closed?), `point_to_part`, `clearance` between two parts. Returns the method and data-quality warnings. |
| `document_import`, `document_parts`, `document_bounds` | Import STL/OBJ/3MF/STEP/IGES/BREP, list parts, bounds. |
| `part_scale`, `part_rename`, `part_set_visible`, `part_remove` | Part edits (undoable). |
| `edit_undo`, `edit_redo`, `edit_history` | Undo history. |

Tool annotations say what a tool does outside the document, so clients can decide what to
auto-approve. They come from each command's effects (`src/commands/src/CommandEffects.cpp`):

- `readOnlyHint`: nothing changes, neither the document nor files or programs on the computer
  (e.g. `project_get_context`, `simulation_results`, `simulation_environment`).
- `destructiveHint`: data may be lost: files are replaced or deleted (`project_save`,
  `document_export`, `document_export_for_print`, `simulation_delete`), the document is
  replaced and the undo history cleared (`project_new`, `project_open`), or `part_remove`.
- `openWorldHint`: the tool may reach other machines or the internet (simulation tools with a
  remote engine, `simulation_install_engine` downloads).

Descriptions add the same in words ("Writes files ...; edit_undo does not reverse this"), and
only undoable document changes promise `edit_undo`.

## Security

- The socket is a Unix domain socket at `~/.aerocad/mcp.sock` in a directory only you can
  enter (mode 0700; the socket itself is user-only too). On Windows it is the named pipe
  `aerocad-mcp-<user>`, restricted to the current user. `AEROCAD_MCP_SOCKET` overrides
  the location for both the app and `aerocad-mcp`.
- Access is scoped to the current project and application session, starts off, and is never
  saved as a preference. Only one AeroCAD window serves agents at a time. Local IPC protocol
  2 binds every connection and command to the explicitly enabled access session. Reconnection
  uses a fresh handshake and never replays commands from a previous connection. Update the
  bundled app and MCP server together when changing protocol versions.
- Consent permits the exposed commands, including filesystem and process effects; it is
  not a filesystem sandbox or an approval prompt for every operation. Undo reverses only
  commands marked as undoable project edits.
- File paths given to `document_import` are read with your permissions, like a file you
  open yourself.

## Without the GUI

`aerocad-mcp --headless` runs the commands in an in-process document instead of the app
(no window). It is used by the automated tests and is handy for trying the protocol, e.g.
with the MCP Inspector: `npx @modelcontextprotocol/inspector /path/to/aerocad-mcp --headless`.

## Not yet done

- Agent changes are individual undo steps; grouping a multi-step agent task into one
  accept/reject step with a base-revision check (CAD-45, CAD-46) comes next.
- Region selection, CAD operations and export are exposed as soon as their commands land.
