/*
 * This file is part of the FreeStreamer project,
 * (C)Copyright 2011-2014 Matias Muhonen.
 * See the file ''LICENSE'' for using the code.
 *
 * Part of the code in this file has been rewritten from
 * the AudioFileStreamExample / afsclient.cpp
 * example, Copyright © 2007 Apple Inc.
 *
 * The threadless playback has been adapted from
 * Alex Crichton's AudioStreamer.
 */

#include "audio_queue.h"

#include <cassert>

//#define AQ_DEBUG 1

#if !defined (AQ_DEBUG)
    #define AQ_TRACE(...) do {} while (0)
#else
    #define AQ_TRACE(...) printf(__VA_ARGS__)
#endif

namespace astreamer {
    
typedef struct queued_packet {
    AudioStreamPacketDescription desc;
    struct queued_packet *next;
    char data[];
} queued_packet_t;
    
/* public */    
    
Audio_Queue::Audio_Queue()
    : m_delegate(0),
    m_state(IDLE),
    m_outAQ(0),
    m_fillBufferIndex(0),
    m_bytesFilled(0),
    m_packetsFilled(0),
    m_buffersUsed(0),
    m_audioQueueStarted(false),
    m_waitingOnBuffer(false),
    m_queuedHead(0),
    m_queuedTail(0),
    m_lastError(noErr)
{
    for (size_t i=0; i < AQ_BUFFERS; i++) {
        m_bufferInUse[i] = false;
    }
}
    
Audio_Queue::~Audio_Queue()
{
    stop(true);
    
    cleanup();
}
    
bool Audio_Queue::initialized()
{
    return (m_outAQ != 0);
}
    
void Audio_Queue::start()
{
    // start the queue if it has not been started already
    if (m_audioQueueStarted) {
        return;
    }
            
    OSStatus err = AudioQueueStart(m_outAQ, NULL);
    if (!err) {
        m_audioQueueStarted = true;
        m_lastError = noErr;
    } else {
        AQ_TRACE("%s: AudioQueueStart failed!\n", __PRETTY_FUNCTION__);
        m_lastError = err;
    }
}
    
void Audio_Queue::pause()
{
    if (m_state == RUNNING) {
        if (AudioQueuePause(m_outAQ) != 0) {
            AQ_TRACE("%s: AudioQueuePause failed!\n", __PRETTY_FUNCTION__);
        }
        setState(PAUSED);
    } else if (m_state == PAUSED) {
        AudioQueueStart(m_outAQ, NULL);
        setState(RUNNING);
    }
}
    
void Audio_Queue::stop()
{
    stop(true);
}

void Audio_Queue::stop(bool stopImmediately)
{
    if (!m_audioQueueStarted) {
        AQ_TRACE("%s: audio queue already stopped, return!\n", __PRETTY_FUNCTION__);
        return;
    }
    m_audioQueueStarted = false;
    
    AQ_TRACE("%s: enter\n", __PRETTY_FUNCTION__);

    if (AudioQueueFlush(m_outAQ) != 0) {
        AQ_TRACE("%s: AudioQueueFlush failed!\n", __PRETTY_FUNCTION__);
    }
    
    if (stopImmediately) {
        AudioQueueRemovePropertyListener(m_outAQ,
                                         kAudioQueueProperty_IsRunning,
                                         audioQueueIsRunningCallback,
                                         this);
    }
    
    if (AudioQueueStop(m_outAQ, stopImmediately) != 0) {
        AQ_TRACE("%s: AudioQueueStop failed!\n", __PRETTY_FUNCTION__);
    }
    
    if (stopImmediately) {
        setState(IDLE);
    }
    
    AQ_TRACE("%s: leave\n", __PRETTY_FUNCTION__);
}
    
unsigned Audio_Queue::timePlayedInSeconds()
{
    unsigned timePlayed = 0;
    
    AudioTimeStamp queueTime;
    Boolean discontinuity;
    
    OSStatus err = AudioQueueGetCurrentTime(m_outAQ, NULL, &queueTime, &discontinuity);
    if (err) {
        goto out;
    }
    
    timePlayed = queueTime.mSampleTime / m_streamDesc.mSampleRate;
    
out:
    return timePlayed;
}

void Audio_Queue::handlePropertyChange(AudioFileStreamID inAudioFileStream, AudioFileStreamPropertyID inPropertyID, UInt32 *ioFlags)
{
    OSStatus err = noErr;
    
    AQ_TRACE("found property '%u%u%u%u'\n", (inPropertyID>>24)&255, (inPropertyID>>16)&255, (inPropertyID>>8)&255, inPropertyID&255);
    
    switch (inPropertyID) {
        case kAudioFileStreamProperty_ReadyToProducePackets:
        {
            cleanup();
            
            // create the audio queue
            err = AudioQueueNewOutput(&m_streamDesc, audioQueueOutputCallback, this, CFRunLoopGetCurrent(), NULL, 0, &m_outAQ);
            if (err) {
                AQ_TRACE("%s: error in AudioQueueNewOutput\n", __PRETTY_FUNCTION__);
                
                m_lastError = err;
                
                if (m_delegate) {
                    m_delegate->audioQueueInitializationFailed();
                }
                
                break;
            }
            
            // allocate audio queue buffers
            for (unsigned int i = 0; i < AQ_BUFFERS; ++i) {
                err = AudioQueueAllocateBuffer(m_outAQ, AQ_BUFSIZ, &m_audioQueueBuffer[i]);
                if (err) {
                    /* If allocating the buffers failed, everything else will fail, too.
                     *  Dispose the queue so that we can later on detect that this
                     *  queue in fact has not been initialized.
                     */
                    
                    AQ_TRACE("%s: error in AudioQueueAllocateBuffer\n", __PRETTY_FUNCTION__);
                    
                    (void)AudioQueueDispose(m_outAQ, true);
                    m_outAQ = 0;
                    
                    m_lastError = err;
                    
                    if (m_delegate) {
                        m_delegate->audioQueueInitializationFailed();
                    }
                    
                    break;
                }
            }
            
            // listen for kAudioQueueProperty_IsRunning
            err = AudioQueueAddPropertyListener(m_outAQ, kAudioQueueProperty_IsRunning, audioQueueIsRunningCallback, this);
            if (err) {
                AQ_TRACE("%s: error in AudioQueueAddPropertyListener\n", __PRETTY_FUNCTION__);
                m_lastError = err;
                break;
            }
            
            break;
        }
    }
}

void Audio_Queue::handleAudioPackets(UInt32 inNumberBytes, UInt32 inNumberPackets, const void *inInputData, AudioStreamPacketDescription *inPacketDescriptions)
{
    if (!initialized()) {
        AQ_TRACE("%s: warning: attempt to handle audio packets with uninitialized audio queue. return.\n", __PRETTY_FUNCTION__);
        
        return;
    }
    
    // this is called by audio file stream when it finds packets of audio
    AQ_TRACE("got data.  bytes: %u  packets: %u\n", inNumberBytes, (unsigned int)inNumberPackets);
    
    /* Place each packet into a buffer and then send each buffer into the audio
     queue */
    UInt32 i;
    
    for (i = 0; i < inNumberPackets && !m_waitingOnBuffer && m_queuedHead == NULL; i++) {
        AudioStreamPacketDescription *desc = &inPacketDescriptions[i];
        if (!handlePacket((const char*)inInputData + desc->mStartOffset, desc)) {
            break;
        }
    }
    if (i == inNumberPackets) {
        return;
    }
    
    for (; i < inNumberPackets; i++) {
        /* Allocate the packet */
        UInt32 size = inPacketDescriptions[i].mDataByteSize;
        queued_packet_t *packet = (queued_packet_t *)malloc(sizeof(queued_packet_t) + size);
        
        /* Prepare the packet */
        packet->next = NULL;
        packet->desc = inPacketDescriptions[i];
        packet->desc.mStartOffset = 0;
        memcpy(packet->data, (const char *)inInputData + inPacketDescriptions[i].mStartOffset,
               size);
        
        if (m_queuedHead == NULL) {
            m_queuedHead = m_queuedTail = packet;
        } else {
            m_queuedTail->next = packet;
            m_queuedTail = packet;
        }
    }
}
    
bool Audio_Queue::handlePacket(const void *data, AudioStreamPacketDescription *desc)
{
    if (!initialized()) {
        AQ_TRACE("%s: warning: attempt to handle audio packets with uninitialized audio queue. return.\n", __PRETTY_FUNCTION__);
        
        return false;
    }
    
    AQ_TRACE("%s: enter\n", __PRETTY_FUNCTION__);
    
    UInt32 packetSize = desc->mDataByteSize;
    
    /* This shouldn't happen because most of the time we read the packet buffer
     size from the file stream, but if we restored to guessing it we could
     come up too small here */
    if (packetSize > AQ_BUFSIZ) {
        AQ_TRACE("%s: packetSize %u > AQ_BUFSIZ %li\n", __PRETTY_FUNCTION__, (unsigned int)packetSize, AQ_BUFSIZ);
        return false;
    }
    
    // if the space remaining in the buffer is not enough for this packet, then
    // enqueue the buffer and wait for another to become available.
    if (AQ_BUFSIZ - m_bytesFilled < packetSize) {
        if (!enqueueBuffer()) {
            return false;
        }
    } else {
        AQ_TRACE("%s: skipped enqueueBuffer AQ_BUFSIZ - m_bytesFilled %lu, packetSize %u\n", __PRETTY_FUNCTION__, (AQ_BUFSIZ - m_bytesFilled), (unsigned int)packetSize);
    }
    
    // copy data to the audio queue buffer
    AudioQueueBufferRef buf = m_audioQueueBuffer[m_fillBufferIndex];
    memcpy((char*)buf->mAudioData + m_bytesFilled, data, packetSize);
    
    // fill out packet description to pass to enqueue() later on
    m_packetDescs[m_packetsFilled] = *desc;
    // Make sure the offset is relative to the start of the audio buffer
    m_packetDescs[m_packetsFilled].mStartOffset = m_bytesFilled;
    // keep track of bytes filled and packets filled
    m_bytesFilled += packetSize;
    m_packetsFilled++;
    
    /* If filled our buffer with packets, then commit it to the system */
    if (m_packetsFilled >= AQ_MAX_PACKET_DESCS) {
        return enqueueBuffer();
    }
    return true;
}

/* private */
    
void Audio_Queue::cleanup()
{
    if (!initialized()) {
        AQ_TRACE("%s: warning: attempt to cleanup an uninitialized audio queue. return.\n", __PRETTY_FUNCTION__);
        
        return;
    }
    
    if (m_state != IDLE) {
        AQ_TRACE("%s: attemping to cleanup the audio queue when it is still playing, force stopping\n",
                 __PRETTY_FUNCTION__);
        
        AudioQueueRemovePropertyListener(m_outAQ,
                                         kAudioQueueProperty_IsRunning,
                                         audioQueueIsRunningCallback,
                                         this);
        
        AudioQueueStop(m_outAQ, true);
        setState(IDLE);
    }
    
    if (AudioQueueDispose(m_outAQ, true) != 0) {
        AQ_TRACE("%s: AudioQueueDispose failed!\n", __PRETTY_FUNCTION__);
    }
    m_outAQ = 0;
    m_fillBufferIndex = m_bytesFilled = m_packetsFilled = m_buffersUsed = 0;
    
    for (size_t i=0; i < AQ_BUFFERS; i++) {
        m_bufferInUse[i] = false;
    }
    
    queued_packet_t *cur = m_queuedHead;
    while (cur) {
        queued_packet_t *tmp = cur->next;
        free(cur);
        cur = tmp;
    }
    m_queuedHead = m_queuedTail = 0;
    
    m_waitingOnBuffer = false;
    m_lastError = noErr;
}
    
void Audio_Queue::setState(State state)
{
    if (m_state == state) {
        /* We are already in this state! */
        return;
    }
    
    m_state = state;
    
    if (m_delegate) {
        m_delegate->audioQueueStateChanged(m_state);
    }
}

bool Audio_Queue::enqueueBuffer()
{
    assert(!m_bufferInUse[m_fillBufferIndex]);
    
    AQ_TRACE("%s: enter\n", __PRETTY_FUNCTION__);
    
    m_bufferInUse[m_fillBufferIndex] = true;
    m_buffersUsed++;
    
    // enqueue buffer
    AudioQueueBufferRef fillBuf = m_audioQueueBuffer[m_fillBufferIndex];
    fillBuf->mAudioDataByteSize = m_bytesFilled;
    
    assert(m_packetsFilled > 0);
    OSStatus err = AudioQueueEnqueueBuffer(m_outAQ, fillBuf, m_packetsFilled, m_packetDescs);
    if (!err) {
        m_lastError = noErr;
        start();
    } else {
        /* If we get an error here, it very likely means that the audio queue is no longer
           running */
        AQ_TRACE("%s: error in AudioQueueEnqueueBuffer\n", __PRETTY_FUNCTION__);
        m_lastError = err;
        return false;
    }
    
    // go to next buffer
    if (++m_fillBufferIndex >= AQ_BUFFERS) {
        m_fillBufferIndex = 0; 
    }
    // reset bytes filled
    m_bytesFilled = 0;
    // reset packets filled
    m_packetsFilled = 0;
    
    // wait until next buffer is not in use
    if (m_bufferInUse[m_fillBufferIndex]) {
        AQ_TRACE("waiting for buffer %u\n", (unsigned int)m_fillBufferIndex);
        
        if (m_delegate) {
            m_delegate->audioQueueOverflow();
        }
        m_waitingOnBuffer = true;
        return false;
    }
    
    return true;
}
    
// this is called by the audio queue when it has finished decoding our data. 
// The buffer is now free to be reused.
void Audio_Queue::audioQueueOutputCallback(void *inClientData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
    Audio_Queue *audioQueue = static_cast<Audio_Queue*>(inClientData);
    
    for (UInt32 bufIndex = 0; bufIndex < AQ_BUFFERS; ++bufIndex) {
        if (inBuffer == audioQueue->m_audioQueueBuffer[bufIndex]) {
            assert(audioQueue->m_bufferInUse[bufIndex]);
            
            audioQueue->m_bufferInUse[bufIndex] = false;
            audioQueue->m_buffersUsed--;
        }
    }

    if (audioQueue->m_buffersUsed == 0 && !audioQueue->m_queuedHead && audioQueue->m_delegate) {
        audioQueue->m_delegate->audioQueueBuffersEmpty();
    } else if (audioQueue->m_waitingOnBuffer) {
        audioQueue->m_waitingOnBuffer = false;

        // Enqueue cached data
        assert(!audioQueue->m_waitingOnBuffer);
        assert(!audioQueue->m_bufferInUse[audioQueue->m_fillBufferIndex]);
        
        /* Queue up as many packets as possible into the buffers */
        queued_packet_t *cur = audioQueue->m_queuedHead;
        while (cur) {
            if (!audioQueue->handlePacket(cur->data, &cur->desc)) {
                break;
            }
            queued_packet_t *next = cur->next;
            free(cur);
            cur = next;
        }
        audioQueue->m_queuedHead = cur;
        
        /* If we finished queueing all our saved packets, we can re-schedule the
         * stream to run */
        if (cur == NULL) {
            audioQueue->m_queuedTail = NULL;
            if (audioQueue->m_delegate) {
                audioQueue->m_delegate->audioQueueUnderflow();
            }
        }
    }
}

void Audio_Queue::audioQueueIsRunningCallback(void *inClientData, AudioQueueRef inAQ, AudioQueuePropertyID inID)
{
    Audio_Queue *audioQueue = static_cast<Audio_Queue*>(inClientData);
    
    AQ_TRACE("%s: enter\n", __PRETTY_FUNCTION__);
    
    UInt32 running;
    UInt32 output = sizeof(running);
    OSStatus err = AudioQueueGetProperty(inAQ, kAudioQueueProperty_IsRunning, &running, &output);
    if (err) {
        AQ_TRACE("%s: error in kAudioQueueProperty_IsRunning\n", __PRETTY_FUNCTION__);
        return;
    }
    if (running) {
        AQ_TRACE("audio queue running!\n");
        audioQueue->setState(RUNNING);
    } else {
        audioQueue->setState(IDLE);
    }
}    
    
} // namespace astreamer