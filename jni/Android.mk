LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := OpenMAXSLDemo
LOCAL_SRC_FILES := OpenMAXSLDemo.c


LOCAL_LDLIBS := -llog
# for native windows
LOCAL_LDLIBS    += -landroid
# for native multimedia
LOCAL_LDLIBS    += -lOpenMAXAL


include $(BUILD_SHARED_LIBRARY)
