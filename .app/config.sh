# Matches the build gate already used in .github/workflows/ci.yml.
# The CMake/CTest suite under test/ and scripts/run_simulator_smoke_test.py
# are heavier, multi-step checks -- run them manually per firmware/AGENTS.md,
# they are intentionally not wired into `app finish`.
APP_TEST_CMD="pio run -e default"
APP_MAIN_BRANCH="main"
