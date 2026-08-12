"""Enable ESP-IDF OpenThread in pioarduino hybrid builds.

The hybrid builder recompiles IDF components but does not export newly enabled
component metadata to the following Arduino build. This script supplies that
metadata and applies Tasmota's small external-settings extension to the IDF
source package before the hybrid pass.
"""

Import("env")

from pathlib import Path


def _matter_thread_enabled():
    flags = " ".join(str(flag) for flag in env.get("BUILD_FLAGS", []))
    return "USE_MATTER_THREAD" in flags


def _replace_once(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if old not in text:
        raise RuntimeError("Cannot patch OpenThread source; unexpected contents in %s" % path)
    path.write_text(text.replace(old, new, 1))


if _matter_thread_enabled():
    platform = env.PioPlatform()
    idf_dir = Path(platform.get_package_dir("framework-espidf"))
    ot_dir = idf_dir / "components" / "openthread"

    # The native OpenThread archive calls the crypto and settings callbacks in
    # BearThread, but no application source includes a BearThread header after
    # the migration to IDF's public API. Make the glue library an explicit
    # dependency instead of relying on PlatformIO's include-based discovery.
    lib_deps = env.GetProjectOption("lib_deps", [])
    if "BearThread" not in lib_deps:
        env.GetProjectConfig().set(
            "env:" + env["PIOENV"],
            "lib_deps",
            "\n".join(list(lib_deps) + ["BearThread"]),
        )

    # ESP-IDF otherwise compiles its strong NVS otPlatSettings* implementation.
    _replace_once(
        ot_dir / "Kconfig",
        '        config OPENTHREAD_HEADER_CUSTOM\n',
        '        config OPENTHREAD_EXTERNAL_SETTINGS\n'
        '            bool "Use an application-provided settings backend"\n'
        '            default n\n'
        '            help\n'
        '                Exclude the ESP-IDF NVS otPlatSettings implementation.\n\n'
        '        config OPENTHREAD_HEADER_CUSTOM\n',
    )
    _replace_once(
        ot_dir / "srcs_ftd_mtd.cmake",
        '# Optional features\n',
        '# External settings backend\n'
        'if(CONFIG_OPENTHREAD_EXTERNAL_SETTINGS)\n'
        '    list(APPEND exclude_srcs\n'
        '        "src/port/esp_openthread_settings.c")\n'
        'endif()\n\n'
        '# Optional features\n',
    )
    _replace_once(
        ot_dir / "src" / "esp_openthread_platform.cpp",
        '    esp_openthread_set_storage_name(config->port_config.storage_partition_name);\n',
        '#if !CONFIG_OPENTHREAD_EXTERNAL_SETTINGS\n'
        '    esp_openthread_set_storage_name(config->port_config.storage_partition_name);\n'
        '#endif\n',
    )

    # Export the IDF component's public API and generated role configuration to
    # the normal Arduino phase. Prefer the sdkconfig produced by the hybrid pass;
    # the project setting is only a fallback during the first/integration pass.
    arduino_dir = Path(platform.get_package_dir("framework-arduinoespressif32"))
    sdk_dir = arduino_dir / "tools" / "esp32-arduino-libs" / "esp32c6"
    memory_type = env.BoardConfig().get(
        "build.arduino.memory_type",
        env.BoardConfig().get("build.flash_mode", "dio") + "_qspi",
    )
    sdkconfig_h = sdk_dir / memory_type / "include" / "sdkconfig.h"
    requested_sdkconfig = env.GetProjectOption("custom_sdkconfig", "")
    sdkconfig = sdkconfig_h.read_text() if sdkconfig_h.is_file() else ""

    generated_has_role = "CONFIG_OPENTHREAD_FTD 1" in sdkconfig or "CONFIG_OPENTHREAD_MTD 1" in sdkconfig
    role_config = sdkconfig if generated_has_role else requested_sdkconfig
    if "CONFIG_OPENTHREAD_MTD 1" in role_config or "CONFIG_OPENTHREAD_MTD=y" in role_config:
        role = "MTD"
        config_file = "openthread-core-esp32x-mtd-config.h"
    elif "CONFIG_OPENTHREAD_FTD 1" in role_config or "CONFIG_OPENTHREAD_FTD=y" in role_config:
        role = "FTD"
        config_file = "openthread-core-esp32x-ftd-config.h"
    else:
        raise RuntimeError("OpenThread is enabled but the hybrid sdkconfig has no FTD/MTD role")

    # The Arduino core's IDF message-pool wrapper is also compiled during the
    # hybrid pass, so headers and role definitions are needed in both phases.
    env.AppendUnique(CPPPATH=[
        str(ot_dir / "include"),
        str(ot_dir / "openthread" / "include"),
        str(ot_dir / "private_include"),
        str(Path(env.subst("$PROJECT_DIR")) / "lib" / "libesp32" / "BearThread" / "include"),
    ])
    env.AppendUnique(CPPDEFINES=[
        ("OPENTHREAD_%s" % role, 1),
        ("OPENTHREAD_CONFIG_FILE", '\"%s\"' % config_file),
    ])

    if not generated_has_role:
        # An sdkconfig from an older hybrid build takes precedence over
        # sdkconfig.defaults. Remove only this generated environment artifact
        # so pioarduino can regenerate it with the requested OT configuration.
        project_sdkconfig = Path(env.subst("$PROJECT_DIR")) / ("sdkconfig." + env["PIOENV"])
        if project_sdkconfig.is_file():
            old_config = project_sdkconfig.read_text()
            requested_lines = [line.strip() for line in requested_sdkconfig.splitlines() if line.strip().startswith("CONFIG_")]
            config_matches = True
            for line in requested_lines:
                key, value = line.split("=", 1)
                if value == "n":
                    matches = line in old_config or ("# %s is not set" % key) in old_config
                else:
                    matches = line in old_config
                if not matches:
                    config_matches = False
                    break
            if not config_matches:
                project_sdkconfig.unlink()
        print("*** ESP-IDF OpenThread source patch enabled; awaiting hybrid sdkconfig ***")
        Return()

    # pioarduino-build.py replaces LIBS after pre-scripts run. Patch its static
    # component list once the hybrid archive exists, matching how this metadata
    # is expected to be supplied by esp32-arduino-lib-builder.
    build_metadata = sdk_dir / "pioarduino-build.py"
    _replace_once(
        build_metadata,
        '    LIBS=[\n        "',
        '    LIBS=[\n        "-lopenthread", "',
    )
    print("*** ESP-IDF OpenThread metadata enabled (%s) ***" % role)
