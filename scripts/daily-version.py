"""Allocate the next MSI build from remote tag history; never reserve a live tag."""
import json
import re
import subprocess
from pathlib import Path


# Only these audited irregular historical tags may bypass version-name parsing.
HISTORICAL_WINDOWS_VERSIONS = {
    release['tag']: release
    for release in json.loads((Path(__file__).parent / 'historical-windows-versions.json').read_text())['releases']
}


def next_version(tags, tag_targets=None, release_base=None):
    if release_base is None:
        release_base = (Path(__file__).parents[1] / 'app/version.txt').read_text().strip()
    if release_base not in ('6.1.0', '6.2.0'):
        raise ValueError('Unsupported nightly app version; explicit Windows migration required')
    maximum = 0
    for tag in tags:
        if tag in HISTORICAL_WINDOWS_VERSIONS:
            history = HISTORICAL_WINDOWS_VERSIONS[tag]
            target = (tag_targets.get(tag) if tag_targets is not None else
                      subprocess.check_output(['git', 'rev-parse', f'{tag}^{{commit}}'], text=True).strip())
            if target != history['tag_commit']:
                raise ValueError(f'Audited historical tag {tag} changed target or has no verified target')
            if history['msi_product_version'] != '6.1.0.0' or history['pe_file_version'] != '6.1.0.0':
                raise ValueError(f'Audited historical tag {tag} requires version migration')
            continue  # Shipped 6.1.0.0 is below every 6.2.B daily build.
        match = re.fullmatch(r"v6\.1\.0-vrr(\d+)(?:\.(\d+))?", tag)
        if match:
            n, patch = match.groups()
            n = int(n)
            build = n * 10 + int(patch) if patch is not None else (n if n <= 17 else n * 10)
            maximum = max(maximum, build)
            continue
        match = re.fullmatch(r"v6\.([12])\.0-daily\.([1-9][0-9]*)", tag)
        if match:
            if match[1] == '2' and release_base == '6.1.0':
                raise ValueError('Nightly app version must not regress from 6.2.0 to 6.1.0')
            maximum = max(maximum, int(match[2]))
            continue
        # A newer stable or unfamiliar custom release needs an explicit migration.
        match = re.match(r"v?(\d+)\.(\d+)\.(\d+)(?:$|-)", tag)
        if match and tuple(map(int, match.groups())) > (6, 1, 0):
            raise ValueError(f"Review Windows version allocation for newer release tag {tag}")
        if match and '-' in tag and tuple(map(int, match.groups())) >= (6, 1, 0):
            raise ValueError(f"Unrecognized Windows release tag {tag}")
    build = maximum + 1
    if build > 65535:
        raise ValueError('Windows MSI build field exhausted; explicit version migration required')
    return f'{release_base}-daily.{build}'


def verify_draft(release, source_sha, tag, remote_tag_sha=None):
    if (release.get('isDraft') is not True or release.get('isPrerelease') is not True
            or release.get('targetCommitish') != source_sha or release.get('tagName') != tag):
        raise ValueError('Draft release state or immutable target changed')
    if remote_tag_sha is not None and remote_tag_sha != source_sha:
        raise ValueError('Existing remote release tag targets a different commit')


def check_vrr_version(ci_version, tags):
    match = re.fullmatch(r'6\.1\.0-vrr(\d+)(?:\.(\d+))?', ci_version)
    if not match:
        raise ValueError('Unsupported VRR version family')
    n, patch = match.groups()
    n = int(n)
    build = n * 10 + int(patch) if patch is not None else (n if n <= 17 else n * 10)
    for tag in tags:
        if tag.startswith(('v6.1.0-daily', 'v6.2.0-daily')):
            daily = re.fullmatch(r'v6\.[12]\.0-daily\.([1-9][0-9]*)', tag)
            if tag.startswith('v6.2.0-daily'):
                raise ValueError('Legacy 6.1.0 VRR app family cannot supersede 6.2.0 nightlies')
            if not daily or int(daily[1]) >= build:
                raise ValueError(f'VRR Windows 6.2.{build} must exceed existing daily tag {tag}')
    if build > 65535:
        raise ValueError('VRR Windows MSI build field exceeds 65535')


if __name__ == '__main__':
    import os
    tags = subprocess.check_output(['git', 'tag', '--list'], text=True).splitlines()
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--check-vrr':
        check_vrr_version(sys.argv[2], tags)
        sys.exit(0)
    version = next_version(tags)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f'ci_version={version}\ntag=v{version}\n')
    print(f'Allocated {version} (Windows 6.2.{version.rsplit(".", 1)[1]})')
