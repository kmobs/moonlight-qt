# Daily Windows prereleases (inactive)

This lane builds unsigned Windows x64 portable and x64/ARM64 Burn installer
packages at 06:47 UTC. It reuses `build-win-mac.yml` with macOS disabled. All
existing Windows application builds, VRR tests, DualSense tests, diagnostic ZIP
checks, packaged replay smoke tests, exact replay fixtures, and packaging must
succeed before publishing. Daily gates additionally check both PE and MSI
versions, the Burn version, and the portable diagnostics/settings marker.
No SignPath action, signing secret, stable release, or latest alias is used.
Existing push/manual release and Linux workflows retain their behavior.

## Activation blockers

Do not enable until the intended source branch is confirmed. `master` is an
explicit proposed source, matching the remote default at implementation time;
the user's intended source remains unresolved. The protected local Deck commit
`a04581d6` was not integrated. If that source is intended, first publish the
reviewed source normally and revise the lane's branch guards and fetch ref.

1. Review/merge the workflow onto remote `master` and confirm Windows daily scope.
2. After confirming source, set `DAILY_RELEASE_SOURCE_BRANCH=master`. Keep
   `DAILY_RELEASE_ENABLED` unset/false and manually dispatch this workflow with
   `publish=false` (the default) to produce artifacts and validate hosted
   x64/ARM64 builds and all tests/packages. Local workflow/version tests alone do not prove those builds.
3. Confirm tag history contains every previously shipped Windows custom version.
   Daily allocation uses the next `6.2.B` above all `v6.1.0-vrrN[.P]` and
   `v6.1.0-daily.B` tags. Existing VRR mappings are unchanged. A future manual VRR
   release must map strictly above the largest daily build; coordinate manual
   release publication with this lane to avoid competing version reservations.
   The reusable Windows workflow checks remote history before the build and again before artifact upload, rejecting a VRR build whose existing mapping is
   equal to or lower than any tagged daily version; it does not remap VRR tags.
   The two irregular historical tags `v6.1.0-vrr` and `v6.1.0-vrr9.1.1`
   are exact exceptions: their downloaded, SHA-256-verified Windows assets both
   have PE and MSI version `6.1.0.0`, below the `6.2.B` sequence. Each exception
   requires the audited exact tag commit. Retargeting either tag blocks allocation.
   [Audit evidence](../scripts/historical-windows-versions.json) records release
   URLs, commit SHAs, all four asset hashes, extracted EXE hashes and versions.
   `vrr9.1.1` republishes unchanged `vrr9.1` Windows assets. The inspected remote
   tag history allocates first daily `6.1.0-daily.181` / Windows `6.2.181`.
   Newer base-version tags, unknown custom tags, or build >65535 block allocation
   and require explicit version migration. Untagged installed custom versions
   must be accounted for before activation.
4. Set repository Actions variable `DAILY_RELEASE_SOURCE_BRANCH=master` only
   after confirmation, then `DAILY_RELEASE_ENABLED=true`. Missing source
   confirmation blocks every run. Missing enabled keeps scheduled runs and every
   publication inactive; manual artifact validation remains possible. Manual
   publication also requires explicitly choosing `publish=true`. Set enabled to
   `false` to stop scheduled publication.
5. Ensure Actions `GITHUB_TOKEN` may write release contents and branch/tag rules
   permit new daily tags. No PAT or signing credentials are needed.

Every run resolves remote master once and passes the immutable SHA to the build.
Publication uses that SHA as the tag target. The serialized daily lane rechecks
remote tags after builds; changed history or an existing release stops the run.
Draft verification requires draft/prerelease flags and the immutable source
target, and checks the dereferenced tag only when that tag exists. Draft creation
may defer tag creation until publication. It uploads only the verified Burn EXE containing both architectures, x64 portable ZIP, nightly metadata and
SHA256SUMS, downloads them to verify hashes, then publishes with `--latest=false`.
A failed upload leaves a draft for review. A rerun never replaces existing assets;
start a fresh run to allocate a new build after resolving a failed draft.
GitHub schedules are best effort and can run later than their cron time.

Local safeguard test:

```powershell
python -m unittest discover -s scripts/tests -p test_daily_release.py -v
```

Daily publication remains disabled. The isolated validation candidate may be
pushed to `codex/daily-prerelease-safety` for artifact-only hosted builds; this
does not integrate it into master or publish a release. Full combined application
build/packaging validation and source approval remain activation requirements.
