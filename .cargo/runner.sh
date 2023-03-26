#! /bin/bash

KERNEL=$1

LIMINE_GIT_URL="https://github.com/limine-bootloader/limine.git"

cp $KERNEL conf/limine.cfg target/limine/limine{.sys,-cd.bin,-cd-efi.bin} target/iso_root

if [ ! -d target/limine ]; then
    git clone $LIMINE_GIT_URL --depth=1 --branch v3.0-branch-binary target/limine
fi

cd target/limine
git fetch
make
cd -

mkdir -p target/iso_root
cp $KERNEL conf/limine.cfg target/limine/limine{.sys,-cd.bin,-cd-efi.bin} target/iso_root

xorriso -as mkisofs                                             \
    -b limine-cd.bin                                            \
    -no-emul-boot -boot-load-size 4 -boot-info-table            \
    --efi-boot limine-cd-efi.bin                                \
    -efi-boot-part --efi-boot-image --protective-msdos-label    \
    target/iso_root -o $KERNEL.iso

target/limine/limine-deploy $KERNEL.iso

qemu-system-x86_64 -D target/log.txt $KERNEL.iso
#qemu-system-x86_64 -D target/log.txt -s -S $KERNEL.iso
