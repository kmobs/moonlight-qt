# Published nightly notes and metadata

`nightly_notes.py` runs only in the gated publish job, after exact build artifact verification and before creating the draft. It appends raw commit messages and exact GitHub commit/compare links to the existing security/build notes, writes `nightly-metadata.json`, and regenerates `SHA256SUMS` including metadata, manifests and packages (excluding the checksum file itself).

The baseline is the most recently **published**, non-draft nightly prerelease returned by the paginated GitHub releases API. Failed builds, retained drafts, stable releases and other prerelease tag families do not advance it. Existing pre-metadata published daily tags can provide a migration baseline by resolving their exact Git commit; new metadata is checked against the immutable tag target. No baseline is saved in the workflow workspace for future runs.

Schema version 1 fields:

| Field | Contract |
| --- | --- |
| `schema_version` | `1` |
| `channel` | `nightly` |
| `tag`, `version` | Immutable tag and complete embedded application version |
| `source_commit` | Exact lowercase 40-character Git SHA |
| `windows_product_version` | Moonlight only: exact `6.2.B`; omitted for hosts. Prior metadata may omit it for migration, but a present value must match `B`. |
| `nightly_sequence` | Integer host UTC date `YYYYMMDD`, or Moonlight build `B` |
| `previous_successful_tag`, `previous_source_commit` | Preceding published nightly identity; null for first nightly |
| `changelog_range` | Exact `previous_source_commit..source_commit`; null for first nightly |
| `compare_url` | Exact-SHA GitHub compare URL; null for first nightly |
| `first_nightly` | No previous successfully published nightly exists |
| `no_source_changes` | A previous baseline exists and its source SHA equals this source SHA |
| `commits_total`, `commits_listed` | Number of newly reachable commits in the exact range; all messages are listed |

Host repositories use `daily-YYYY-MM-DD` and `{base}-daily.dYYYYMMDD+{sha12}`. Moonlight uses `v{app/version.txt}-daily.B` and `{app/version.txt}-daily.B`. The supported app bases are `6.1.0` (historical/transition) and `6.2.0`. Both share one monotonically increasing `B` across all historical VRR and nightly tags, retaining Windows PE/MSI/Burn product version `6.2.B`. For example, `6.1.0-daily.182` upgrades to `6.2.0-daily.183` / Windows `6.2.183`; the sequence must never reset when the app base changes. macOS uses the app base as `CFBundleShortVersionString`, numeric `6.2.B` as `CFBundleVersion`, and the full nightly identity/source as separate plist fields. The Linux AppImage and displayed app version use the full nightly identity.

Bare Windows product version `6.2.0` is older than existing `6.2.B` installations. Publishing a future stable Windows package therefore requires an explicit higher product-version allocation; changing only the displayed base cannot make it upgrade an installed nightly. The allocator fails closed on unrecognized published stable tags. It also blocks returning to the `6.1.0` app family after a `6.2.0` nightly is published. This is a fork version alignment, not a claim of upstream feature parity.

Metadata carries the full source SHA independently of application version display. The updated notes generator and updater accept both nightly families with the same Windows product-version contract. Clients enable prereleases by default with a persistent stable-only opt-out, and must require a non-draft prerelease with its expected packages and this metadata before treating it as available. Clients from daily 182 or earlier do not recognize the new `6.2.0-daily.B` family; they need a manual upgrade or a final `6.1.0` transition build carrying the dual-family updater before moving automatically to `6.2.0`.

First nightly notes explicitly claim no baseline or delta. Same-source nightlies explicitly say no source changes and rebuild. Divergent/rewound history warns that `git log previous..source` lists newly reachable commits, without claiming forward ancestry; an empty rewind delta is distinct from identical sources.

Run focused offline checks with `python -m unittest discover -s scripts/tests -p test_nightly_notes.py -v`. No credential, signing, OS trust, release creation or network access is needed by these tests.
# Unchanged-source policy

Before allocating a build number or starting platform builds, the daily workflow
compares its pinned source SHA with this repository's last successfully published
nightly. Drafts, unpublished releases, stable releases and failed build runs do
not advance that baseline. Matching source skips all platform jobs and publication;
changed source (including changed CI code) qualifies for the next build number.
Release-note generation rechecks the same baseline before creating a draft and
refuses a duplicate source. API or metadata validation failures stop the workflow
instead of assuming a new build is needed.
