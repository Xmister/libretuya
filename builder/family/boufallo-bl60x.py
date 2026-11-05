# Experimental BL602 SDK family builder.
import os
import hashlib
import shlex
import shutil
import subprocess
import struct
import zlib
from os.path import join

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()


def lz4_compress_block(data):
    """Encode one raw LZ4 block without adding a third-party build dependency."""
    output = bytearray()
    table = {}
    anchor = 0
    cursor = 0
    length = len(data)

    def write_length(value):
        while value >= 255:
            output.append(255)
            value -= 255
        output.append(value)

    while cursor + 4 <= length:
        sequence = int.from_bytes(data[cursor : cursor + 4], "little")
        key = ((sequence * 2654435761) & 0xFFFFFFFF) >> 16
        reference = table.get(key)
        table[key] = cursor
        if (
            reference is None
            or cursor - reference > 0xFFFF
            or data[reference : reference + 4] != data[cursor : cursor + 4]
        ):
            cursor += 1
            continue

        match_end = cursor + 4
        reference_end = reference + 4
        while match_end < length and data[match_end] == data[reference_end]:
            match_end += 1
            reference_end += 1
        literal_length = cursor - anchor
        match_length = match_end - cursor - 4
        output.append((min(literal_length, 15) << 4) | min(match_length, 15))
        if literal_length >= 15:
            write_length(literal_length - 15)
        output.extend(data[anchor:cursor])
        output.extend(struct.pack("<H", cursor - reference))
        if match_length >= 15:
            write_length(match_length - 15)

        prior = cursor
        cursor = match_end
        anchor = cursor
        for position in range(prior + 1, min(cursor, length - 3)):
            sequence = int.from_bytes(data[position : position + 4], "little")
            key = ((sequence * 2654435761) & 0xFFFFFFFF) >> 16
            table[key] = position

    literal_length = length - anchor
    output.append(min(literal_length, 15) << 4)
    if literal_length >= 15:
        write_length(literal_length - 15)
    output.extend(data[anchor:])
    return bytes(output)


def build_staged_image(target, source, env):
    helper_path = str(source[0])
    target_path = str(target[0])
    # link2bin emits image_firmware_slot.bin as a side effect of building the
    # main ELF, so it must not be declared as a SCons source node: on a clean
    # build SCons checks source nodes before that side effect exists.
    firmware_path = join(os.path.dirname(target_path), "image_firmware_slot.bin")
    flash_layout = env.BoardConfig().get("flash", {})
    ota_range = flash_layout.get("ota")
    app_range = flash_layout.get("app")
    if (
        not ota_range
        or "+" not in ota_range
        or not app_range
        or "+" not in app_range
    ):
        print("BL602 staged OTA requires board app and OTA partition definitions")
        return 1
    _, ota_size = ota_range.split("+", 1)
    stage_capacity = int(ota_size, 0)
    _, app_size = app_range.split("+", 1)
    app_capacity = int(app_size, 0)

    def slot_image(path):
        with open(path, "rb") as image_file:
            image = image_file.read()
        if len(image) < 0x1000 or image[:4] != b"BFNP":
            raise ValueError(f"{path} is not a BL602 firmware slot image")
        app_length = struct.unpack_from("<I", image, 120)[0]
        image_length = 0x1000 + app_length
        if not app_length or image_length > len(image):
            raise ValueError(f"{path} has an invalid BL602 firmware length")
        return image[:image_length]

    try:
        helper = slot_image(helper_path)
        firmware = slot_image(firmware_path)
        if len(firmware) > app_capacity:
            raise ValueError(
                f"firmware needs {len(firmware)} bytes but the BL602 app slot "
                f"holds {app_capacity} bytes"
            )
        compressed = lz4_compress_block(firmware)
        manifest_size = 96
        payload_offset = len(helper) + manifest_size
        if payload_offset > 0xFFFFFFFF or len(compressed) > 0xFFFFFFFF:
            raise ValueError("staged OTA image exceeds the BL602 manifest format")
        manifest = bytearray(
            struct.pack(
                "<8sHHIII32s32s4sI",
                b"LT602STG",
                1,
                manifest_size,
                payload_offset,
                len(compressed),
                len(firmware),
                hashlib.sha256(compressed).digest(),
                hashlib.sha256(firmware).digest(),
                b"\0" * 4,
                0,
            )
        )
        struct.pack_into("<I", manifest, manifest_size - 4, zlib.crc32(manifest[:-4]))
        capsule = helper + manifest + compressed
        if len(capsule) > stage_capacity:
            raise ValueError(
                f"staged OTA needs {len(capsule)} bytes but the BL602 OTA slot "
                f"holds {stage_capacity} bytes"
            )
        with open(target_path, "wb") as output_file:
            output_file.write(capsule)
        print(
            "BL602 staged OTA: "
            f"updater={len(helper)} compressed={len(compressed)} "
            f"firmware={len(firmware)} capsule={len(capsule)}/{stage_capacity} bytes"
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"BL602 staged OTA packaging failed: {error}")
        return 1
    return 0
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
env.BoardConfig().update("build.prefix", prefix)
env.ExportBoardData(env.BoardConfig())
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
    LINKFLAGS=arch + [
        "-nostartfiles",
        "-Wl,--wrap=vPortDefineHeapRegions",
        # LTO may introduce putchar calls after archive symbol selection.
        "-Wl,--undefined=__wrap_putchar",
    ],
    CPPPATH=includes,
    CPPDEFINES=defines,
    LIBPATH=[join(sdk, "system", "lib")],
)
# The SDK tries to mount ROMFS at startup; skip it when the board has no
# media partition.
if not env.BoardConfig().get("flash.media", None):
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

