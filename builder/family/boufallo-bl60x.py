# Experimental BL602 SDK family builder.
# The main builder emits a raw application and a MINIR3 dual-slot UF2.
import os
import shlex
import shutil
from os.path import join

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
platform = env.PioPlatform()
sdk = platform.get_package_dir("framework-bouffalo-bl602")
toolchain = platform.get_package_dir("toolchain-riscv")
if not sdk or not toolchain:
    raise RuntimeError("BL602 requires the SDK and RISC-V toolchain packages")
env.PrependENVPath("PATH", join(toolchain, "bin"))
# PlatformIO's registry GCC uses riscv-none-elf; the BL602 reference SDK's
# GCC 10 uses riscv64-unknown-elf. Both target rv32 with the flags below.
tool_bin = join(toolchain, "bin")
prefix = next(
    (
        name
        for name in ("riscv-none-elf-", "riscv64-unknown-elf-")
        if os.path.isfile(join(tool_bin, name + "gcc"))
    ),
    None,
)
if not prefix:
    raise RuntimeError("BL602 toolchain has no supported RISC-V GCC executable")
env.Replace(
    AR=prefix + "gcc-ar",
    AS=prefix + "gcc",
    CC=prefix + "gcc",
    CXX=prefix + "g++",
    GDB=prefix + "gdb",
    NM=prefix + "gcc-nm",
    LINK=prefix + "gcc",
    OBJCOPY=prefix + "objcopy",
    OBJDUMP=prefix + "objdump",
    RANLIB=prefix + "gcc-ranlib",
    SIZETOOL=prefix + "size",
)
env.ConfigureFamily()

# The reference package carries the vendor headers and prebuilt libraries.
# Derive its exact include and feature flags from its pinned platform manifest.
props = {}
with open(join(sdk, "platform.txt"), encoding="utf-8") as source:
    for line in source:
        key, sep, value = line.strip().partition("=")
        if sep:
            props[key] = value
flags = shlex.split(
    props["compiler.sdk.includes"] + " " + props["compiler.riscv.flags"]
)
includes = [
    f[2:].replace("{runtime.platform.path}", sdk) for f in flags if f.startswith("-I")
]
defines = [f[2:] for f in flags if f.startswith("-D")]
defines += [
    f[2:] for f in shlex.split(props["compiler.sdk.defines"]) if f.startswith("-D")
]
arch = ["-march=rv32imfc", "-mabi=ilp32f", "-ffunction-sections", "-fdata-sections"]
env.Append(CXXFLAGS=["-fno-threadsafe-statics"])
env.Append(
    CCFLAGS=arch,
    ASFLAGS=arch,
    LINKFLAGS=arch + ["-nostartfiles", "-Wl,--wrap=vPortDefineHeapRegions"],
    CPPPATH=includes,
    CPPDEFINES=defines,
    LIBPATH=[join(sdk, "system", "lib")],
)
# The stock MINIR3 partition table has no media partition. The SDK's prebuilt
# startup otherwise tries to mount ROMFS there and loops forever when missing.
if env.subst("$BOARD") == "sonoff-minir3":
    env.Append(LINKFLAGS=["-Wl,--wrap=romfs_register"])
if os.environ.get("BL602_BLE_INIT_TRACE") == "1":
    env.Append(
        CPPDEFINES=["LT_BL602_BLE_INIT_TRACE"],
        LINKFLAGS=[
            "-Wl,--wrap=" + name
            for name in (
                "rfc_init",
                "rwip_init",
                "ble_ke_init",
                "ble_ke_mem_init",
                "ble_rf_init",
                "ecc_init",
                "hci_init",
                "rwble_init",
                "ea_init",
            )
        ],
    )
# Give consumers a build-local linker script they can safely patch without
# modifying the installed SDK. In particular ESPHome routes ISR code to RAM.
ld_dir = env.subst("$BUILD_DIR")
os.makedirs(ld_dir, exist_ok=True)
ld_path = join(ld_dir, "bl602_flash_rom.ld")
shutil.copyfile(join(sdk, "system", "ld", "flash_rom.ld"), ld_path)
env.Replace(LDSCRIPT_PATH=[ld_path])

queue = env.AddLibraryQueue("bouffalo-bl602")
# The SDK's software atomic implementation is included from this core's api/
# sources. Keep the A extension disabled so GCC cannot emit AMO instructions
# into ordinary SRAM accesses.
# The SDK runs C++ constructors before bfl_main initializes heap_5.  Initialize
# those same regions immediately before constructors, and intercept the later
# SDK initialization so it cannot reset an already used heap.
startup_dir = join(ld_dir, "bl602_early_startup")
os.makedirs(startup_dir, exist_ok=True)
with open(join(sdk, "system", "startup", "start.S"), encoding="utf-8") as source:
    startup = source.read()
marker = "// call global obj constructors"
if startup.count(marker) != 1:
    raise RuntimeError(
        "Unexpected BL602 SDK startup; cannot insert early heap initialization"
    )
startup = startup.replace(marker, "    call lt_bl602_prepare_heap\n\n" + marker)
with open(join(startup_dir, "start.S"), "w", encoding="utf-8") as target:
    target.write(startup)
queue.AddLibrary(name="bl602_startup", base_dir=startup_dir, srcs=["+<start.S>"])
queue.BuildLibraries()

# Match the SDK's archive list. The linker command groups all archives to
# resolve circular references. The generic UF2 package is produced by the main
# builder from the linked application and verified board partition map.
sdk_libs = shlex.split(props["compiler.c.elf.libs"])
sdk_libs = [x for x in sdk_libs if x.startswith("-l")]
env.Append(LIBS=[x[2:] for x in sdk_libs])
env.Replace(
    LINKCOM="${LINK} -o $TARGET $LINKFLAGS $__RPATH $SOURCES -Wl,--start-group $_LIBDIRFLAGS $_LIBFLAGS -Wl,--end-group"
)
