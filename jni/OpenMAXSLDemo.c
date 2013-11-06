/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This is a JNI example where we use native methods to play video
 * using OpenMAX AL. See the corresponding Java source file located at:
 *
 *   src/com/example/nativemedia/NativeMedia/NativeMedia.java
 *
 * In this example we use assert() for "impossible" error conditions,
 * and explicit handling and recovery for more likely error conditions.
 */

#include <jni.h>
#include <stdio.h>
#include <assert.h>

#include <pthread.h>
// for native media
#include <OMXAL/OpenMAXAL.h>
#include <OMXAL/OpenMAXAL_Android.h>

// for native window JNI
#include <android/native_window_jni.h>

#include "mylog.h"

// engine interfaces
static XAObjectItf engineObject = NULL;
static XAEngineItf engineEngine = NULL;
// output mix interfaces
static XAObjectItf outputMixObject = NULL;
// streaming media player interfaces
static XAObjectItf             playerObj = NULL;
static XAPlayItf               playerPlayItf = NULL;
static XAAndroidBufferQueueItf playerBQItf = NULL;
static XAVolumeItf             playerVolItf = NULL;

// video sink for the player
static ANativeWindow* theNativeWindow;

// number of required interfaces for the MediaPlayer creation
#define NB_MAXAL_INTERFACES 2 // XAAndroidBufferQueueItf, XAStreamInformationItf and XAPlayItf
// number of buffers in our buffer queue, an arbitrary number
#define NB_BUFFERS 8
// number of MPEG-2 transport stream blocks per buffer, an arbitrary number
#define PACKETS_PER_BUFFER 10
// we're streaming MPEG-2 transport stream data, operate on transport stream block size
#define MPEG2_TS_PACKET_SIZE 188
// determines how much memory we're dedicating to memory caching
#define BUFFER_SIZE (PACKETS_PER_BUFFER*MPEG2_TS_PACKET_SIZE)
// where we cache in memory the data to play
// note this memory is re-used by the buffer queue callback
static char dataCache[BUFFER_SIZE * NB_BUFFERS];
// handle of the file to play
static FILE *file;
// has the app reached the end of the file
static jboolean reachedEof = JNI_FALSE;
// whether a discontinuity is in progress
static jboolean discontinuity = JNI_FALSE;
// constant to identify a buffer context which is the end of the stream to decode
static const int kEosBufferCntxt = 1980; // a magic value we can compare against

// For mutual exclusion between callback thread and application thread(s).
// The mutex protects reachedEof, discontinuity,
// The condition is signalled when a discontinuity is acknowledged.
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

void naShutdown(JNIEnv* env, jclass clazz);
void naSetSurface(JNIEnv *env, jclass clazz, jobject surface);
void naSetPlayingStreamingMediaPlayer(JNIEnv* env, jclass clazz, jboolean isPlaying);
void naRewindStreamingMediaPlayer(JNIEnv *env, jclass clazz);
jboolean naCreateStreamingMediaPlayer(JNIEnv* env, jclass clazz, jstring filename);
void naCreateEngine(JNIEnv* env, jclass clazz);



jint JNI_OnLoad(JavaVM* pVm, void* reserved) {


	JNIEnv* env;
	if ((*pVm)->GetEnv(pVm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		 return -1;
	}


	JNINativeMethod nm[6];
	nm[0].name = "naCreateEngine";
	nm[0].signature = "()V";
	nm[0].fnPtr = (void*)naCreateEngine;

	nm[1].name = "naShutdown";
	nm[1].signature = "()V";
	nm[1].fnPtr = (void*)naShutdown;

	nm[2].name = "naSetSurface";
	nm[2].signature = "(Landroid/view/Surface;)V";
	nm[2].fnPtr = (void*)naSetSurface;

	nm[3].name = "naCreateStreamingMediaPlayer";
	nm[3].signature = "(Ljava/lang/String;)Z";
	nm[3].fnPtr = (void*)naCreateStreamingMediaPlayer;

	nm[4].name = "naSetPlayingStreamingMediaPlayer";
	nm[4].signature = "(Z)V";
	nm[4].fnPtr = (void*)naSetPlayingStreamingMediaPlayer;

	nm[5].name = "naRewindStreamingMediaPlayer";
	nm[5].signature = "()V";
	nm[5].fnPtr = (void*)naRewindStreamingMediaPlayer;


	jclass cls = (*env)->FindClass(env, "cookbook/chapter7/openmaxsldemo/MainActivity");
	// Register methods with env->RegisterNatives.
	(*env)->RegisterNatives(env, cls, nm, 6);
	return JNI_VERSION_1_6;

}


void naCreateEngine(JNIEnv* env, jclass clazz) {
    XAresult res;
    // create engine
    res = xaCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(XA_RESULT_SUCCESS == res);
    // realize the engine
    res = (*engineObject)->Realize(engineObject, XA_BOOLEAN_FALSE);
    assert(XA_RESULT_SUCCESS == res);
    // get the engine interface, which is needed in order to create other objects
    res = (*engineObject)->GetInterface(engineObject, XA_IID_ENGINE, &engineEngine);
    assert(XA_RESULT_SUCCESS == res);
    // create output mix
    res = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, NULL, NULL);
    assert(XA_RESULT_SUCCESS == res);
    // realize the output mix
    res = (*outputMixObject)->Realize(outputMixObject, XA_BOOLEAN_FALSE);
    assert(XA_RESULT_SUCCESS == res);

}

