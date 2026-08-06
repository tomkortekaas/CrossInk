"""Use NimBLE-Arduino's C++ API with ESP-IDF's single NimBLE stack.

NimBLE-Arduino ships its own NimBLE C sources for normal Arduino builds.  The
probe environment compiles ESP-IDF's BT component instead, so those bundled C
sources must not enter the PlatformIO archive.  A generated library.json keeps
the dependency pin intact while selecting only the top-level C++ wrapper.
"""

Import("env")

import json
import os


def configure_nimble_idf(environment):
    if environment.subst("$PIOENV") != "x3-dashboard-ble-probe":
        return

    library_dir = os.path.join(
        environment["PROJECT_DIR"], ".pio", "libdeps", environment.subst("$PIOENV"), "NimBLE-Arduino"
    )
    properties_path = os.path.join(library_dir, "library.properties")
    if not os.path.isfile(properties_path):
        print("WARNING: NimBLE-Arduino dependency is not installed yet")
        return

    with open(properties_path, "r", encoding="utf-8") as properties_file:
        properties = properties_file.read()
    if "version=2.3.8" not in properties:
        raise RuntimeError("Expected pinned NimBLE-Arduino 2.3.8")

    manifest = {
        "name": "NimBLE-Arduino",
        "version": "2.3.8",
        "build": {"srcFilter": ["+<*.cpp>", "+<*.h>", "-<nimble/>"]},
    }
    manifest_path = os.path.join(library_dir, "library.json")
    with open(manifest_path, "w", encoding="utf-8") as manifest_file:
        json.dump(manifest, manifest_file, indent=2)
        manifest_file.write("\n")
    print("Configured NimBLE-Arduino 2.3.8 for the ESP-IDF single-stack backend")


configure_nimble_idf(env)
