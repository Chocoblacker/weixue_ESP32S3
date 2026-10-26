"""Static release contracts, not hardware or live API validation."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TranslatorRelease(unittest.TestCase):
    def test_no_prompter_dependencies(self):
        for name in ('muse_ui.c', 'muse_ui.h', 'muse_input.c', 'CMakeLists.txt'):
            text = (ROOT / 'components/muse' / name).read_text()
            self.assertNotIn('prompter', text.lower(), name)
            self.assertNotIn('muse_navigation', text, name)
        self.assertFalse((ROOT / 'components/muse/muse_prompter.c').exists())

    def test_no_embedded_key(self):
        text = (ROOT / 'components/muse/translator.c').read_text()
        self.assertNotIn('translator_secrets.h', text)
        self.assertNotIn('TR_INITIAL_KEY', text)
        self.assertIn("cfg.key[0]='\\0'", text)

    def test_translation_limits_and_tls(self):
        text = (ROOT / 'components/muse/muse_voice.c').read_text()
        self.assertRegex(text, r'#define REC_MAX\s+\(16000\*12\)')
        self.assertRegex(text, r'#define TRANSLATOR_PARALLEL_TARGETS\s+1')
        self.assertIn('esp_crt_bundle_attach', text)
        self.assertNotRegex(text, r'skip_cert_common_name_check\s*=\s*true')

    def test_public_profiles_have_no_credentials(self):
        files = [ROOT / 'sdkconfig.defaults', ROOT / 'devices/sdkconfig.muse',
                 ROOT / 'devices/sdkconfig.muse-waveshare-s3-175c']
        for path in files:
            for name, value in re.findall(r'^CONFIG_(\w*(?:TOKEN|PASSWORD|SSID))="([^"\n]*)"', path.read_text(), re.M):
                self.assertEqual(value, '', f'{path.name}: {name}')


if __name__ == '__main__':
    unittest.main()
