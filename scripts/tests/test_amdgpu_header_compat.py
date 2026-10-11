"""Compile AMDGPU header variants only: no linking, execution or device access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = shutil.which('g++') or shutil.which('clang++')
SYMBOLS = (
    'DRM_IOCTL_AMDGPU_CTX', 'AMDGPU_CTX_OP_ALLOC_CTX',
    'AMDGPU_CTX_OP_FREE_CTX', 'AMDGPU_CTX_PRIORITY_NORMAL',
    'AMDGPU_CTX_OP_SET_STABLE_PSTATE', 'AMDGPU_CTX_OP_GET_STABLE_PSTATE',
    'AMDGPU_CTX_STABLE_PSTATE_PEAK', 'AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK',
)
MODERN_CONTEXT = '''
union drm_amdgpu_ctx {
    struct { unsigned int op, flags, ctx_id; int priority; } in;
    union {
        struct { unsigned int ctx_id; } alloc;
        struct { unsigned int flags; } pstate;
    } out;
};
'''
SOURCE = '''
#include "gpuperformancehold_amdgpu.h"
#ifdef HAVE_AMDGPU_STABLE_PSTATE
static_assert(EXPECT_SUPPORTED, "Incomplete headers incorrectly enable the hold");
void check_context_fields() {
    union drm_amdgpu_ctx args = {};
    args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    args.in.priority = AMDGPU_CTX_PRIORITY_NORMAL;
    const unsigned int context = args.out.alloc.ctx_id;
    args.in.op = AMDGPU_CTX_OP_SET_STABLE_PSTATE;
    args.in.ctx_id = context;
    args.in.flags = AMDGPU_CTX_STABLE_PSTATE_PEAK;
    args.in.op = AMDGPU_CTX_OP_GET_STABLE_PSTATE;
    args.out.pstate.flags &= AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK;
    args.in.op = AMDGPU_CTX_OP_FREE_CTX;
    const unsigned int request = DRM_IOCTL_AMDGPU_CTX;
    (void)request;
}
#else
static_assert(!EXPECT_SUPPORTED, "Complete headers fail to enable the hold");
#endif
'''

@unittest.skipUnless(COMPILER, 'C++ compiler unavailable')
class AmdgpuHeaderCompatibilityTests(unittest.TestCase):
    def compile_header(self, header, expected, linux=True, layout='drm/amdgpu_drm.h', shadow=None):
        with tempfile.TemporaryDirectory(prefix='amdgpu-header-') as directory:
            temp = Path(directory)
            if header is not None:
                path = temp / layout
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(header)
            if shadow is not None:
                (temp / 'drm').mkdir(exist_ok=True)
                (temp / 'drm/amdgpu_drm.h').write_text(shadow)
            (temp / 'fixture.cpp').write_text(SOURCE)
            result = subprocess.run([
                COMPILER, '-std=c++17', '-fsyntax-only', '-nostdinc',
                '-D__linux__=1' if linux else '-U__linux__',
                f'-DEXPECT_SUPPORTED={int(expected)}',
                '-I', str(temp), '-I', str(ROOT / 'app/streaming'),
                str(temp / 'fixture.cpp'),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    def test_absent_header_keeps_hold_disabled(self):
        self.compile_header(None, False)
    def test_old_header_without_stable_pstate_or_output_member(self):
        old = '\n'.join(f'#define {name} 1' for name in SYMBOLS[:4])
        self.compile_header(old + '\nunion drm_amdgpu_ctx { int old_fields; };', False)
    def test_partial_header_missing_each_required_symbol(self):
        for missing in SYMBOLS:
            with self.subTest(missing=missing):
                header = '\n'.join(f'#define {name} 1' for name in SYMBOLS if name != missing)
                self.compile_header(header + '\nunion drm_amdgpu_ctx { int old_fields; };', False)
    def test_modern_header_compiles_all_context_operations(self):
        header = '\n'.join(f'#define {name} 1' for name in SYMBOLS) + MODERN_CONTEXT
        self.compile_header(header, True)
    def test_modern_distribution_and_pkgconfig_header_layouts(self):
        header = '\n'.join(f'#define {name} 1' for name in SYMBOLS) + MODERN_CONTEXT
        for layout in ('libdrm/amdgpu_drm.h', 'amdgpu_drm.h'):
            with self.subTest(layout=layout):
                self.compile_header(header, True, layout=layout)
    def test_incomplete_distribution_headers_keep_hold_disabled(self):
        old = '\n'.join(f'#define {name} 1' for name in SYMBOLS[:4])
        for layout in ('libdrm/amdgpu_drm.h', 'amdgpu_drm.h'):
            with self.subTest(layout=layout):
                self.compile_header(old, False, layout=layout)
    def test_modern_libdrm_is_not_shadowed_by_old_kernel_uapi(self):
        header = '\n'.join(f'#define {name} 1' for name in SYMBOLS) + MODERN_CONTEXT
        old = '\n'.join(f'#define {name} 1' for name in SYMBOLS[:4])
        self.compile_header(header, True, layout='libdrm/amdgpu_drm.h', shadow=old)
    def test_non_linux_keeps_hold_disabled_even_with_modern_header(self):
        header = '\n'.join(f'#define {name} 1' for name in SYMBOLS) + MODERN_CONTEXT
        self.compile_header(header, False, linux=False)

if __name__ == '__main__':
    unittest.main()
