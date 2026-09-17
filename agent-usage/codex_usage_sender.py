#!/usr/bin/env python3

import argparse
import asyncio
import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from pathlib import Path

from bleak.exc import BleakError

from ble_sender import send_usage


CODEX_SESSIONS = Path.home() / ".codex" / "sessions"
WEEK_MINUTES = 7 * 24 * 60


@dataclass(frozen=True)
class CursorUsage:
    used_cents: int
    limit_cents: int


def read_weekly_remaining() -> int:
    if not CODEX_SESSIONS.is_dir():
        raise RuntimeError(f"Codex sessions directory not found: {CODEX_SESSIONS}")

    session_files = sorted(
        CODEX_SESSIONS.rglob("*.jsonl"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )

    for path in session_files:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError:
            continue

        for line in reversed(lines):
            try:
                event = json.loads(line)
                payload = event.get("payload", {})
                rate_limits = payload.get("rate_limits", {})
                weekly = rate_limits.get("secondary")

                if (
                    payload.get("type") == "token_count"
                    and weekly
                    and weekly.get("window_minutes") == WEEK_MINUTES
                ):
                    used = float(weekly["used_percent"])
                    return round(max(0.0, min(100.0, 100.0 - used)))
            except (json.JSONDecodeError, KeyError, TypeError, ValueError):
                continue

    raise RuntimeError("No Codex seven-day usage data found in local sessions")


def dollars_to_cents(value: str) -> int:
    try:
        dollars = Decimal(value)
    except InvalidOperation as error:
        raise ValueError("Cursor personal limit must be a dollar amount") from error
    if not dollars.is_finite() or dollars <= 0:
        raise ValueError("Cursor personal limit must be greater than zero")
    cents = int((dollars * 100).quantize(Decimal("1"), rounding=ROUND_HALF_UP))
    if cents > 999_999:
        raise ValueError("Cursor personal limit is too large for the BLE payload")
    return cents


def parse_cursor_usage(payload: object, personal_limit_cents: int) -> CursorUsage:
    if not isinstance(payload, list):
        raise RuntimeError("CodexBar output was not a JSON array")

    cursor_result = next(
        (
            item
            for item in payload
            if isinstance(item, dict) and item.get("provider") == "cursor"
        ),
        None,
    )
    if cursor_result is None:
        raise RuntimeError("CodexBar output did not contain the Cursor provider")

    error = cursor_result.get("error")
    if isinstance(error, dict):
        message = error.get("message")
        raise RuntimeError(
            f"CodexBar Cursor provider failed: {message or 'unknown error'}"
        )

    usage = cursor_result.get("usage")
    primary = usage.get("primary") if isinstance(usage, dict) else None
    used_percent = primary.get("usedPercent") if isinstance(primary, dict) else None
    if (
        isinstance(used_percent, bool)
        or not isinstance(used_percent, (int, float))
        or not 0 <= used_percent <= 100
    ):
        raise RuntimeError("CodexBar output has no valid Cursor monthly usage")

    # Deliberately ignore providerCost: for Team/Enterprise accounts CodexBar may
    # populate it from the shared team pool.  The explicitly configured personal
    # cap is the only denominator used here.
    used_cents = round(personal_limit_cents * float(used_percent) / 100.0)
    return CursorUsage(used_cents=used_cents, limit_cents=personal_limit_cents)


def read_cursor_usage(
    personal_limit_cents: int,
    timeout: float = 15.0,
    codexbar_bin: str | None = None,
) -> CursorUsage:
    executable = (
        codexbar_bin
        or os.environ.get("CODEXBAR_BIN")
        or shutil.which("codexbar")
    )
    if not executable:
        raise RuntimeError("CodexBar CLI was not found in PATH")

    command = [
        executable,
        "usage",
        "--provider",
        "cursor",
        "--source",
        "auto",
        "--format",
        "json",
        "--json-only",
        "--web-timeout",
        str(timeout),
    ]
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout + 5,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("CodexBar Cursor query timed out") from error
    except OSError as error:
        raise RuntimeError(f"Could not run CodexBar: {error}") from error

    try:
        payload = json.loads(result.stdout)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        detail = result.stderr.strip().splitlines()[-1] if result.stderr.strip() else ""
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(f"CodexBar did not return valid JSON{suffix}") from error

    return parse_cursor_usage(payload, personal_limit_cents)


def format_dollars(cents: int) -> str:
    return f"${cents / 100:.2f}"


async def run_once(
    dry_run: bool,
    cursor_timeout: float,
    cursor_limit_cents: int | None,
    codexbar_bin: str | None,
) -> None:
    codex_remaining: int | None = None
    cursor_usage: CursorUsage | None = None

    try:
        codex_remaining = read_weekly_remaining()
        print(f"Codex weekly remaining: {codex_remaining}%")
    except (RuntimeError, OSError, ValueError) as error:
        print(f"Codex error [{type(error).__name__}]: {error}")

    if cursor_limit_cents is None:
        print(
            "Cursor error [Configuration]: set --cursor-personal-limit-usd or "
            "CURSOR_PERSONAL_LIMIT_USD; pooled team limits are intentionally ignored"
        )
    else:
        try:
            cursor_usage = await asyncio.to_thread(
                read_cursor_usage,
                cursor_limit_cents,
                cursor_timeout,
                codexbar_bin,
            )
            print(
                "Cursor personal monthly usage: "
                f"{format_dollars(cursor_usage.used_cents)}/"
                f"{format_dollars(cursor_usage.limit_cents)}"
            )
        except (RuntimeError, OSError, ValueError) as error:
            print(f"Cursor error [{type(error).__name__}]: {error}")

    if codex_remaining is None and cursor_usage is None:
        raise RuntimeError("No usage data could be collected")

    if not dry_run:
        await send_usage(
            codex_remaining,
            cursor_usage.used_cents if cursor_usage else None,
            cursor_usage.limit_cents if cursor_usage else None,
        )


async def run(
    interval: int | None,
    dry_run: bool,
    cursor_timeout: float,
    cursor_limit_cents: int | None,
    codexbar_bin: str | None,
) -> None:
    while True:
        try:
            await run_once(
                dry_run,
                cursor_timeout,
                cursor_limit_cents,
                codexbar_bin,
            )
        except (BleakError, RuntimeError, OSError, ValueError) as error:
            detail = str(error) or repr(error)
            print(f"Error [{type(error).__name__}]: {detail}")

        if interval is None:
            return

        print(f"Next update in {interval} seconds")
        await asyncio.sleep(interval)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Read Codex and Cursor usage and send it over BLE."
    )
    parser.add_argument(
        "--watch",
        type=int,
        default=300,
        metavar="SECONDS",
        help="repeat interval in seconds (default: 300, minimum: 60)",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="send once and exit",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the value without using BLE",
    )
    parser.add_argument(
        "--cursor-timeout",
        type=float,
        default=15.0,
        metavar="SECONDS",
        help="Cursor API timeout in seconds (default: 15)",
    )
    parser.add_argument(
        "--cursor-personal-limit-usd",
        default=os.environ.get("CURSOR_PERSONAL_LIMIT_USD"),
        metavar="DOLLARS",
        help=(
            "personal monthly Cursor limit in USD; can also be set with "
            "CURSOR_PERSONAL_LIMIT_USD"
        ),
    )
    parser.add_argument(
        "--codexbar-bin",
        default=os.environ.get("CODEXBAR_BIN"),
        metavar="PATH",
        help="CodexBar CLI path; defaults to CODEXBAR_BIN or PATH",
    )
    args = parser.parse_args()

    if args.watch < 60:
        parser.error("--watch must be at least 60 seconds")
    if args.cursor_timeout <= 0:
        parser.error("--cursor-timeout must be greater than zero")

    try:
        cursor_limit_cents = (
            dollars_to_cents(args.cursor_personal_limit_usd)
            if args.cursor_personal_limit_usd is not None
            else None
        )
    except ValueError as error:
        parser.error(str(error))

    asyncio.run(
        run(
            None if args.once else args.watch,
            args.dry_run,
            args.cursor_timeout,
            cursor_limit_cents,
            args.codexbar_bin,
        )
    )


if __name__ == "__main__":
    main()
