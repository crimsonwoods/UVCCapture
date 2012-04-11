LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE      := uvccap
LOCAL_CFLAGS      := -Werror -Wall -O2
LOCAL_SRC_FILES   := uvccap_main.c uvccap.c
LOCAL_LDLIBS      := -llog

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE      := uvcc
LOCAL_CFLAGS      := -Werror -Wall -O2
LOCAL_SRC_FILES   := uvccap.c
LOCAL_LDLIBS      := -llog

include $(BUILD_SHARED_LIBRARY)
