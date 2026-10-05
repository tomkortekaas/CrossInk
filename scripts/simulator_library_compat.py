"""Bridge the installed simulator HAL to this fork's Library and timer wake APIs.

Host-only compatibility; never loaded by firmware environments.
"""
from pathlib import Path

Import("env")
source = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "simulator" / "src"
patches = {
    "HalDisplay.cpp": ("  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);", "  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);\n  if (!sdl_renderer) sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);"),
    "InputManager.h": ("  InputManager() {}", "  static constexpr unsigned char BTN_BACK = 0, BTN_CONFIRM = 1, BTN_UP = 4, BTN_DOWN = 5;\n  InputManager() {}"),
    "HalStorage.h": ("  uint64_t fileSize64();", "  uint64_t fileSize64();\n  uint32_t creationTime() { return 0; }\n  uint32_t modificationTime() { return 0; }"),
    "HalGPIO.h": ("enum class WakeupReason { PowerButton, AfterFlash", "enum class WakeupReason { PowerButton, Timer, AfterFlash"),
    "HalPowerManager.h": ("  void startDeepSleep(HalGPIO &gpio) const;", "  void startDeepSleep(HalGPIO &gpio) const;\n  void startDeepSleep(HalGPIO &gpio, uint64_t) const { startDeepSleep(gpio); }"),
}
for name, (old, new) in patches.items():
    path = source / name
    if not path.exists():
        continue
    text = path.read_text()
    if name == "HalStorage.h" and "creationTime(" in text:
        continue
    if new not in text:
        path.write_text(text.replace(old, new))


# telldir cookies cannot resume a newly opened directory on macOS. Use a
# stable entry count, matching the reader's close/reopen scan protocol.
path = source / "HalStorage.cpp"
text = path.read_text()
if "directoryPosition" not in text:
    text = text.replace("  if (impl && impl->dir) { seekdir(impl->dir, static_cast<long>(offset)); return true; }\n", "")
    text = text.replace("  if (impl && impl->dir) { const long offset = telldir(impl->dir); return offset < 0 ? 0 : static_cast<size_t>(offset); }\n", "")
    patches = [
        ("  DIR *dir = nullptr;", "  DIR *dir = nullptr;\n  size_t directoryPosition = 0;"),
        ("bool HalFile::seekSet(size_t offset) {", "bool HalFile::seekSet(size_t offset) {\n  if (impl && impl->dir) {\n    rewinddir(impl->dir);\n    impl->directoryPosition = 0;\n    while (impl->directoryPosition < offset) {\n      if (!readdir(impl->dir)) return false;\n      ++impl->directoryPosition;\n    }\n    return true;\n  }"),
        ("size_t HalFile::position() const {", "size_t HalFile::position() const {\n  if (impl && impl->dir) return impl->directoryPosition;"),
        ("    struct dirent *entry = readdir(impl->dir);", "    struct dirent *entry = readdir(impl->dir);\n    if (entry) ++impl->directoryPosition;"),
        ("    rewinddir(impl->dir);\n}", "    rewinddir(impl->dir);\n  if (impl) impl->directoryPosition = 0;\n}"),
    ]
    for old, new in patches:
        if new not in text:
            text = text.replace(old, new)
    path.write_text(text)
