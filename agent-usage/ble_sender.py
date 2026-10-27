#!/usr/bin/env python3

import argparse
import asyncio

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


DEVICE_NAME = "Codex-Usage-Monitor"
SERVICE_UUID = "12345678-1234-1234-1234-123456789abc"
CHARACTERISTIC_UUID = "87654321-4321-4321-4321-cba987654321"
MAX_CURSOR_CENTS = 999_999
CONNECT_ATTEMPTS = 3


def _validate_optional_value(name: str, value: int | None, maximum: int) -> None:
    if value is not None and not 0 <= value <= maximum:
        raise ValueError(f"{name} must be between 0 and {maximum}")


def encode_usage_payload(
    codex_percentage: int | None,
    cursor_used_cents: int | None,
    cursor_limit_cents: int | None,
) -> bytes:
    _validate_optional_value("Codex percentage", codex_percentage, 100)
    _validate_optional_value("Cursor used cents", cursor_used_cents, MAX_CURSOR_CENTS)
    _validate_optional_value("Cursor limit cents", cursor_limit_cents, MAX_CURSOR_CENTS)
    if (cursor_used_cents is None) != (cursor_limit_cents is None):
        raise ValueError("Cursor used and limit values must both be set or both be unknown")
    if cursor_limit_cents == 0:
        raise ValueError("Cursor limit cents must be greater than zero")

    codex = "-" if codex_percentage is None else str(codex_percentage)
    cursor_used = "-" if cursor_used_cents is None else str(cursor_used_cents)
    cursor_limit = "-" if cursor_limit_cents is None else str(cursor_limit_cents)
    payload = f"C{codex};U{cursor_used};L{cursor_limit}".encode("ascii")

    # Keep the write within the default 20-byte BLE ATT payload.
    if len(payload) > 20:
        raise ValueError("encoded BLE usage payload exceeds 20 bytes")
    return payload


class UsageBleClient:
    """Keep one BLE/GATT connection alive across periodic usage updates."""

    def __init__(self) -> None:
        self._client: BleakClient | None = None

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    async def _disconnect(self) -> None:
        client, self._client = self._client, None
        if client is None:
            return
        try:
            # Call disconnect even when BlueZ already reports false. A timed
            # out connection can still have a half-open D-Bus device object
            # that must be cleaned up before the next attempt.
            await client.disconnect()
        except (BleakError, OSError, RuntimeError, TimeoutError):
            pass

    async def close(self) -> None:
        await self._disconnect()

    async def _connect(self) -> BleakClient:
        if self.is_connected:
            return self._client  # type: ignore[return-value]

        # Discard any stale BlueZ client before starting a fresh scan.
        await self._disconnect()

        for attempt in range(1, CONNECT_ATTEMPTS + 1):
            print(
                f"Scanning for {DEVICE_NAME} "
                f"(attempt {attempt}/{CONNECT_ATTEMPTS})..."
            )
            device = await BleakScanner.find_device_by_name(
                DEVICE_NAME,
                timeout=10.0,
            )

            if device is None:
                print(f"Attempt {attempt} failed: device not found")
                if attempt == CONNECT_ATTEMPTS:
                    raise RuntimeError(
                        f"Could not find {DEVICE_NAME}. Check that the board is "
                        "powered on and advertising."
                    )
                await asyncio.sleep(2.0)
                continue

            print(
                f"Connecting to {device.address} "
                f"(attempt {attempt}/{CONNECT_ATTEMPTS})..."
            )
            client = BleakClient(
                device,
                timeout=20.0,
                services=[SERVICE_UUID],
            )
            try:
                await client.connect()
            except (BleakError, RuntimeError, TimeoutError) as error:
                detail = str(error) or repr(error)
                print(
                    f"Attempt {attempt} failed "
                    f"[{type(error).__name__}]: {detail}"
                )
                try:
                    await client.disconnect()
                except (BleakError, OSError, RuntimeError, TimeoutError):
                    pass
                if attempt == CONNECT_ATTEMPTS:
                    raise
                await asyncio.sleep(4.0)
                continue

            self._client = client
            print("Connected; GATT services discovered; keeping connection open")
            return client

        raise RuntimeError(f"Could not connect to {DEVICE_NAME}")

    async def send_usage(
        self,
        codex_percentage: int | None,
        cursor_used_cents: int | None,
        cursor_limit_cents: int | None,
    ) -> None:
        payload = encode_usage_payload(
            codex_percentage,
            cursor_used_cents,
            cursor_limit_cents,
        )

        # A connection can disappear while the process sleeps. Retry the write
        # once with a fresh connection when that happens.
        for write_attempt in range(1, 3):
            client = await self._connect()
            try:
                print(f"Writing usage payload: {payload.decode('ascii')}")
                await client.write_gatt_char(
                    CHARACTERISTIC_UUID,
                    payload,
                    response=True,
                )
                print("Sent usage successfully; BLE connection remains open")
                return
            except (BleakError, RuntimeError, TimeoutError) as error:
                detail = str(error) or repr(error)
                print(
                    "BLE write failed "
                    f"[{type(error).__name__}]: {detail}"
                )
                await self._disconnect()
                if write_attempt == 2:
                    raise
                print("Reconnecting before retrying the write...")


async def send_usage(
    codex_percentage: int | None,
    cursor_used_cents: int | None,
    cursor_limit_cents: int | None,
) -> None:
    """Send once and disconnect; periodic callers should reuse UsageBleClient."""
    client = UsageBleClient()
    try:
        await client.send_usage(
            codex_percentage,
            cursor_used_cents,
            cursor_limit_cents,
        )
    finally:
        await client.close()


async def send_percentage(percentage: int) -> None:
    """Backward-compatible helper for callers that only have Codex usage."""
    await send_usage(percentage, None, None)


def percentage_type(value: str) -> int:
    percentage = int(value)
    if not 0 <= percentage <= 100:
        raise argparse.ArgumentTypeError("percentage must be between 0 and 100")
    return percentage


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send Codex and Cursor usage to the T-Display-S3 over BLE."
    )
    parser.add_argument("percentage", type=percentage_type, help="value from 0 to 100")
    parser.add_argument("--cursor-used-cents", type=int)
    parser.add_argument("--cursor-limit-cents", type=int)
    args = parser.parse_args()

    try:
        asyncio.run(
            send_usage(
                args.percentage,
                args.cursor_used_cents,
                args.cursor_limit_cents,
            )
        )
    except (BleakError, RuntimeError, OSError, ValueError) as error:
        detail = str(error) or repr(error)
        raise SystemExit(f"Error [{type(error).__name__}]: {detail}") from error


if __name__ == "__main__":
    main()
