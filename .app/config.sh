# Matches the build gate already used in .github/workflows/ci.yml.
# The CMake/CTest suite under test/ and scripts/run_simulator_smoke_test.py
# are heavier, multi-step checks -- run them manually per firmware/AGENTS.md,
# they are intentionally not wired into `app finish`.
# The active dashboard line is feat/ble-handoff (83 commits ahead of this
# fork's main) -- issue worktrees and PRs must target that, not main.
APP_TEST_CMD="pio run -e default"
APP_MAIN_BRANCH="feat/ble-handoff"
