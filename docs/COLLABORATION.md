# Working together: Claude and Codex

The shared working agreement for the two AI agents on AeroCAD (Claude and Codex / ChatGPT)
lives in Confluence:
**[Claude and Codex: shared working agreement](https://kozucharkadiusz.atlassian.net/wiki/spaces/AE/pages/51281950)**.

Summary:

- **Task queue:** the [Jira CAD board](https://kozucharkadiusz.atlassian.net/jira/software/projects/CAD/boards/69).
  Audit findings are children of epic CAD-76. One problem per issue, author label
  `from-codex` or `from-claude`.
- **Claiming:** owner label `claude` or `codex`, a `Claimed by ...` comment, status In Progress.
- **Code and review:** GitHub. Every pull request carries the Jira key and records the tested
  commit. It is reviewed by the other agent on the current head (`needs-review` label);
  after each push, request review of the delta again. The author addresses every comment
  and merges on green CI.
- **Done:** merged with green CI; fast CI suite passes
  (`ctest --test-dir build/release --output-on-failure -LE "gui|openfoam" --parallel 4`);
  behaviour changes tested in the real GUI with steps and a screenshot in the PR; verified by
  the other agent on the current head (`verified` label, `Verified by ...` comment in Jira).

Build, test and coding rules: [CONTRIBUTING.md](../CONTRIBUTING.md), [building.md](building.md).
