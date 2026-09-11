Import("env")
from pathlib import Path


def merge_firmware(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    project_dir = Path(env.subst("$PROJECT_DIR"))
    packages_dir = Path(env.subst("$PROJECT_PACKAGES_DIR"))
    python_exe = env.subst("$PYTHONEXE")
    esptool = packages_dir / "tool-esptoolpy" / "esptool.py"
    boot_app0 = packages_dir / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
    out_dir = project_dir / "dist"
    out_dir.mkdir(exist_ok=True)
    output = out_dir / "huawei-sunbridge-wt32-eth01-webflash.bin"

    cmd = [
        python_exe, str(esptool), "--chip", "esp32", "merge_bin",
        "-o", str(output),
        "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
        "0x1000", str(build_dir / "bootloader.bin"),
        "0x8000", str(build_dir / "partitions.bin"),
        "0xe000", str(boot_app0),
        "0x10000", str(build_dir / "firmware.bin"),
    ]
    env.Execute(" ".join('\"%s\"' % x if ' ' in x else x for x in cmd))
    print("\nWeb-flash image: %s" % output)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)
