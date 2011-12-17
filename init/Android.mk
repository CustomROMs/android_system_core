# Copyright 2005 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)

# --

ifneq (,$(filter userdebug eng,$(TARGET_BUILD_VARIANT)))
init_options += \
    -DALLOW_LOCAL_PROP_OVERRIDE=1 \
    -DALLOW_PERMISSIVE_SELINUX=1 \
    -DREBOOT_BOOTLOADER_ON_PANIC=0 \
    -DDUMP_ON_UMOUNT_FAILURE=1
else
init_options += \
    -DALLOW_LOCAL_PROP_OVERRIDE=0 \
    -DALLOW_PERMISSIVE_SELINUX=0 \
    -DREBOOT_BOOTLOADER_ON_PANIC=0 \
    -DDUMP_ON_UMOUNT_FAILURE=0
endif

ifneq (,$(filter eng,$(TARGET_BUILD_VARIANT)))
init_options += \
    -DSHUTDOWN_ZERO_TIMEOUT=1
else
init_options += \
    -DSHUTDOWN_ZERO_TIMEOUT=0
endif

init_options += -DLOG_UEVENTS=0

ifeq ($(TARGET_USER_MODE_LINUX), true)
    init_cflags += -DUSER_MODE_LINUX
endif

init_cflags += \
    $(init_options) \
    -Wall -Wextra \
    -Wno-unused-parameter \
    -std=gnu++1z

# --

include $(CLEAR_VARS)
LOCAL_CPPFLAGS := $(init_cflags)
LOCAL_SRC_FILES:= \
    bootchart.cpp \
    builtins.cpp \
    init.cpp \
    init_first_stage.cpp \
    keychords.cpp \
    property_service.cpp \
    reboot.cpp \
    signal_handler.cpp \
    ueventd.cpp \
    watchdogd.cpp \
    vendor_init.cpp

ifeq ($(KERNEL_HAS_FINIT_MODULE), false)
LOCAL_CFLAGS += -DNO_FINIT_MODULE
endif

SYSTEM_CORE_INIT_DEFINES := BOARD_CHARGING_MODE_BOOTING_LPM \
    BOARD_CHARGING_CMDLINE_NAME \
    BOARD_CHARGING_CMDLINE_VALUE

$(foreach system_core_init_define,$(SYSTEM_CORE_INIT_DEFINES), \
  $(if $($(system_core_init_define)), \
    $(eval LOCAL_CFLAGS += -D$(system_core_init_define)=\"$($(system_core_init_define))\") \
  ) \
)

LOCAL_MODULE:= init
LOCAL_C_INCLUDES += \
    system/core/mkbootimg

LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_UNSTRIPPED)

LOCAL_STATIC_LIBRARIES := \
    libinit \
    libbootloader_message \
    libfs_mgr \
    libfec \
    libfec_rs \
    libsquashfs_utils \
    liblogwrap \
    libext4_utils \
    libcutils \
    libbase \
    libc \
    libselinux \
    liblog \
    libcrypto_utils \
    libcrypto \
    libc++_static \
    libdl \
    libsparse \
    libz \
    libprocessgroup \
    libavb \
    libkeyutils \

LOCAL_REQUIRED_MODULES := \
    e2fsdroid \
    mke2fs \
    sload_f2fs \
    make_f2fs \

# Create symlinks.
LOCAL_POST_INSTALL_CMD := $(hide) mkdir -p $(TARGET_ROOT_OUT)/sbin; \
    ln -sf ../init $(TARGET_ROOT_OUT)/sbin/ueventd; \
    ln -sf ../init $(TARGET_ROOT_OUT)/sbin/watchdogd

LOCAL_SANITIZE := signed-integer-overflow
LOCAL_CLANG := true

ifneq ($(strip $(TARGET_INIT_VENDOR_LIB)),)
LOCAL_WHOLE_STATIC_LIBRARIES += $(TARGET_INIT_VENDOR_LIB)
endif

include $(BUILD_EXECUTABLE)
