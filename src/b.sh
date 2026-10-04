#!/usr/bin/env bash

set -e

DO_CLEAN=0
DO_BUILD=0
DO_TEST=0
DO_RUN=0

for arg in "$@"; do
    case "$arg" in
        -c)   DO_CLEAN=1 ;;
        -cr)  DO_CLEAN=1; DO_BUILD=1 ;;
        -crt) DO_CLEAN=1; DO_BUILD=1; DO_TEST=1 ;;
        -r)   DO_RUN=1 ;;
        *) echo "unknown: $arg"; exit 1 ;;
    esac
done

OUT=OUTBIN
SHELL_SRC=API-LIB/Bin/poser_shell
LIMASM_SRC=API-LIB/Bin/limasm
DASM_SRC=API-LIB/Bin/dasm
RUNDWN_SRC=API-LIB/Bin/rundwn
RUNPACK_SRC=API-LIB/Bin/runpack
PACK_SRC=API-LIB/Bin/pack
DAWNVM_SRC=API-LIB/Bin/dawnvm
TEST_SRC=API-LIB/Bin/tests

# persistent hard-disk image (qcow2). Reused as-is between builds; override
# with the DISK env var, e.g.  DISK=/tmp/other.qcow2 ./b.sh -cr
DISK="${DISK:-/storage/emulated/0/文件减/000/Studio/OS/PoserOS/hello.qcow2}"

# .bss 字节数 = 各 LOAD 段的 (memsz - filesz) 之和。PEXC 头要把这个值填进
# bss_size，内核加载时才会把 .bss 一并映射并清零，否则程序一碰 .bss 就页错误。
bss_size() {
    python3 - "$1" <<'PY'
import subprocess, sys
out = subprocess.check_output(["readelf", "-lW", sys.argv[1]]).decode()
s = 0
for line in out.splitlines():
    f = line.split()
    if f and f[0] == "LOAD":
        filesz = int(f[4], 16); memsz = int(f[5], 16)
        if memsz > filesz:
            s += memsz - filesz
print(s)
PY
}

