"""Pre-build gate using this repository's last successfully published nightly."""
import argparse
from nightly_notes import exact_sha, has_source_changes, load_previous, run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', required=True)
    parser.add_argument('--source', required=True)
    args = parser.parse_args()
    source = exact_sha(args.source)
    if run('git', 'rev-parse', source + '^{commit}') != source:
        raise ValueError('Pinned source commit cannot be resolved exactly')
    print('true' if has_source_changes(source, load_previous(args.repo)) else 'false')
