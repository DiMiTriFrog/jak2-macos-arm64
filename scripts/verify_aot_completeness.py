#!/usr/bin/env python3
"""Reject an inventory/report until it contains linked AOT code."""

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_aot_completeness.py BUILD-REPORT.json")
    report_path = Path(sys.argv[1])
    if not report_path.is_file():
        raise SystemExit(f"missing AOT build report: {report_path}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("complete") is not True or report.get("aot_code_generation") != "linked":
        raise SystemExit(
            "AOT input is an inventory only; linked code generation is required before this gate"
        )
    print("PASS: complete linked Jak II AOT input")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
