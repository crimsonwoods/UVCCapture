LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE      := uvccap
LOCAL_CFLAGS      := -Werror -Wall -O2
LOCAL_SRC_FILES   := uvccap.c
LOCAL_LDLIBS      := -llog

include $(BUILD_EXECUTABLE)

