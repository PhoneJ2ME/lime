/*
 * Copyright  1990-2009 Sun Microsystems, Inc. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 only, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details (a copy is
 * included at /legal/license.txt).
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 * 
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 or visit www.sun.com if you need additional
 * information or have any questions.
 */

#include "sublimeLinuxWin32Bridge.h"

#ifdef WIN32 
#include <jni.h>

void WaitForMutex(MUTEX_HANDLE m){
    WaitForSingleObject(m, INFINITE); 
}

void WaitForEvent(EVENT_HANDLE e){ 
    WaitForSingleObject(e, INFINITE); 
}

/* empty implementation: this method is used only by the linux version */ 
JNIEXPORT void JNICALL Java_com_sun_kvem_Sublime_setTempFilesDirectory(JNIEnv * env, jclass clz, jstring jstr){}

void yield(){
  Sleep(0); 
}

#else /* !WIN32 */ 

#include <unistd.h> 
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include "com_sun_kvem_Sublime.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Directory for temporary files used by Events/SharedBuffers */ 
static char * tempFilesLocation; 

/* After the directory for temporary files was created on the Java part, the path 
 *  should be updated with this function 
*/ 
JNIEXPORT void JNICALL Java_com_sun_kvem_Sublime_setTempFilesDirectory(JNIEnv * env, jclass clz, jstring jstr){
    tempFilesLocation = (char*)malloc((*env)->GetStringUTFLength(env, jstr));
    (*env)->GetStringUTFRegion(env, jstr, 0, (*env)->GetStringUTFLength(env, jstr), tempFilesLocation); 
}

/* if the calling process for this function is the Java process - 
 * the return value is the "tempFilesLocation" var. if it is 
 * a process generated by the Java process - the return value 
 * is an environment var. 
 */ 
char* getTempDirLocation(void){
    char* res = NULL; 
    jstring jstr; 
    if ((res = (char *)getenv("LIME_TMP_DIR")) != NULL){
        return res; 
    }
    return tempFilesLocation; 
}

void itoa(int num, char *buf, int radix){
    int tmp, index, length;
    char c; 
    index = 0 ; 
    while(num != 0) { 
        tmp = num % radix; 
        num = num / radix; 
        buf[index++] = tmp + 48 ;
    }
    buf[index--] = 0; 
    length = index; 
    for (;index > (length - index); index--){ 
        c = buf[index]; 
        buf[index] = buf[length - index];
        buf[length - index] = c; 
    }
}

void WaitForMutex(MUTEX_HANDLE m){
    pthread_mutex_lock(m); 
}

void ReleaseMutex(MUTEX_HANDLE m){
    pthread_mutex_unlock(m); 
}

void WaitForEvent(EVENT_HANDLE e){
    if (e == NULL){
        fprintf(stderr, "WaitForEvent: EVENT_HANDLE is NULL\n");
        fflush(stderr); 
        exit(1); 
    }
    e->waitForEvent(e);
}

void SetEvent(EVENT_HANDLE e){
    if (e == NULL){
        fprintf(stderr,"SetEvent: EVENT_HANDLE is NULL\n");
        fflush(stderr); 
        exit(1); 
    }
    e->setEvent(e); 
}


/* Not like the Win32 implementation - the mutex in the Unix version 
 * is used only within the same process. The Sublime component does not 
 * requires a shared lock between processes. However - if, due to changes of 
 * Sublime implementations, there will be a need to lock between processes - 
 * the mutex can be implemented with a semaphore (sys/sem.h)
 */
MUTEX_HANDLE CreateMutex(void* sa, int initialState, char * name){
    pthread_mutex_t * m =(pthread_mutex_t *) malloc (sizeof(pthread_mutex_t)); 
    if (m == NULL) { 
        fprintf(stderr, "CreateMutex: malloc failed\n"); 
        fflush(stderr); 
        exit(1); 
    }
    pthread_mutex_init(m, NULL); 
    if (initialState != 0) { 
        pthread_mutex_lock(m); 
    }
    return m; 
}

/* Wait/set event is done by reading/writing one char to the FIFO */  
static void sublime_event_setEvent(EVENT_HANDLE e){
    char signal = EVENT_CHAR; 
    if (e== NULL){
        fprintf(stderr, "SetEvent: sublimeEvent is NULL\n");     
        fflush(stderr); 
    }
    if (write(e->pipeDescriptor, &signal, 1) <= 0){
        fprintf(stderr, "SetEvent: write to FIFO failed\n");
        fflush(stderr); 
    } 
}

static void sublime_event_waitForEvent(EVENT_HANDLE e){
    fd_set fds; 
    char c; 
    if (e == NULL){
        fprintf(stderr, "WaitForEvent: sublimeEvent is NULL\n");     
        fflush(stderr); 
        exit(1); 
    } 
    read(e->pipeDescriptor, &c, 1);  
}

EVENT_HANDLE CreateEvent(void* sa, int manualReset, int initialState, char * name){
    char * buf = (char *)malloc(sizeof(char)*PATH_MAX); 
    char * tmpDirBuf; 
    int fd; 
    sublimeEvent *e = (sublimeEvent*)malloc(sizeof(sublimeEvent)); 
    /* assign a unique name for every event */ 
    static int nameCounter = 0;
    buf[0] = 0;
    if (e == NULL){
        fprintf(stderr, "CreateEvent: malloc failed\n"); 
        fflush(stderr); 
        exit(1); 
    }
    /* anonymous event - must have a name
       used within a single process */ 
    if (name == NULL) { 
        name = (char*)malloc(EVENT_NAME_LENGTH);
        if (name == NULL) { 
            fprintf(stderr,"SUBLIMEWIN2LINUX: malloc failed\n"); 
            fflush(stderr); 
            exit(1); 
        }
        itoa(getpid()*10 + nameCounter++, name, 10); 
    } 
    tmpDirBuf =  getTempDirLocation();
    if (tmpDirBuf == NULL){
        fprintf(stderr,"error: could not read environment var LIME_TMP_DIR \n"); 
        exit(1);
    }
    
    strcat( strcat( strcat(buf, tmpDirBuf), "/"), name); 
    
    /* will fail with EEXIST for alread opened FIFO when a second process 
	  opens this event */ 
    mkfifo(buf, S_IRWXO | S_IRWXU | S_IRWXG );
    
    if ((fd = open(buf, O_RDWR | O_CREAT)) == -1){
        error("CreateEvent: open failed\n"); 
    }
    e->pipeDescriptor = fd; 
    e->setEvent = sublime_event_setEvent;
    e->waitForEvent = sublime_event_waitForEvent;
    return e; 
}

int GetCurrentProcessId(){
    return getpid(); 
}

int GetLastError(){
    return errno; 
}

void yield(){
  sched_yield(); 
}

#endif
  
