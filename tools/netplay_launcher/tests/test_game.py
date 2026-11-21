from __future__ import annotations

import unittest

from launcher.game import normalize_address


class AddressTests(unittest.TestCase):
    def test_default_port_is_added(self) -> None:
        self.assertEqual(normalize_address("192.168.1.10"), "192.168.1.10:25565")
        self.assertEqual(normalize_address("example.org"), "example.org:25565")
        self.assertEqual(normalize_address(" example.org:20000 "), "example.org:20000")

    def test_empty_address_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            normalize_address("   ")


if __name__ == "__main__":
    unittest.main()
