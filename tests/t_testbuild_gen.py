"""Which builds are test builds: the build-time half of the rule, as
gen_xcodeproj.py bakes it into Info.plist (KLTestBuild). The TEST_BUILD block is
taken from the script and run against each environment below, because the
whole script needs a game's unpacked files. The launch-line half is
tests/t_testbuild.swift. `make testbuild`.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, "..", "visionos", "gen_xcodeproj.py")).read()
start = src.index("_bundle_test = ")
end = src.index("\n\n", start)
block = src[start:end]

DEFAULT = "someone.dev.klepton.target.moss"
VARS = ("KLEPTON_BUNDLE_ID", "KLEPTON_TEST_ON", "KLEPTON_DISPLAY_SUFFIX",
        "KLEPTON_DISPLAY_OVERRIDE", "KLEPTON_TEST_BUILD")
CASES = [
    ({}, False),
    # AVP Play sets a bundle id for every install, and KLEPTON_TEST_ON for a
    # player who chose its experimental switches: neither makes a test build.
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.moss"}, False),
    ({"KLEPTON_TEST_ON": "KL_OVRP_ONE_POSE,KL_AUDIO_SINK2"}, False),
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.mosstest"}, False),
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.moss-tests"}, False),
    # Bundle id ending in -test or .test, in any case (regex [-.]test$).
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.moss-test"}, True),
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.moss.test"}, True),
    ({"KLEPTON_BUNDLE_ID": "abcd.dev.klepton.target.moss-TEST"}, True),
    ({"KLEPTON_DISPLAY_SUFFIX": " (Test)"}, True),
    ({"KLEPTON_DISPLAY_SUFFIX": ""}, False),
    ({"KLEPTON_DISPLAY_OVERRIDE": "Moss X"}, True),
    ({"KLEPTON_TEST_ON": "KL_AUDIO_SINK2,KL_TEST_BUILD"}, True),
    ({"KLEPTON_TEST_BUILD": "1"}, True),
    ({"KLEPTON_TEST_BUILD": "0"}, False),
]

print("=== test build: what gen_xcodeproj.py bakes ===")
failed = 0
for env, want in CASES:
    for k in VARS:
        os.environ.pop(k, None)
    os.environ.update(env)
    g = {"re": re, "os": os, "sys": sys, "KLT": {"bundle": DEFAULT},
         "BUNDLE_ID": os.environ.get("KLEPTON_BUNDLE_ID", DEFAULT)}
    exec(block, g)
    ok = g["TEST_BUILD"] == want
    failed += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {env} -> {'test build' if g['TEST_BUILD'] else 'player build'}")
print("testbuild (gen): " + ("PASS" if not failed else f"FAIL ({failed})"))
sys.exit(1 if failed else 0)