// Enqueue the initial buffers, and optionally signal a discontinuity in the first buffer
jboolean enqueueInitialBuffers(jboolean discontinuity) {
    /* Fill our cache.
     * We want to read whole packets (integral multiples of MPEG2_TS_PACKET_SIZE).
     * fread returns units of "elements" not bytes, so we ask for 1-byte elements
     * and then check that the number of elements is a multiple of the packet size.
     */
    size_t bytesRead;
    bytesRead = fread(dataCache, 1, BUFFER_SIZE * NB_BUFFERS, file);
    if (bytesRead <= 0) {
        // could be premature EOF or I/O error
        return JNI_FALSE;
    }
    if ((bytesRead % MPEG2_TS_PACKET_SIZE) != 0) {
        LOGI(1, "Dropping last packet because it is not whole");
    }
    size_t packetsRead = bytesRead / MPEG2_TS_PACKET_SIZE;
    LOGI(1, "Initially queueing %u packets", packetsRead);
    /* Enqueue the content of our cache before starting to play,
       we don't want to starve the player */
    size_t i;
    for (i = 0; i < NB_BUFFERS && packetsRead > 0; i++) {
        // compute size of this buffer
        size_t packetsThisBuffer = packetsRead;
        if (packetsThisBuffer > PACKETS_PER_BUFFER) {
            packetsThisBuffer = PACKETS_PER_BUFFER;
        }
        size_t bufferSize = packetsThisBuffer * MPEG2_TS_PACKET_SIZE;
        XAresult res;
        if (discontinuity) {
            // signal discontinuity
            XAAndroidBufferItem items[1];
            items[0].itemKey = XA_ANDROID_ITEMKEY_DISCONTINUITY;
            items[0].itemSize = 0;
            // DISCONTINUITY message has no parameters,
            //   so the total size of the message is the size of the key
            //   plus the size if itemSize, both XAuint32
            res = (*playerBQItf)->Enqueue(playerBQItf, NULL /*pBufferContext*/,
                    dataCache + i*BUFFER_SIZE, bufferSize, items /*pMsg*/,
                    sizeof(XAuint32)*2 /*msgLength*/);
            discontinuity = JNI_FALSE;
        } else {
            res = (*playerBQItf)->Enqueue(playerBQItf, NULL /*pBufferContext*/,
                    dataCache + i*BUFFER_SIZE, bufferSize, NULL, 0);
        }
        assert(XA_RESULT_SUCCESS == res);
        packetsRead -= packetsThisBuffer;
    }

    return JNI_TRUE;
}