# Build a second, minimal BL602 image. The image is intentionally separate
# from the consumer firmware so the OTA slot only needs to hold this updater
# plus the compressed consumer image.
stage_helper_dir = join("$BUILD_DIR", "bl602_stage_helper")
os.makedirs(env.subst(stage_helper_dir), exist_ok=True)
stage_helper_env = env.Clone()
stage_helper_env.Replace(
    LINKFLAGS=[
        flag
        for flag in env["LINKFLAGS"]
        if "wrap" not in flag and "--undefined=__wrap_" not in flag
    ],
    LIBS=[
        "freertos_riscv_ram",
        "utils",
        "bl602",
        "bl602_std",
        "mbedtls_lts",
        "m",
    ],
)
stage_helper_env.Prepend(
    LINKFLAGS=[
        "-T",
        env.subst("$LDSCRIPT_PATH"),
        "-Wl,--gc-sections",
        # The SDK linker script defaults to 2 KiB, which is too small for
        # nested SHA/UART calls during the standalone OTA helper.
        "-Wl,--defsym=__stack_size=0x4000",
    ]
)
stage_startup = startup.replace("\tcall bfl_main", "\tcall lt_bl602_ota_stage_main")
if stage_startup == startup:
    raise RuntimeError("Unexpected BL602 startup; cannot redirect updater entry point")
stage_startup_path = join(env.subst(stage_helper_dir), "start.S")
with open(stage_startup_path, "w", encoding="utf-8") as target:
    target.write(stage_startup)


def extract_stage_hal_objects(target, source, env):
    archive = str(source[0])
    directory = os.path.dirname(str(target[0]))
    members = [os.path.basename(str(path)) for path in target]
    subprocess.run(
        [env.subst("$AR"), "x", archive, *members],
        cwd=directory,
        check=True,
    )
    return 0


stage_hal_members = [
    "hal_boot2.o",
    "bl_boot2.o",
    "bl_flash.o",
    "bl_irq.o",
	"bl_uart.o",
    "bl_sec_sha.o",
    "bl_sys.o",
]
stage_hal_objects = env.Command(
    [join(stage_helper_dir, member) for member in stage_hal_members],
    join(sdk, "system", "lib", "libhosal.a"),
    extract_stage_hal_objects,
)

stage_helper_elf = stage_helper_env.Program(
    join(stage_helper_dir, "ota_stage_helper.elf"),
    [
        join(env.subst("$FAMILY_DIR"), "base", "ota_stage", "lt_bl602_ota_stage_main.c"),
        join(env.subst("$FAMILY_DIR"), "base", "api", "lt_bl602_ota_stage.c"),
        stage_startup_path,
        *stage_hal_objects,
    ],
)
stage_helper_image = env.Command(
    join(stage_helper_dir, "image_firmware_slot.bin"),
    stage_helper_elf,
    "${LTCHIPTOOL} elf2bin ${BOARD_JSON} $SOURCE 2",
)

# Main firmware outputs and actions
image_app = "${BUILD_DIR}/image_firmware_ota1.bin"
image_slot = "${BUILD_DIR}/image_firmware_slot.bin"
image_staged = "${BUILD_DIR}/image_firmware_staged.bin"

staged_capsule = env.Command(
    image_staged,
    [stage_helper_image, join("$BUILD_DIR", "${PROGNAME}.elf")],
    build_staged_image,
)
# BuildUF2OTA is created by builder/main.py after this family script executes.
env.Depends(join("$BUILD_DIR", "firmware.uf2"), staged_capsule)

env.Replace(
    LINK='${LTCHIPTOOL} link2bin ${BOARD_JSON} "" ""',
    UF2OTA=[
        f"{image_app},{image_app}+0x1000=flasher:app,ota",
        f"{image_staged},{image_staged}=device:app,ota",
    ],
)
