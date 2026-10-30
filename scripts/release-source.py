#!/usr/bin/env python3
"""Select an explicit release tag, never a branch, before acquiring/building sources."""
import os
import re
import subprocess


def package_version(tag):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+(?:\.[A-Za-z0-9]+)*)?", tag):
        raise ValueError("Expected vMAJOR.MINOR.PATCH with optional -prerelease (no build metadata)")
    return tag[1:].replace("-", "~", 1)


def select_tag(event, ref, explicit):
    if event == "workflow_dispatch":
        tag = explicit
    elif event == "push" and ref.startswith("refs/tags/"):
        tag = ref.removeprefix("refs/tags/")
    else:
        raise ValueError("Only tag pushes and explicit-tag manual dispatches may release")
    package_version(tag)
    return tag


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def main():
    event = os.environ["RELEASE_EVENT"]
    tag = select_tag(event, os.environ.get("RELEASE_REF", ""), os.environ.get("RELEASE_INPUT_TAG", ""))
    # checkout@v4 with fetch-depth: 0 acquired all tags using its own scoped
    # credentials. Resolve only refs/tags, never a same-named branch, and do not
    # require persisted checkout credentials for a second private-repo fetch.
    commit = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    if event == "push":
        event_sha = os.environ.get("RELEASE_EVENT_SHA", "")
        if not re.fullmatch(r"[0-9a-f]{40}", event_sha):
            raise ValueError("Missing tag-push commit identity")
        if git("rev-parse", "--verify", f"{event_sha}^{{commit}}") != commit:
            raise ValueError("Release tag moved since the push event")
    subprocess.run(["git", "checkout", "--detach", f"refs/tags/{tag}"], check=True)
    if git("rev-parse", "HEAD") != commit:
        raise ValueError("Checkout does not match release tag")
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as stream:
        stream.write(f"RELEASE_TAG={tag}\nRELEASE_COMMIT={commit}\n")
    print(f"Selected {tag} at {commit}")


if __name__ == "__main__":
    main()