// AndroidBufferQueueItf callback to supply MPEG-2 TS packets to the media player
XAresult AndroidBufferQueueCallback(
        XAAndroidBufferQueueItf caller,
        void *pCallbackContext,        /* input */
        void *pBufferContext,          /* input */
        void *pBufferData,             /* input */
        XAuint32 dataSize,             /* input */
        XAuint32 dataUsed,             /* input */
        const XAAndroidBufferItem *pItems,/* input */
        XAuint32 itemsLength           /* input */) {
    XAresult res;
    int ok;
    // pCallbackContext was specified as NULL at RegisterCallback and is unused here
    assert(NULL == pCallbackContext);
    // note there is never any contention on this mutex unless a discontinuity request is active
    ok = pthread_mutex_lock(&mutex);
    assert(0 == ok);
    // was a discontinuity requested?
    if (discontinuity) {
        // Note: can't rewind after EOS, which we send when reaching EOF
        // (don't send EOS if you plan to play more content through the same player)
        if (!reachedEof) {
            // clear the buffer queue
            res = (*playerBQItf)->Clear(playerBQItf);
            assert(XA_RESULT_SUCCESS == res);
            // rewind the data source so we are guaranteed to be at an appropriate point
            rewind(file);
            // Enqueue the initial buffers, with a discontinuity indicator on first buffer
            (void) enqueueInitialBuffers(JNI_TRUE);
        }
        // acknowledge the discontinuity request
        discontinuity = JNI_FALSE;
        ok = pthread_cond_signal(&cond);
        assert(0 == ok);
        goto exit;
    }
    if ((pBufferData == NULL) && (pBufferContext != NULL)) {
        const int processedCommand = *(int *)pBufferContext;
        if (kEosBufferCntxt == processedCommand) {
            LOGI(1, "EOS was processed\n");
            // our buffer with the EOS message has been consumed
            assert(0 == dataSize);
            goto exit;
        }
    }
    // pBufferData is a pointer to a buffer that we previously Enqueued
    assert((dataSize > 0) && ((dataSize % MPEG2_TS_PACKET_SIZE) == 0));
    assert(dataCache <= (char *) pBufferData && (char *) pBufferData <
            &dataCache[BUFFER_SIZE * NB_BUFFERS]);
    assert(0 == (((char *) pBufferData - dataCache) % BUFFER_SIZE));
    // don't bother trying to read more data once we've hit EOF
    if (reachedEof) {
        goto exit;
    }
    size_t nbRead;
    // note we do call fread from multiple threads, but never concurrently
    size_t bytesRead;
    bytesRead = fread(pBufferData, 1, BUFFER_SIZE, file);
    if (bytesRead > 0) {
        if ((bytesRead % MPEG2_TS_PACKET_SIZE) != 0) {
            LOGI(2, "Dropping last packet because it is not whole");
        }
        size_t packetsRead = bytesRead / MPEG2_TS_PACKET_SIZE;
        size_t bufferSize = packetsRead * MPEG2_TS_PACKET_SIZE;
        res = (*caller)->Enqueue(caller, NULL /*pBufferContext*/,
                pBufferData /*pData*/,
                bufferSize /*dataLength*/,
                NULL /*pMsg*/,
                0 /*msgLength*/);
        assert(XA_RESULT_SUCCESS == res);
    } else {
        // EOF or I/O error, signal EOS
        XAAndroidBufferItem msgEos[1];
        msgEos[0].itemKey = XA_ANDROID_ITEMKEY_EOS;
        msgEos[0].itemSize = 0;
        // EOS message has no parameters, so the total size of the message is the size of the key
        //   plus the size if itemSize, both XAuint32
        res = (*caller)->Enqueue(caller, (void *)&kEosBufferCntxt /*pBufferContext*/,
                NULL /*pData*/, 0 /*dataLength*/,
                msgEos /*pMsg*/,
                sizeof(XAuint32)*2 /*msgWWLength*/);
        assert(XA_RESULT_SUCCESS == res);
        reachedEof = JNI_TRUE;
    }
exit:
    ok = pthread_mutex_unlock(&mutex);
    assert(0 == ok);
    return XA_RESULT_SUCCESS;
}

