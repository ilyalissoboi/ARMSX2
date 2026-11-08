# Fork release notes

One file per release, named after its version: `2.9.2-fork.1.md`. It holds the part of the
release notes that only a person can write, and it has to be on master in the commit you
tag. [fork-release.yml](../workflows/fork-release.yml) adds the rest.

Write, in this order:

1. A one-paragraph intro: "A fork of ARMSX2 based on upstream X.Y.Z (upstream commit
   abcdef1234), with … and, new in this release, …"
2. `**What's new in this fork:**`, with an italic heading per feature and bullets under
   it, then `_Carried over from earlier fork releases:_` linking the releases that
   introduced each earlier feature.
3. `**From upstream:**` either "no change since <previous fork release>. This is still
   upstream …" or what the upstream merge brought in, linking upstream's release notes.

The workflow appends these sections, so don't write them yourself:

- **Not in this build:** Discord
- **Which APK to install:** (the four tiers)
- **Signing note:**
- the SHA-256 table
- the `Source:` line with the tag and commit

If the file is missing when the tag is pushed, the release stays a draft. Its notes then
start with GitHub's list of pull requests merged since the previous fork release.
