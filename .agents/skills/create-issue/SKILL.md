---
name: create-issue
description: Create Gitea/Forgejo issues — interactive or with flags for title, description, labels, assignees, milestone, and deadline.
user-invokable: true
---

# Create Issue

Use the available `gt` or `tea` CLI to create a Gitea/Forgejo issue. Consult the installed CLI's help rather than assuming command syntax.

1. Confirm the target host, repository and account; ask if ambiguous.
2. Collect the title and description, plus any requested labels, assignees, milestone and deadline. Do not invent optional metadata.
3. Create the issue using `gt` or `tea`, explicitly targeting the known host, repository and necessary account.
4. Verify the created issue and requested metadata, then return its URL. If creation reports a partial failure, check whether the issue already exists before retrying to avoid duplicates.