# wrap a raw flat binary into a PEXC executable image
#   mkexc <bin> <exc> [bss_size]
mkexc() {
    python3 - "$1" "$2" "${3:-0}" <<'PY'
import struct, sys, time
src, dst, bss = sys.argv[1], sys.argv[2], int(sys.argv[3])
code = open(src, "rb").read()
t = time.localtime()
dos = ((t.tm_year - 1980) << 25) | (t.tm_mon << 21) | (t.tm_mday << 16) \
      | (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
h = bytearray(64)
h[0:4] = b"PEXC"
struct.pack_into("<H", h, 4, 1)            # version
struct.pack_into("<H", h, 6, 64)           # hdr_size
struct.pack_into("<I", h, 8, 0x86)         # arch x86
struct.pack_into("<I", h, 12, 32)          # bits
struct.pack_into("<I", h, 16, dos)         # created (DOS time)
struct.pack_into("<I", h, 20, dos)         # modified
struct.pack_into("<I", h, 24, 0x04000000)  # entry
struct.pack_into("<I", h, 28, 0x04000000)  # load
struct.pack_into("<I", h, 32, len(code))   # code_size
struct.pack_into("<I", h, 36, bss)         # bss_size
struct.pack_into("<I", h, 48, 0)           # priv = user
struct.pack_into("<I", h, 52, 1)           # flags: ring0 allowed
s = 2166136261
for b in h:
    s ^= b; s = (s * 16777619) & 0xFFFFFFFF
for b in code:
    s ^= b; s = (s * 16777619) & 0xFFFFFFFF
struct.pack_into("<I", h, 56, s)           # checksum
open(dst, "wb").write(bytes(h) + code)
PY
}

FS_OFFSET=67108864

# write the install payload (system image + built-in programs) as a linear
# blob at LBA 0 of the raw disk.  arguments are  name=path  pairs.
# layout: sector 0 header, sectors 1.. TOC (8 x 64-byte entries), then data.
mkpayload() {
    python3 - "$@" <<'PY'
import struct, sys

raw = sys.argv[1]
ents = []
for a in sys.argv[2:]:
    name, path = a.split("=", 1)
    try:
        with open(path, "rb") as f:
            ents.append((name, f.read()))
    except OSError:
        pass

count = len(ents)
toc_lba = 1
data_lba = toc_lba + (count + 7) // 8

hdr = bytearray(512)
hdr[0:4] = b"PINS"
struct.pack_into("<I", hdr, 4, 1)          # version
struct.pack_into("<I", hdr, 8, count)
struct.pack_into("<I", hdr, 12, toc_lba)

toc = bytearray()
body = bytearray()
sector = data_lba
for name, data in ents:
    nm = name.encode()[:47]
    ent = bytearray(64)
    ent[0:len(nm)] = nm
    struct.pack_into("<I", ent, 48, sector)
    struct.pack_into("<I", ent, 52, len(data))
    struct.pack_into("<I", ent, 56, 0)     # 0 = file, 1 = dir
    toc += ent
    body += data
    body += b"\x00" * ((-len(data)) % 512)
    sector += (len(data) + 511) // 512

# the TOC must fill whole sectors, otherwise the first data sector recorded
# in the entries no longer matches where the body actually starts
toc += b"\x00" * ((-len(toc)) % 512)
data_lba = toc_lba + len(toc) // 512

total = data_lba + len(body) // 512
struct.pack_into("<I", hdr, 16, total)     # sectors to release after install

with open(raw, "r+b") as f:
    f.write(bytes(hdr) + bytes(toc) + bytes(body))
print("  payload: %d files, %d sectors" % (count, total))
PY
}

# update the persistent qcow2 disk without recreating it: convert to a raw
# scratch copy, format the FAT volume and lay down the install payload, then
# convert back.  The kernel installs the payload into the FAT on first boot.
update_disk() {
    raw="$OUT/disk.raw"

    if [ -f "$DISK" ]; then
        echo "  reusing $DISK"
        qemu-img convert -f qcow2 -O raw "$DISK" "$raw"
    else
        echo "  creating $DISK (first time only)"
        mkdir -p "$(dirname "$DISK")"
        dd if=/dev/zero of="$raw" bs=1M count=128 status=none
    fi

    if ! mdir -i "$raw@@$FS_OFFSET" :: >/dev/null 2>&1; then
        mformat -i "$raw@@$FS_OFFSET" ::
    fi

    # drop the install marker so the next boot reinstalls the fresh payload
    mdel -i "$raw@@$FS_OFFSET" ::/system/installed 2>/dev/null || true

    mkpayload "$raw" \
        "/system/kernel.bin=$OUT/kernel.bin" \
        "shell.bin=$OUT/shell.bin" \
        "/system/bin/limasm.exc=$OUT/limasm.exc" \
        "/system/bin/dasm.exc=$OUT/dasm.exc" \
        "/system/bin/rundwn.exc=$OUT/rundwn.exc" \
        "/system/bin/runpack.exc=$OUT/runpack.exc" \
        "/user/home/hello.dasm=$DASM_SRC/hello.dasm" \
        "/user/home/syscall.asm=$LIMASM_SRC/syscall_demo.asm" \
        "/user/home/syscalls.txt=SYSCALLS.txt" \
        "/user/home/t.exc=$OUT/syscall_test.exc"

    qemu-img convert -f raw -O qcow2 "$raw" "$DISK"
    rm -f "$raw"
    echo "  disk: $(stat -c%s "$DISK") bytes qcow2"
}

FLOPPY_SECTORS=2880
MAX_KERNEL_SECTORS=1152

CFLAGS=(
    --target=i686-unknown-linux-gnu
    -m32
    -ffreestanding
    -fno-pic
    -fno-stack-protector
    -mno-sse
    -mno-sse2
    -mno-mmx
    -mno-80387
    -I.
    -IAPI-LIB
    -IAPI-LIB/Pstd
)

if [ "$DO_CLEAN" = "1" ]; then
    echo "clean"
    rm -rf "$OUT"
fi

if [ "$DO_BUILD" = "1" ]; then
    mkdir -p "$OUT"

    echo "cc"
    clang "${CFLAGS[@]}" -c kernel.c -o $OUT/kernel.o
    clang "${CFLAGS[@]}" -c $SHELL_SRC/shell.c -o $OUT/shell.o
    clang "${CFLAGS[@]}" -c $SHELL_SRC/edstyle.c -o $OUT/edstyle.o
    clang "${CFLAGS[@]}" -c $LIMASM_SRC/limasm.c -o $OUT/limasm.o

    echo "asm"
    nasm -f elf32 kload.asm -o $OUT/kload.o
    nasm -f elf32 intr.asm  -o $OUT/intr.o
    nasm -f elf32 proc.asm  -o $OUT/proc.o

    echo "ld"
    ld.lld -m elf_i386 -T linker.ld \
        $OUT/kload.o $OUT/kernel.o $OUT/intr.o $OUT/proc.o -o $OUT/kernel.elf
    objcopy -O binary $OUT/kernel.elf $OUT/kernel.bin

    echo "shell"
    ld.lld -m elf_i386 -T $SHELL_SRC/shell.ld $OUT/shell.o $OUT/edstyle.o -o $OUT/shell.elf
    objcopy -O binary $OUT/shell.elf $OUT/shell.bin
    echo "shell: $(stat -c%s $OUT/shell.bin) bytes"

    echo "limasm"
    ld.lld -m elf_i386 -T $LIMASM_SRC/limasm.ld $OUT/limasm.o -o $OUT/limasm.elf
    objcopy -O binary $OUT/limasm.elf $OUT/limasm.bin
    mkexc "$OUT/limasm.bin" "$OUT/limasm.exc" "$(bss_size "$OUT/limasm.elf")"
    echo "limasm: $(stat -c%s $OUT/limasm.exc) bytes exc"

    echo "dasm"
    clang "${CFLAGS[@]}" -c $DASM_SRC/dasm.c -o $OUT/dasm.o
    ld.lld -m elf_i386 -T $DASM_SRC/dasm.ld $OUT/dasm.o -o $OUT/dasm.elf
    objcopy -O binary $OUT/dasm.elf $OUT/dasm.bin
    mkexc "$OUT/dasm.bin" "$OUT/dasm.exc" "$(bss_size "$OUT/dasm.elf")"
    echo "dasm: $(stat -c%s $OUT/dasm.exc) bytes exc"

    echo "rundwn"
    clang "${CFLAGS[@]}" -c $RUNDWN_SRC/rundwn.c -o $OUT/rundwn.o
    clang "${CFLAGS[@]}" -c $DAWNVM_SRC/dawnvm.c -o $OUT/dawnvm.o
    clang "${CFLAGS[@]}" -c $DAWNVM_SRC/dawnbc.c -o $OUT/dawnbc.o
    ld.lld -m elf_i386 -T $RUNDWN_SRC/rundwn.ld \
        $OUT/rundwn.o $OUT/dawnvm.o $OUT/dawnbc.o -o $OUT/rundwn.elf
    objcopy -O binary $OUT/rundwn.elf $OUT/rundwn.bin
    mkexc "$OUT/rundwn.bin" "$OUT/rundwn.exc" "$(bss_size "$OUT/rundwn.elf")"
    echo "rundwn: $(stat -c%s $OUT/rundwn.exc) bytes exc"

    echo "runpack"
    clang "${CFLAGS[@]}" -c $RUNPACK_SRC/runpack.c -o $OUT/runpack.o
    clang "${CFLAGS[@]}" -c $PACK_SRC/pack.c -o $OUT/pack.o
    ld.lld -m elf_i386 -T $RUNPACK_SRC/runpack.ld \
        $OUT/runpack.o $OUT/pack.o $OUT/dawnvm.o $OUT/dawnbc.o -o $OUT/runpack.elf
    objcopy -O binary $OUT/runpack.elf $OUT/runpack.bin
    mkexc "$OUT/runpack.bin" "$OUT/runpack.exc" "$(bss_size "$OUT/runpack.elf")"
    echo "runpack: $(stat -c%s $OUT/runpack.exc) bytes exc"

    echo "syscall_test"
    clang "${CFLAGS[@]}" -c $TEST_SRC/syscall_test.c -o $OUT/syscall_test.o
    ld.lld -m elf_i386 -T $TEST_SRC/syscall_test.ld $OUT/syscall_test.o -o $OUT/syscall_test.elf
    objcopy -O binary $OUT/syscall_test.elf $OUT/syscall_test.bin
    mkexc "$OUT/syscall_test.bin" "$OUT/syscall_test.exc" "$(bss_size "$OUT/syscall_test.elf")"
    echo "syscall_test: $(stat -c%s $OUT/syscall_test.exc) bytes exc"

    KERNEL_SECTORS=$(( ($(stat -c%s $OUT/kernel.bin) + 511) / 512 ))
    if [ "$KERNEL_SECTORS" -gt "$MAX_KERNEL_SECTORS" ]; then
        echo "kernel.bin too large: $KERNEL_SECTORS sectors (max $MAX_KERNEL_SECTORS)"
        exit 1
    fi
    echo "kernel: $(stat -c%s $OUT/kernel.bin) bytes = $KERNEL_SECTORS sectors"

    echo "boot"
    nasm -f bin boot.asm -dKERNEL_SECTORS=$KERNEL_SECTORS -o $OUT/boot.bin

    echo "img"
    dd if=/dev/zero of=$OUT/floppy.img bs=512 count=$FLOPPY_SECTORS status=none
    dd if=$OUT/boot.bin of=$OUT/floppy.img bs=512 count=1 conv=notrunc status=none
    dd if=$OUT/kernel.bin of=$OUT/floppy.img bs=512 seek=1 conv=notrunc status=none

    echo "disk"
    update_disk

    echo "done"
fi

if [ "$DO_RUN" = "1" ]; then
    if [ ! -f $OUT/floppy.img ]; then
        echo "$OUT/floppy.img not found, build first"
        exit 1
    fi
    if [ ! -f "$DISK" ]; then
        echo "$DISK not found, build first"
        exit 1
    fi

    if command -v qemu-system-i386 >/dev/null 2>&1; then
        qemu-system-i386 -display curses -m 512 -boot a \
            -drive file=$OUT/floppy.img,format=raw,if=floppy \
            -drive file="$DISK",format=qcow2
    elif command -v qemu-system-x86_64 >/dev/null 2>&1; then
        qemu-system-x86_64 -cpu qemu32 -m 512 -display curses -boot a \
            -drive file=$OUT/floppy.img,format=raw,if=floppy \
            -drive file="$DISK",format=qcow2
    else
        echo "qemu not installed"
        echo "pkg install qemu-system-i386"
        exit 1
    fi
fi
