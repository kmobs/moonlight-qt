"""Exercise real MSI Property databases without running an installer."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which('pwsh') or shutil.which('powershell')

@unittest.skipUnless(os.name == 'nt' and POWERSHELL, 'Windows Installer COM requires Windows')
class WindowsProductVersionTests(unittest.TestCase):
    def test_readonly_msi_versions_and_rejected_mismatches(self):
        with tempfile.TemporaryDirectory(prefix='moonlight-msi-version-') as directory:
            result = subprocess.run([
                POWERSHELL, '-NoProfile', '-File',
                str(ROOT / 'scripts/tests/check_windows_product_version.ps1'),
                '-FixtureDirectory', directory,
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('Verified MSI ProductVersion'), 8)

if __name__ == '__main__':
    unittest.main()
