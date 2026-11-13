#!/usr/bin/env python3
"""Verify Bazel build/test evidence instead of trusting the exit status alone.

Usage:
  verify-results.py build <bep-jsonl> [--report <json>]
  verify-results.py test  <bep-jsonl> --testlogs <bazel-testlogs> [--report <json>]

Gates enforced here:
  * build: buildFinished reports overall success, at least one target completed
    successfully, and the action summary shows actions were actually executed;
  * test:  every executed test target has a non-empty GTest XML with at least
           one test case and no failures/errors, and no result came from a
           cache (CI must observe a real run).

Note on BEP shapes: the build-finished payload is under "finished" (its id key is
"buildFinished"), test summaries carry a "totalNumCached" counter, and per-action
events are absent unless --build_event_publish_all_actions is set, so action
counts come from buildMetrics.actionSummary.
"""
import argparse
import json
import os
import pathlib
import sys
import xml.etree.ElementTree as ET


def die(message):
    print(f"ci verify: {message}", file=sys.stderr)
    sys.exit(1)


def events(path):
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield json.loads(line)


def label_path(label):
    """//tests/unit/core:cancel_test -> tests/unit/core/cancel_test"""
    body = label[2:] if label.startswith("//") else label
    package, _, name = body.partition(":")
    name = name or package.rsplit("/", 1)[-1]
    return os.path.join(package, name)


def verify_build(bep):
    finished = None
    completed = 0
    summary = {}
    for event in events(bep):
        event_id = event.get("id", {})
        if "buildFinished" in event_id:
            finished = event.get("finished", {})
        elif "targetCompleted" in event_id:
            if event.get("completed", {}).get("success"):
                completed += 1
        elif "buildMetrics" in event_id:
            summary = event.get("buildMetrics", {}).get("actionSummary", {})

    if finished is None:
        die(f"no buildFinished event in {bep}; the build did not complete")
    if not finished.get("overallSuccess"):
        die("buildFinished reports failure")
    if completed == 0:
        die("no successful targetCompleted event; nothing was built")

    executed = int(summary.get("actionsExecuted", 0) or 0)
    if executed == 0:
        die("buildMetrics reports zero executed actions")

    compiles = sum(
        int(entry.get("actionsExecuted", 0) or 0)
        for entry in summary.get("actionData", [])
        if entry.get("mnemonic") in ("CppCompile", "CppLink", "ProtoCompile", "KotlinCompile")
    )
    if compiles == 0:
        die("no C++/proto/kotlin compile action in the action summary")

    return {
        "schema_version": 1,
        "kind": "build",
        "targets_completed": completed,
        "actions_executed": executed,
        "compile_actions": compiles,
        "success": True,
    }


def verify_test(bep, testlogs):
    reports = []
    for event in events(bep):
        event_id = event.get("id", {})

        cached = event.get("testResult", {})
        if "testResult" in event_id and cached.get("cachedLocally"):
            die(f"{event_id['testResult'].get('label')}: result came from the local test cache")

        if "testSummary" not in event_id:
            continue
        summary_id = event_id["testSummary"]
        if summary_id.get("label") is None:  # summary of a skipped target
            continue
        label = summary_id["label"]
        payload = event.get("testSummary", {})
        if int(payload.get("totalNumCached", 0) or 0) > 0:
            die(f"{label}: Bazel served a cached test result")

        status = payload.get("overallStatus", "NO_STATUS")
        xml_path = pathlib.Path(testlogs) / label_path(label) / "test.xml"
        if not xml_path.is_file() or xml_path.stat().st_size == 0:
            die(f"{label}: missing or empty GTest XML at {xml_path}")
        root = ET.parse(xml_path).getroot()
        suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
        if not suites:
            die(f"{label}: no testsuite in {xml_path}")
        tests = sum(int(suite.get("tests", 0)) for suite in suites)
        failures = sum(int(suite.get("failures", 0)) for suite in suites)
        errors = sum(int(suite.get("errors", 0)) for suite in suites)
        skipped = sum(int(suite.get("skipped", 0) or 0) for suite in suites)
        if tests == 0:
            die(f"{label}: zero test cases executed")
        if failures or errors:
            die(f"{label}: {failures} failures / {errors} errors")
        if status != "PASSED":
            die(f"{label}: Bazel reports {status}")
        reports.append(
            {"label": label, "status": status, "tests": tests, "skipped": skipped}
        )

    if not reports:
        die("no test targets ran; a passing job must execute tests")
    return {
        "schema_version": 1,
        "kind": "test",
        "targets": len(reports),
        "test_cases": sum(report["tests"] for report in reports),
        "skipped": sum(report["skipped"] for report in reports),
        "results": sorted(reports, key=lambda report: report["label"]),
        "success": True,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("kind", choices=("build", "test"))
    parser.add_argument("bep")
    parser.add_argument("--testlogs")
    parser.add_argument("--report")
    args = parser.parse_args()

    if not os.path.isfile(args.bep):
        die(f"missing Bazel event file: {args.bep}")

    if args.kind == "build":
        report = verify_build(args.bep)
    else:
        if not args.testlogs:
            die("--testlogs is required for test verification")
        report = verify_test(args.bep, args.testlogs)

    if args.report:
        pathlib.Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
