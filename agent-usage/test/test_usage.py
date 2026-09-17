import unittest

from ble_sender import encode_usage_payload
from codex_usage_sender import CursorUsage, dollars_to_cents, parse_cursor_usage


class CursorUsageTests(unittest.TestCase):
    def test_uses_primary_percentage_with_personal_limit(self) -> None:
        response = [
            {
                "provider": "cursor",
                "source": "web",
                "usage": {
                    "primary": {"usedPercent": 40.0},
                    "providerCost": {"used": 0, "limit": 510},
                },
            }
        ]

        self.assertEqual(
            parse_cursor_usage(response, 15000),
            CursorUsage(6000, 15000),
        )

    def test_reports_provider_error(self) -> None:
        response = [
            {
                "provider": "cursor",
                "source": "auto",
                "error": {"message": "not signed in"},
            }
        ]

        with self.assertRaisesRegex(RuntimeError, "not signed in"):
            parse_cursor_usage(response, 15000)

    def test_rejects_missing_primary_usage(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "no valid Cursor monthly usage"):
            parse_cursor_usage([{"provider": "cursor", "usage": {}}], 15000)

    def test_parses_personal_limit_in_dollars(self) -> None:
        self.assertEqual(dollars_to_cents("150"), 15000)
        self.assertEqual(dollars_to_cents("19.995"), 2000)


class BlePayloadTests(unittest.TestCase):
    def test_encodes_codex_and_cursor_usage(self) -> None:
        self.assertEqual(
            encode_usage_payload(87, 6000, 15000),
            b"C87;U6000;L15000",
        )

    def test_encodes_unknown_values(self) -> None:
        self.assertEqual(encode_usage_payload(87, None, None), b"C87;U-;L-")

    def test_maximum_payload_fits_default_att_write(self) -> None:
        payload = encode_usage_payload(100, 999999, 999999)
        self.assertEqual(len(payload), 20)

    def test_rejects_out_of_range_values(self) -> None:
        with self.assertRaises(ValueError):
            encode_usage_payload(101, 0, 1)

    def test_rejects_partial_cursor_usage(self) -> None:
        with self.assertRaises(ValueError):
            encode_usage_payload(87, 6000, None)


if __name__ == "__main__":
    unittest.main()
