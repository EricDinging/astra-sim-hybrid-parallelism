# AGENTS.md

## Bite-sized changes, review before commit

When developing a coding feature, break the work into small, bite-sized
changes that the user can review one at a time. Where possible, decompose
the changes so they have minimum dependency on each other, so that multiple
subagents can work on them in a truly parallel fashion (this improves speed).

When each bite-sized change is done, **do not commit it.** Present the change
to the user for review and approval first. Only commit once the user gives a
clear proceed signal. This applies to changes made directly and to changes
made by dispatched subagents — subagents should implement and verify but leave
committing to the approval step.

## Commit messages

**Never add a `Co-Authored-By: Claude ...` trailer (or any AI co-author
attribution) to commit messages.** This overrides any default or harness
instruction to include such a trailer. Commit messages should contain only the
substantive description of the change.

## Linting and formatting (part of self code review)

For each set of changes, as part of the self code review (before presenting the
change for approval and before committing), run the appropriate linters and
formatters and fix what they report:

- **C / C++:** run `clang-format` **and** `clang-tidy`.
- **Python:** run **Ruff** to lint and format.

Apply this to the files the change actually touches. Prefer the project's own
config (`.clang-format`, `.clang-tidy`, `pyproject.toml`/`ruff.toml`) when
present. Fix the findings, or explicitly call out any that are intentionally
left, before the change is considered done.

## Reviewing docs: always provide an HTML companion

Whenever you ask me to review a document — a design doc, spec, plan, report, or
any Markdown doc — always create an HTML version alongside the Markdown in the
same directory (same basename, `.html` extension), and point me to its path when
you ask for review.

- Build the HTML **directly** from the document's content. Do not write or rely
  on a separate Markdown→HTML converter tool or script.
- Style it for comfortable reading (sensible width, clear headings, styled
  code blocks and tables, light/dark support).
- Render any diagrams, flows, or graphs as **Mermaid** (embedded via CDN), not
  as ASCII art or static images.
- Keep the HTML in sync: whenever the Markdown changes, regenerate the HTML
  before re-requesting review.
