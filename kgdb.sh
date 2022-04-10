#!/bin/bash
# note : prerequisite - cgdb, gdb also works though

SMDK_KERNEL_PATH=../linux-5.17-rc5-smdk

cgdb -q $SMDK_KERNEL_PATH/vmlinux -ex 'target remote localhost:12345'
#gdb -q $SMDK_KERNEL_PATH/vmlinux -ex 'target remote /dev/pts/4'
