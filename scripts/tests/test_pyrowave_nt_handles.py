"""Guard the actual Windows interoperability probe against unused KMT gates."""
from pathlib import Path
import re
import unittest

SOURCE = (Path(__file__).resolve().parents[2] /
          'pyrowave/pyrowave/pyrowave_c.cpp').read_text(encoding='utf-8')
MEMORY = SOURCE.split('static bool pyrowave_device_confirm_external_memory_support(', 1)[1]
MEMORY = MEMORY.split('bool pyrowave_device_confirm_interop_support(', 1)[0]
ARRAY = re.search(r'required_external_types\[\]\s*=\s*\{(.*?)\};', MEMORY, re.S).group(1)
WINDOWS, UNIX = ARRAY.split('#else', 1)
WINDOWS_TYPES = set(re.findall(r'VK_EXTERNAL_MEMORY_HANDLE_TYPE_\w+', WINDOWS))
UNIX_TYPES = set(re.findall(r'VK_EXTERNAL_MEMORY_HANDLE_TYPE_\w+', UNIX))
NT_TYPES = {
    'VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT',
    'VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT',
    'VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT',
}


class WindowsInteropRequirements(unittest.TestCase):
    def test_decoder_and_calibration_keep_adapter_selection_and_use_scoped_probe(self):
        root = Path(__file__).resolve().parents[2] / 'app/streaming/video/pyrowave'
        decoder = (root / 'pyrowavedecoder.cpp').read_text(encoding='utf-8')
        calibration = (root / 'pyrowavecalibrator.cpp').read_text(encoding='utf-8')
        self.assertIn('reinterpret_cast<const pyrowave_luid*>(luid)', decoder)
        self.assertIn('reinterpret_cast<const pyrowave_luid*>(&description.AdapterLuid)', calibration)
        self.assertIn('pyrowave_create_device_by_compat', decoder)
        self.assertIn('pyrowave_create_device_by_compat', calibration)
        self.assertIn('config.tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM', decoder)
        self.assertIn('pyrowave_device_confirm_d3d11_interop_support(impl->device', decoder)
        self.assertEqual(calibration.count('pyrowave_device_confirm_d3d11_interop_support(decodeDevice'), 2)
        self.assertNotIn('pyrowave_device_confirm_interop_support(decodeDevice)', calibration)
        self.assertNotIn('pyrowave_device_confirm_interop_support(impl->device)', decoder)

    def test_scoped_wrapper_rejects_missing_device_and_query_functions(self):
        wrapper = SOURCE.split('bool pyrowave_device_confirm_d3d11_interop_support(', 1)[1]
        wrapper = wrapper.split('bool pyrowave_device_confirm_interop_support(', 1)[0]
        self.assertIn('if (!device)', wrapper)
        self.assertIn('!vkGetPhysicalDeviceExternalSemaphoreProperties', wrapper)
        self.assertIn('!vkGetPhysicalDeviceImageFormatProperties2', wrapper)
        self.assertIn('device->device.get_device_features().supports_external', wrapper)

    def test_actual_probe_requires_only_used_nt_memory_handles(self):
        self.assertEqual(WINDOWS_TYPES, NT_TYPES)

    def test_nt_only_driver_is_not_rejected_for_unused_kmt_handles(self):
        # Arc's reported capability shape: all used NT imports, no KMT imports.
        supported = NT_TYPES
        self.assertTrue(WINDOWS_TYPES <= supported)

    def test_every_used_nt_import_remains_mandatory(self):
        for missing in NT_TYPES:
            with self.subTest(missing=missing):
                self.assertFalse(WINDOWS_TYPES <= NT_TYPES - {missing})
        self.assertIn('for (auto type : required_external_types)', MEMORY)
        self.assertRegex(MEMORY, r'if \(!\(external_props\.externalMemoryProperties\.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT\)\)\s*return false;')

    def test_linux_fd_and_dma_buf_requirements_are_preserved(self):
        self.assertEqual(UNIX_TYPES, {
            'VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT',
            'VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT',
        })

    def test_required_semaphore_import_checks_are_preserved(self):
        semaphore = SOURCE.split('static bool pyrowave_device_confirm_external_semaphore_support(', 1)[1]
        semaphore = semaphore.split('static bool pyrowave_device_confirm_external_memory_support(', 1)[0]
        for token in ('VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT',
                      'VK_SEMAPHORE_TYPE_BINARY', 'VK_SEMAPHORE_TYPE_TIMELINE',
                      'VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT'):
            self.assertIn(token, semaphore)
        self.assertEqual(len(re.findall(r'if \(!\(sem_props\.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT\)\)\s*return false;', semaphore)), 3)


if __name__ == '__main__':
    unittest.main()