jboolean naCreateStreamingMediaPlayer(JNIEnv* env, jclass clazz, jstring filename) {
    XAresult res;
    // convert Java string to UTF-8
    const char *utf8FileName = (*env)->GetStringUTFChars(env, filename, NULL);
    assert(NULL != utf8);
    // open the file to play
    file = fopen(utf8FileName, "rb");
    if (file == NULL) {
    	LOGE(1, "cannot open file %s", utf8FileName);
        return JNI_FALSE;
    }
    // configure data source
    XADataLocator_AndroidBufferQueue loc_abq = { XA_DATALOCATOR_ANDROIDBUFFERQUEUE, NB_BUFFERS };
    XADataFormat_MIME format_mime = {
            XA_DATAFORMAT_MIME, XA_ANDROID_MIME_MP2TS, XA_CONTAINERTYPE_MPEG_TS };
    XADataSource dataSrc = {&loc_abq, &format_mime};
    // configure audio sink
    XADataLocator_OutputMix loc_outmix = { XA_DATALOCATOR_OUTPUTMIX, outputMixObject };
    XADataSink audioSnk = { &loc_outmix, NULL };
    // configure image video sink
    XADataLocator_NativeDisplay loc_nd = {
            XA_DATALOCATOR_NATIVEDISPLAY,        // locatorType
            // the video sink must be an ANativeWindow created from a Surface or SurfaceTexture
            (void*)theNativeWindow,              // hWindow
            // must be NULL
            NULL                                 // hDisplay
    };
    XADataSink imageVideoSink = {&loc_nd, NULL};
    // declare interfaces to use
    XAboolean required[NB_MAXAL_INTERFACES]
        = {XA_BOOLEAN_TRUE, XA_BOOLEAN_TRUE};
    XAInterfaceID iidArray[NB_MAXAL_INTERFACES]
        = {XA_IID_PLAY, XA_IID_ANDROIDBUFFERQUEUESOURCE};
    // create media player
    res = (*engineEngine)->CreateMediaPlayer(engineEngine, 	//interface self-reference
	    &playerObj, 	//newly created media player object
	    &dataSrc,		//data source
            NULL, 		//ignored for non-MIDI data sources
	    &audioSnk, 		//audio data sink, such as an audio output device
	    &imageVideoSink, 	//image/video data sink, such as a native window handle
	    NULL, 		//vibra I/O device to which media player should send vibration data
	    NULL,		//LED array I/O device to which media player should send LED array data
            NB_MAXAL_INTERFACES, //number of interfaces to support, not including implicit interfaces
            iidArray, 	//array of interface IDs should support
            required 	//array of flags, indicating whether the respective interface is required
	);
    assert(XA_RESULT_SUCCESS == res);
    // release the Java string and UTF-8
    (*env)->ReleaseStringUTFChars(env, filename, utf8FileName);
    // realize the player
    res = (*playerObj)->Realize(playerObj, XA_BOOLEAN_FALSE);
    assert(XA_RESULT_SUCCESS == res);
    // get the play interface
    res = (*playerObj)->GetInterface(playerObj, XA_IID_PLAY, &playerPlayItf);
    assert(XA_RESULT_SUCCESS == res);
    // get the Android buffer queue interface
    res = (*playerObj)->GetInterface(playerObj, XA_IID_ANDROIDBUFFERQUEUESOURCE, &playerBQItf);
    assert(XA_RESULT_SUCCESS == res);
    // specify which events we want to be notified of
    res = (*playerBQItf)->SetCallbackEventsMask(playerBQItf, XA_ANDROIDBUFFERQUEUEEVENT_PROCESSED);
    assert(XA_RESULT_SUCCESS == res);
    // register the callback from which OpenMAX AL can retrieve the data to play
    res = (*playerBQItf)->RegisterCallback(playerBQItf, AndroidBufferQueueCallback, NULL);
    assert(XA_RESULT_SUCCESS == res);
    // enqueue the initial buffers
    if (!enqueueInitialBuffers(JNI_FALSE)) {
        return JNI_FALSE;
    }
    // prepare the player
    res = (*playerPlayItf)->SetPlayState(playerPlayItf, XA_PLAYSTATE_PAUSED);
    assert(XA_RESULT_SUCCESS == res);
    // start the playback
    res = (*playerPlayItf)->SetPlayState(playerPlayItf, XA_PLAYSTATE_PLAYING);
    assert(XA_RESULT_SUCCESS == res);
    return JNI_TRUE;
}

// rewind the streaming media player
void naRewindStreamingMediaPlayer(JNIEnv *env, jclass clazz) {
    XAresult res;
    // make sure the streaming media player was created
    if (NULL != playerBQItf && NULL != file) {
        // first wait for buffers currently in queue to be drained
        int ok;
        ok = pthread_mutex_lock(&mutex);
        assert(0 == ok);
        discontinuity = JNI_TRUE;
        // wait for discontinuity request to be observed by buffer queue callback
        // Note: can't rewind after EOS, which we send when reaching EOF
        // (don't send EOS if you plan to play more content through the same player)
        while (discontinuity && !reachedEof) {
            ok = pthread_cond_wait(&cond, &mutex);
            assert(0 == ok);
        }
        ok = pthread_mutex_unlock(&mutex);
        assert(0 == ok);
    }
}
// set the playing state for the streaming media player
void naSetPlayingStreamingMediaPlayer(JNIEnv* env, jclass clazz, jboolean isPlaying) {
    XAresult res;
    // make sure the streaming media player was created
    if (NULL != playerPlayItf) {
        // set the player's state
        res = (*playerPlayItf)->SetPlayState(playerPlayItf, isPlaying ?
            XA_PLAYSTATE_PLAYING : XA_PLAYSTATE_PAUSED);
        assert(XA_RESULT_SUCCESS == res);
    }
}
// set the surface
void naSetSurface(JNIEnv *env, jclass clazz, jobject surface) {
    // obtain a native window from a Java surface
    theNativeWindow = ANativeWindow_fromSurface(env, surface);
}
// shut down the native media system
void naShutdown(JNIEnv* env, jclass clazz) {
    // destroy streaming media player object, and invalidate all associated interfaces
    if (playerObj != NULL) {
        (*playerObj)->Destroy(playerObj);
        playerObj = NULL;
        playerPlayItf = NULL;
        playerBQItf = NULL;
        playerVolItf = NULL;
    }
    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }
    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
    // close the file
    if (file != NULL) {
        fclose(file);
        file = NULL;
    }
    // make sure we don't leak native windows
    if (theNativeWindow != NULL) {
        ANativeWindow_release(theNativeWindow);
        theNativeWindow = NULL;
    }
}
