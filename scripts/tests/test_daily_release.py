import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('daily_version', ROOT / 'scripts/daily-version.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class DailyVersionTests(unittest.TestCase):
    def allocate(self, tags, targets=None):
        return module.next_version(tags, targets, release_base='6.2.0')
    def test_monotonic_mixed_history(self):
        self.assertEqual(self.allocate(['v6.1.0-vrr17', 'v6.1.0-vrr17.1', 'v6.1.0-vrr18']), '6.2.0-daily.181')
        self.assertEqual(self.allocate(['v6.1.0-daily.181', 'v6.1.0-vrr18.2']), '6.2.0-daily.183')
        self.assertEqual(self.allocate(['v6.1.0-daily.999']), '6.2.0-daily.1000')
        self.assertEqual(self.allocate(['v6.1.0-daily.182']), '6.2.0-daily.183')
        self.assertEqual(self.allocate(['v6.1.0-daily.182', 'v6.2.0-daily.183']), '6.2.0-daily.184')
    def test_family_migration_cannot_reset_sequence_or_regress_app_base(self):
        with self.assertRaisesRegex(ValueError, 'must not regress'):
            module.next_version(['v6.2.0-daily.183'], release_base='6.1.0')
        with self.assertRaises(ValueError):
            module.next_version([], release_base='6.3.0')
    def test_boundary_and_migration_fail_closed(self):
        self.assertEqual(self.allocate(['v6.1.0-daily.65534']), '6.2.0-daily.65535')
        for tags in (['v6.1.0-daily.65535'], ['v6.2.0-daily.65535'], ['v6.3.0'], ['v6.1.0-vrr18-bad'], ['v6.1.0-daily.bad'], ['v6.2.0-daily.bad'], ['v6.1.0-rc1'], ['v6.2.0']):
            with self.subTest(tags=tags), self.assertRaises(ValueError):
                self.allocate(tags)
    def test_audited_legacy_tags_require_exact_commits(self):
        tags = list(module.HISTORICAL_WINDOWS_VERSIONS) + ['v6.1.0-vrr18']
        targets = {tag: entry['tag_commit'] for tag, entry in module.HISTORICAL_WINDOWS_VERSIONS.items()}
        self.assertEqual(self.allocate(tags, targets), '6.2.0-daily.181')
        for tag in module.HISTORICAL_WINDOWS_VERSIONS:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                self.allocate(tags, dict(targets, **{tag: 'f' * 40}))
            with self.subTest(missing=tag), self.assertRaises(ValueError):
                self.allocate([tag], {})
        with self.assertRaises(ValueError):
            self.allocate(['v6.1.0-vrr9.1.2'], targets)
    def test_actual_repository_history_first_daily(self):
        tags = subprocess.check_output(['git', '-C', str(ROOT), 'tag', '--list'], text=True).splitlines()
        if not all(tag in tags for tag in module.HISTORICAL_WINDOWS_VERSIONS):
            self.skipTest('Audited repository historical tags not available')
        targets = {tag: subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', f'{tag}^{{commit}}'], text=True).strip() for tag in module.HISTORICAL_WINDOWS_VERSIONS}
        import json
        audited = json.loads((ROOT / 'scripts/historical-windows-versions.json').read_text())['audited_tag_history']
        if not set(audited).issubset(tags):
            self.skipTest('Full audited tag history not available')
        self.assertEqual(module.next_version(audited, targets, release_base='6.1.0'), '6.1.0-daily.181')
        self.assertEqual(self.allocate(audited, targets), '6.2.0-daily.181')
    def test_draft_without_tag_is_valid_but_wrong_state_or_target_is_blocked(self):
        release = dict(isDraft=True, isPrerelease=True, targetCommitish='a' * 40, tagName='v6.1.0-daily.181')
        module.verify_draft(release, 'a' * 40, release['tagName'])
        module.verify_draft(release, 'a' * 40, release['tagName'], 'a' * 40)
        for key, value in [('isDraft', False), ('isPrerelease', False), ('targetCommitish', 'master'), ('tagName', 'stable')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                module.verify_draft(dict(release, **{key: value}), 'a' * 40, release['tagName'])
        with self.assertRaises(ValueError):
            module.verify_draft(release, 'a' * 40, release['tagName'], 'b' * 40)
        workflow = (ROOT / '.github/workflows/daily-prerelease.yml').read_text(encoding='utf-8-sig')
        self.assertIn('module.verify_draft(', workflow)
        self.assertNotIn('test "$(git rev-list', workflow)
    def test_future_vrr_cannot_reuse_or_downgrade_daily_windows_version(self):
        module.check_vrr_version('6.1.0-vrr19', ['v6.1.0-daily.189'])
        for tag in ('v6.1.0-daily.190', 'v6.1.0-daily.191', 'v6.1.0-daily.bad', 'v6.2.0-daily.183'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                module.check_vrr_version('6.1.0-vrr19', [tag])
    @unittest.skipUnless(__import__('shutil').which('pwsh') or __import__('shutil').which('powershell'), 'PowerShell unavailable')
    def test_pe_mapping_preserves_vrr_and_enforces_daily_limits(self):
        import shutil
        shell = shutil.which('pwsh') or shutil.which('powershell')
        cases = {'6.1.0-vrr17': '6.2.17', '6.1.0-vrr17.1': '6.2.171', '6.1.0-vrr18': '6.2.180', '6.1.0-vrr18.2': '6.2.182', '6.1.0-daily.183': '6.2.183', '6.1.0-daily.65535': '6.2.65535', '6.2.0-daily.183': '6.2.183', '6.2.0-daily.65535': '6.2.65535'}
        for ci, expected in cases.items():
            result = subprocess.run([shell, '-NoProfile', '-File', str(ROOT / 'scripts/vrr-pe-version.ps1'), '-CiVersion', ci], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)
        for ci in ('6.1.0-daily.65536', '6.1.0-daily.0', '6.2.0-daily.65536', '6.2.0-daily.0', '6.3.0-daily.183', 'not-a-version'):
            result = subprocess.run([shell, '-NoProfile', '-File', str(ROOT / 'scripts/vrr-pe-version.ps1'), '-CiVersion', ci], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)

if __name__ == '__main__':
    unittest.main()
