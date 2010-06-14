//
// Copyright 2010 The Android Open Source Project
//
// The input dispatcher.
//
#define LOG_TAG "InputDispatcher"

//#define LOG_NDEBUG 0

// Log detailed debug messages about each inbound event notification to the dispatcher.
#define DEBUG_INBOUND_EVENT_DETAILS 1

// Log detailed debug messages about each outbound event processed by the dispatcher.
#define DEBUG_OUTBOUND_EVENT_DETAILS 1

// Log debug messages about batching.
#define DEBUG_BATCHING 1

// Log debug messages about the dispatch cycle.
#define DEBUG_DISPATCH_CYCLE 1

// Log debug messages about performance statistics.
#define DEBUG_PERFORMANCE_STATISTICS 1

#include <cutils/log.h>
#include <ui/InputDispatcher.h>

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

namespace android {

// TODO, this needs to be somewhere else, perhaps in the policy
static inline bool isMovementKey(int32_t keyCode) {
    return keyCode == KEYCODE_DPAD_UP
            || keyCode == KEYCODE_DPAD_DOWN
            || keyCode == KEYCODE_DPAD_LEFT
            || keyCode == KEYCODE_DPAD_RIGHT;
}

// --- InputDispatcher ---

InputDispatcher::InputDispatcher(const sp<InputDispatchPolicyInterface>& policy) :
    mPolicy(policy) {
    mPollLoop = new PollLoop();

    mInboundQueue.head.refCount = -1;
    mInboundQueue.head.type = EventEntry::TYPE_SENTINEL;
    mInboundQueue.head.eventTime = LONG_LONG_MIN;

    mInboundQueue.tail.refCount = -1;
    mInboundQueue.tail.type = EventEntry::TYPE_SENTINEL;
    mInboundQueue.tail.eventTime = LONG_LONG_MAX;

    mKeyRepeatState.lastKeyEntry = NULL;
}

InputDispatcher::~InputDispatcher() {
    resetKeyRepeatLocked();

    while (mConnectionsByReceiveFd.size() != 0) {
        unregisterInputChannel(mConnectionsByReceiveFd.valueAt(0)->inputChannel);
    }

    for (EventEntry* entry = mInboundQueue.head.next; entry != & mInboundQueue.tail; ) {
        EventEntry* next = entry->next;
        mAllocator.releaseEventEntry(next);
        entry = next;
    }
}

void InputDispatcher::dispatchOnce() {
    bool allowKeyRepeat = mPolicy->allowKeyRepeat();

    nsecs_t currentTime;
    nsecs_t nextWakeupTime = LONG_LONG_MAX;
    { // acquire lock
        AutoMutex _l(mLock);
        currentTime = systemTime(SYSTEM_TIME_MONOTONIC);

        // Reset the key repeat timer whenever we disallow key events, even if the next event
        // is not a key.  This is to ensure that we abort a key repeat if the device is just coming
        // out of sleep.
        // XXX we should handle resetting input state coming out of sleep more generally elsewhere
        if (! allowKeyRepeat) {
            resetKeyRepeatLocked();
        }

        // Process timeouts for all connections and determine if there are any synchronous
        // event dispatches pending.
        bool hasPendingSyncTarget = false;
        for (size_t i = 0; i < mActiveConnections.size(); ) {
            Connection* connection = mActiveConnections.itemAt(i);

            nsecs_t connectionTimeoutTime  = connection->nextTimeoutTime;
            if (connectionTimeoutTime <= currentTime) {
                bool deactivated = timeoutDispatchCycleLocked(currentTime, connection);
                if (deactivated) {
                    // Don't increment i because the connection has been removed
                    // from mActiveConnections (hence, deactivated).
                    continue;
                }
            }

            if (connectionTimeoutTime < nextWakeupTime) {
                nextWakeupTime = connectionTimeoutTime;
            }

            if (connection->hasPendingSyncTarget()) {
                hasPendingSyncTarget = true;
            }

            i += 1;
        }

        // If we don't have a pending sync target, then we can begin delivering a new event.
        // (Otherwise we wait for dispatch to complete for that target.)
        if (! hasPendingSyncTarget) {
            if (mInboundQueue.isEmpty()) {
                if (mKeyRepeatState.lastKeyEntry) {
                    if (currentTime >= mKeyRepeatState.nextRepeatTime) {
                        processKeyRepeatLocked(currentTime);
                        return; // dispatched once
                    } else {
                        if (mKeyRepeatState.nextRepeatTime < nextWakeupTime) {
                            nextWakeupTime = mKeyRepeatState.nextRepeatTime;
                        }
                    }
                }
            } else {
                // Inbound queue has at least one entry.  Dequeue it and begin dispatching.
                // Note that we do not hold the lock for this process because dispatching may
                // involve making many callbacks.
                EventEntry* entry = mInboundQueue.dequeueAtHead();

                switch (entry->type) {
                case EventEntry::TYPE_CONFIGURATION_CHANGED: {
                    ConfigurationChangedEntry* typedEntry =
                            static_cast<ConfigurationChangedEntry*>(entry);
                    processConfigurationChangedLocked(currentTime, typedEntry);
                    mAllocator.releaseConfigurationChangedEntry(typedEntry);
                    break;
                }

                case EventEntry::TYPE_KEY: {
                    KeyEntry* typedEntry = static_cast<KeyEntry*>(entry);
                    processKeyLocked(currentTime, typedEntry);
                    mAllocator.releaseKeyEntry(typedEntry);
                    break;
                }

                case EventEntry::TYPE_MOTION: {
                    MotionEntry* typedEntry = static_cast<MotionEntry*>(entry);
                    processMotionLocked(currentTime, typedEntry);
                    mAllocator.releaseMotionEntry(typedEntry);
                    break;
                }

                default:
                    assert(false);
                    break;
                }
                return; // dispatched once
            }
        }
    } // release lock

    // Wait for callback or timeout or wake.
    nsecs_t timeout = nanoseconds_to_milliseconds(nextWakeupTime - currentTime);
    int32_t timeoutMillis = timeout > INT_MAX ? -1 : timeout > 0 ? int32_t(timeout) : 0;
    mPollLoop->pollOnce(timeoutMillis);
}

void InputDispatcher::processConfigurationChangedLocked(nsecs_t currentTime,
        ConfigurationChangedEntry* entry) {
#if DEBUG_OUTBOUND_EVENT_DETAILS
    LOGD("processConfigurationChanged - eventTime=%lld, touchScreenConfig=%d, "
            "keyboardConfig=%d, navigationConfig=%d", entry->eventTime,
            entry->touchScreenConfig, entry->keyboardConfig, entry->navigationConfig);
#endif

    mPolicy->notifyConfigurationChanged(entry->eventTime, entry->touchScreenConfig,
            entry->keyboardConfig, entry->navigationConfig);
}

void InputDispatcher::processKeyLocked(nsecs_t currentTime, KeyEntry* entry) {
#if DEBUG_OUTBOUND_EVENT_DETAILS
    LOGD("processKey - eventTime=%lld, deviceId=0x%x, nature=0x%x, policyFlags=0x%x, action=0x%x, "
            "flags=0x%x, keyCode=0x%x, scanCode=0x%x, metaState=0x%x, downTime=%lld",
            entry->eventTime, entry->deviceId, entry->nature, entry->policyFlags, entry->action,
            entry->flags, entry->keyCode, entry->scanCode, entry->metaState,
            entry->downTime);
#endif

    // TODO: Poke user activity.

    if (entry->action == KEY_EVENT_ACTION_DOWN) {
        if (mKeyRepeatState.lastKeyEntry
                && mKeyRepeatState.lastKeyEntry->keyCode == entry->keyCode) {
            // We have seen two identical key downs in a row which indicates that the device
            // driver is automatically generating key repeats itself.  We take note of the
            // repeat here, but we disable our own next key repeat timer since it is clear that
            // we will not need to synthesize key repeats ourselves.
            entry->repeatCount = mKeyRepeatState.lastKeyEntry->repeatCount + 1;
            resetKeyRepeatLocked();
            mKeyRepeatState.nextRepeatTime = LONG_LONG_MAX; // don't generate repeats ourselves
        } else {
            // Not a repeat.  Save key down state in case we do see a repeat later.
            resetKeyRepeatLocked();
            mKeyRepeatState.nextRepeatTime = entry->eventTime + mPolicy->getKeyRepeatTimeout();
        }
        mKeyRepeatState.lastKeyEntry = entry;
        entry->refCount += 1;
    } else {
        resetKeyRepeatLocked();
    }

    identifyInputTargetsAndDispatchKeyLocked(currentTime, entry);
}

void InputDispatcher::processKeyRepeatLocked(nsecs_t currentTime) {
    // TODO Old WindowManagerServer code sniffs the input queue for following key up
    //      events and drops the repeat if one is found.  We should do something similar.
    //      One good place to do it is in notifyKey as soon as the key up enters the
    //      inbound event queue.

    // Synthesize a key repeat after the repeat timeout expired.
    // We reuse the previous key entry if otherwise unreferenced.
    KeyEntry* entry = mKeyRepeatState.lastKeyEntry;
    if (entry->refCount == 1) {
        entry->repeatCount += 1;
    } else {
        KeyEntry* newEntry = mAllocator.obtainKeyEntry();
        newEntry->deviceId = entry->deviceId;
        newEntry->nature = entry->nature;
        newEntry->policyFlags = entry->policyFlags;
        newEntry->action = entry->action;
        newEntry->flags = entry->flags;
        newEntry->keyCode = entry->keyCode;
        newEntry->scanCode = entry->scanCode;
        newEntry->metaState = entry->metaState;
        newEntry->repeatCount = entry->repeatCount + 1;

        mKeyRepeatState.lastKeyEntry = newEntry;
        mAllocator.releaseKeyEntry(entry);

        entry = newEntry;
    }
    entry->eventTime = currentTime;
    entry->downTime = currentTime;
    entry->policyFlags = 0;

    mKeyRepeatState.nextRepeatTime = currentTime + mPolicy->getKeyRepeatTimeout();

#if DEBUG_OUTBOUND_EVENT_DETAILS
    LOGD("processKeyRepeat - eventTime=%lld, deviceId=0x%x, nature=0x%x, policyFlags=0x%x, "
            "action=0x%x, flags=0x%x, keyCode=0x%x, scanCode=0x%x, metaState=0x%x, "
            "repeatCount=%d, downTime=%lld",
            entry->eventTime, entry->deviceId, entry->nature, entry->policyFlags,
            entry->action, entry->flags, entry->keyCode, entry->scanCode, entry->metaState,
            entry->repeatCount, entry->downTime);
#endif

    identifyInputTargetsAndDispatchKeyLocked(currentTime, entry);
}

void InputDispatcher::processMotionLocked(nsecs_t currentTime, MotionEntry* entry) {
#if DEBUG_OUTBOUND_EVENT_DETAILS
    LOGD("processMotion - eventTime=%lld, deviceId=0x%x, nature=0x%x, policyFlags=0x%x, action=0x%x, "
            "metaState=0x%x, edgeFlags=0x%x, xPrecision=%f, yPrecision=%f, downTime=%lld",
            entry->eventTime, entry->deviceId, entry->nature, entry->policyFlags, entry->action,
            entry->metaState, entry->edgeFlags, entry->xPrecision, entry->yPrecision,
            entry->downTime);

    // Print the most recent sample that we have available, this may change due to batching.
    size_t sampleCount = 1;
    MotionSample* sample = & entry->firstSample;
    for (; sample->next != NULL; sample = sample->next) {
        sampleCount += 1;
    }
    for (uint32_t i = 0; i < entry->pointerCount; i++) {
        LOGD("  Pointer %d: id=%d, x=%f, y=%f, pressure=%f, size=%f",
                i, entry->pointerIds[i],
                sample->pointerCoords[i].x,
                sample->pointerCoords[i].y,
                sample->pointerCoords[i].pressure,
                sample->pointerCoords[i].size);
    }

    // Keep in mind that due to batching, it is possible for the number of samples actually
    // dispatched to change before the application finally consumed them.
    if (entry->action == MOTION_EVENT_ACTION_MOVE) {
        LOGD("  ... Total movement samples currently batched %d ...", sampleCount);
    }
#endif

    identifyInputTargetsAndDispatchMotionLocked(currentTime, entry);
}

void InputDispatcher::identifyInputTargetsAndDispatchKeyLocked(
        nsecs_t currentTime, KeyEntry* entry) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("identifyInputTargetsAndDispatchKey");
#endif

    mReusableKeyEvent.initialize(entry->deviceId, entry->nature, entry->action, entry->flags,
            entry->keyCode, entry->scanCode, entry->metaState, entry->repeatCount,
            entry->downTime, entry->eventTime);

    mCurrentInputTargets.clear();
    mPolicy->getKeyEventTargets(& mReusableKeyEvent, entry->policyFlags,
            mCurrentInputTargets);

    dispatchEventToCurrentInputTargetsLocked(currentTime, entry, false);
}

void InputDispatcher::identifyInputTargetsAndDispatchMotionLocked(
        nsecs_t currentTime, MotionEntry* entry) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("identifyInputTargetsAndDispatchMotion");
#endif

    mReusableMotionEvent.initialize(entry->deviceId, entry->nature, entry->action,
            entry->edgeFlags, entry->metaState,
            entry->firstSample.pointerCoords[0].x, entry->firstSample.pointerCoords[0].y,
            entry->xPrecision, entry->yPrecision,
            entry->downTime, entry->eventTime, entry->pointerCount, entry->pointerIds,
            entry->firstSample.pointerCoords);

    mCurrentInputTargets.clear();
    mPolicy->getMotionEventTargets(& mReusableMotionEvent, entry->policyFlags,
            mCurrentInputTargets);

    dispatchEventToCurrentInputTargetsLocked(currentTime, entry, false);
}

void InputDispatcher::dispatchEventToCurrentInputTargetsLocked(nsecs_t currentTime,
        EventEntry* eventEntry, bool resumeWithAppendedMotionSample) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("dispatchEventToCurrentInputTargets, "
            "resumeWithAppendedMotionSample=%s",
            resumeWithAppendedMotionSample ? "true" : "false");
#endif

    for (size_t i = 0; i < mCurrentInputTargets.size(); i++) {
        const InputTarget& inputTarget = mCurrentInputTargets.itemAt(i);

        ssize_t connectionIndex = mConnectionsByReceiveFd.indexOfKey(
                inputTarget.inputChannel->getReceivePipeFd());
        if (connectionIndex >= 0) {
            sp<Connection> connection = mConnectionsByReceiveFd.valueAt(connectionIndex);
            prepareDispatchCycleLocked(currentTime, connection.get(), eventEntry, & inputTarget,
                    resumeWithAppendedMotionSample);
        } else {
            LOGW("Framework requested delivery of an input event to channel '%s' but it "
                    "is not registered with the input dispatcher.",
                    inputTarget.inputChannel->getName().string());
        }
    }
}

void InputDispatcher::prepareDispatchCycleLocked(nsecs_t currentTime, Connection* connection,
        EventEntry* eventEntry, const InputTarget* inputTarget,
        bool resumeWithAppendedMotionSample) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("channel '%s' ~ prepareDispatchCycle, flags=%d, timeout=%lldns, "
            "xOffset=%f, yOffset=%f, resumeWithAppendedMotionSample=%s",
            connection->getInputChannelName(), inputTarget->flags, inputTarget->timeout,
            inputTarget->xOffset, inputTarget->yOffset,
            resumeWithAppendedMotionSample ? "true" : "false");
#endif

    // Skip this event if the connection status is not normal.
    // We don't want to queue outbound events at all if the connection is broken or
    // not responding.
    if (connection->status != Connection::STATUS_NORMAL) {
        LOGV("channel '%s' ~ Dropping event because the channel status is %s",
                connection->status == Connection::STATUS_BROKEN ? "BROKEN" : "NOT RESPONDING");
        return;
    }

    // Resume the dispatch cycle with a freshly appended motion sample.
    // First we check that the last dispatch entry in the outbound queue is for the same
    // motion event to which we appended the motion sample.  If we find such a dispatch
    // entry, and if it is currently in progress then we try to stream the new sample.
    bool wasEmpty = connection->outboundQueue.isEmpty();

    if (! wasEmpty && resumeWithAppendedMotionSample) {
        DispatchEntry* motionEventDispatchEntry =
                connection->findQueuedDispatchEntryForEvent(eventEntry);
        if (motionEventDispatchEntry) {
            // If the dispatch entry is not in progress, then we must be busy dispatching an
            // earlier event.  Not a problem, the motion event is on the outbound queue and will
            // be dispatched later.
            if (! motionEventDispatchEntry->inProgress) {
#if DEBUG_BATCHING
                LOGD("channel '%s' ~ Not streaming because the motion event has "
                        "not yet been dispatched.  "
                        "(Waiting for earlier events to be consumed.)",
                        connection->getInputChannelName());
#endif
                return;
            }

            // If the dispatch entry is in progress but it already has a tail of pending
            // motion samples, then it must mean that the shared memory buffer filled up.
            // Not a problem, when this dispatch cycle is finished, we will eventually start
            // a new dispatch cycle to process the tail and that tail includes the newly
            // appended motion sample.
            if (motionEventDispatchEntry->tailMotionSample) {
#if DEBUG_BATCHING
                LOGD("channel '%s' ~ Not streaming because no new samples can "
                        "be appended to the motion event in this dispatch cycle.  "
                        "(Waiting for next dispatch cycle to start.)",
                        connection->getInputChannelName());
#endif
                return;
            }

            // The dispatch entry is in progress and is still potentially open for streaming.
            // Try to stream the new motion sample.  This might fail if the consumer has already
            // consumed the motion event (or if the channel is broken).
            MotionSample* appendedMotionSample = static_cast<MotionEntry*>(eventEntry)->lastSample;
            status_t status = connection->inputPublisher.appendMotionSample(
                    appendedMotionSample->eventTime, appendedMotionSample->pointerCoords);
            if (status == OK) {
#if DEBUG_BATCHING
                LOGD("channel '%s' ~ Successfully streamed new motion sample.",
                        connection->getInputChannelName());
#endif
                return;
            }

#if DEBUG_BATCHING
            if (status == NO_MEMORY) {
                LOGD("channel '%s' ~ Could not append motion sample to currently "
                        "dispatched move event because the shared memory buffer is full.  "
                        "(Waiting for next dispatch cycle to start.)",
                        connection->getInputChannelName());
            } else if (status == status_t(FAILED_TRANSACTION)) {
                LOGD("channel '%s' ~ Could not append motion sample to currently "
                        "dispatchedmove event because the event has already been consumed.  "
                        "(Waiting for next dispatch cycle to start.)",
                        connection->getInputChannelName());
            } else {
                LOGD("channel '%s' ~ Could not append motion sample to currently "
                        "dispatched move event due to an error, status=%d.  "
                        "(Waiting for next dispatch cycle to start.)",
                        connection->getInputChannelName(), status);
            }
#endif
            // Failed to stream.  Start a new tail of pending motion samples to dispatch
            // in the next cycle.
            motionEventDispatchEntry->tailMotionSample = appendedMotionSample;
            return;
        }
    }

    // This is a new event.
    // Enqueue a new dispatch entry onto the outbound queue for this connection.
    DispatchEntry* dispatchEntry = mAllocator.obtainDispatchEntry(eventEntry); // increments ref
    dispatchEntry->targetFlags = inputTarget->flags;
    dispatchEntry->xOffset = inputTarget->xOffset;
    dispatchEntry->yOffset = inputTarget->yOffset;
    dispatchEntry->timeout = inputTarget->timeout;
    dispatchEntry->inProgress = false;
    dispatchEntry->headMotionSample = NULL;
    dispatchEntry->tailMotionSample = NULL;

    // Handle the case where we could not stream a new motion sample because the consumer has
    // already consumed the motion event (otherwise the corresponding dispatch entry would
    // still be in the outbound queue for this connection).  We set the head motion sample
    // to the list starting with the newly appended motion sample.
    if (resumeWithAppendedMotionSample) {
#if DEBUG_BATCHING
        LOGD("channel '%s' ~ Preparing a new dispatch cycle for additional motion samples "
                "that cannot be streamed because the motion event has already been consumed.",
                connection->getInputChannelName());
#endif
        MotionSample* appendedMotionSample = static_cast<MotionEntry*>(eventEntry)->lastSample;
        dispatchEntry->headMotionSample = appendedMotionSample;
    }

    // Enqueue the dispatch entry.
    connection->outboundQueue.enqueueAtTail(dispatchEntry);

    // If the outbound queue was previously empty, start the dispatch cycle going.
    if (wasEmpty) {
        activateConnectionLocked(connection);
        startDispatchCycleLocked(currentTime, connection);
    }
}

void InputDispatcher::startDispatchCycleLocked(nsecs_t currentTime, Connection* connection) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("channel '%s' ~ startDispatchCycle",
            connection->getInputChannelName());
#endif

    assert(connection->status == Connection::STATUS_NORMAL);
    assert(! connection->outboundQueue.isEmpty());

    DispatchEntry* dispatchEntry = connection->outboundQueue.head.next;
    assert(! dispatchEntry->inProgress);

    // TODO throttle successive ACTION_MOVE motion events for the same device
    //      possible implementation could set a brief poll timeout here and resume starting the
    //      dispatch cycle when elapsed

    // Publish the event.
    status_t status;
    switch (dispatchEntry->eventEntry->type) {
    case EventEntry::TYPE_KEY: {
        KeyEntry* keyEntry = static_cast<KeyEntry*>(dispatchEntry->eventEntry);

        // Apply target flags.
        int32_t action = keyEntry->action;
        int32_t flags = keyEntry->flags;
        if (dispatchEntry->targetFlags & InputTarget::FLAG_CANCEL) {
            flags |= KEY_EVENT_FLAG_CANCELED;
        }

        // Publish the key event.
        status = connection->inputPublisher.publishKeyEvent(keyEntry->deviceId, keyEntry->nature,
                action, flags, keyEntry->keyCode, keyEntry->scanCode,
                keyEntry->metaState, keyEntry->repeatCount, keyEntry->downTime,
                keyEntry->eventTime);

        if (status) {
            LOGE("channel '%s' ~ Could not publish key event, "
                    "status=%d", connection->getInputChannelName(), status);
            abortDispatchCycleLocked(currentTime, connection, true /*broken*/);
            return;
        }
        break;
    }

    case EventEntry::TYPE_MOTION: {
        MotionEntry* motionEntry = static_cast<MotionEntry*>(dispatchEntry->eventEntry);

        // Apply target flags.
        int32_t action = motionEntry->action;
        if (dispatchEntry->targetFlags & InputTarget::FLAG_OUTSIDE) {
            action = MOTION_EVENT_ACTION_OUTSIDE;
        }
        if (dispatchEntry->targetFlags & InputTarget::FLAG_CANCEL) {
            action = MOTION_EVENT_ACTION_CANCEL;
        }

        // If headMotionSample is non-NULL, then it points to the first new sample that we
        // were unable to dispatch during the previous cycle so we resume dispatching from
        // that point in the list of motion samples.
        // Otherwise, we just start from the first sample of the motion event.
        MotionSample* firstMotionSample = dispatchEntry->headMotionSample;
        if (! firstMotionSample) {
            firstMotionSample = & motionEntry->firstSample;
        }

        // Publish the motion event and the first motion sample.
        status = connection->inputPublisher.publishMotionEvent(motionEntry->deviceId,
                motionEntry->nature, action, motionEntry->edgeFlags, motionEntry->metaState,
                dispatchEntry->xOffset, dispatchEntry->yOffset,
                motionEntry->xPrecision, motionEntry->yPrecision,
                motionEntry->downTime, firstMotionSample->eventTime,
                motionEntry->pointerCount, motionEntry->pointerIds,
                firstMotionSample->pointerCoords);

        if (status) {
            LOGE("channel '%s' ~ Could not publish motion event, "
                    "status=%d", connection->getInputChannelName(), status);
            abortDispatchCycleLocked(currentTime, connection, true /*broken*/);
            return;
        }

        // Append additional motion samples.
        MotionSample* nextMotionSample = firstMotionSample->next;
        for (; nextMotionSample != NULL; nextMotionSample = nextMotionSample->next) {
            status = connection->inputPublisher.appendMotionSample(
                    nextMotionSample->eventTime, nextMotionSample->pointerCoords);
            if (status == NO_MEMORY) {
#if DEBUG_DISPATCH_CYCLE
                    LOGD("channel '%s' ~ Shared memory buffer full.  Some motion samples will "
                            "be sent in the next dispatch cycle.",
                            connection->getInputChannelName());
#endif
                break;
            }
            if (status != OK) {
                LOGE("channel '%s' ~ Could not append motion sample "
                        "for a reason other than out of memory, status=%d",
                        connection->getInputChannelName(), status);
                abortDispatchCycleLocked(currentTime, connection, true /*broken*/);
                return;
            }
        }

        // Remember the next motion sample that we could not dispatch, in case we ran out
        // of space in the shared memory buffer.
        dispatchEntry->tailMotionSample = nextMotionSample;
        break;
    }

    default: {
        assert(false);
    }
    }

    // Send the dispatch signal.
    status = connection->inputPublisher.sendDispatchSignal();
    if (status) {
        LOGE("channel '%s' ~ Could not send dispatch signal, status=%d",
                connection->getInputChannelName(), status);
        abortDispatchCycleLocked(currentTime, connection, true /*broken*/);
        return;
    }

    // Record information about the newly started dispatch cycle.
    dispatchEntry->inProgress = true;

    connection->lastEventTime = dispatchEntry->eventEntry->eventTime;
    connection->lastDispatchTime = currentTime;

    nsecs_t timeout = dispatchEntry->timeout;
    connection->nextTimeoutTime = (timeout >= 0) ? currentTime + timeout : LONG_LONG_MAX;

    // Notify other system components.
    onDispatchCycleStartedLocked(currentTime, connection);
}

void InputDispatcher::finishDispatchCycleLocked(nsecs_t currentTime, Connection* connection) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("channel '%s' ~ finishDispatchCycle: %01.1fms since event, "
            "%01.1fms since dispatch",
            connection->getInputChannelName(),
            connection->getEventLatencyMillis(currentTime),
            connection->getDispatchLatencyMillis(currentTime));
#endif

    if (connection->status == Connection::STATUS_BROKEN) {
        return;
    }

    // Clear the pending timeout.
    connection->nextTimeoutTime = LONG_LONG_MAX;

    if (connection->status == Connection::STATUS_NOT_RESPONDING) {
        // Recovering from an ANR.
        connection->status = Connection::STATUS_NORMAL;

        // Notify other system components.
        onDispatchCycleFinishedLocked(currentTime, connection, true /*recoveredFromANR*/);
    } else {
        // Normal finish.  Not much to do here.

        // Notify other system components.
        onDispatchCycleFinishedLocked(currentTime, connection, false /*recoveredFromANR*/);
    }

    // Reset the publisher since the event has been consumed.
    // We do this now so that the publisher can release some of its internal resources
    // while waiting for the next dispatch cycle to begin.
    status_t status = connection->inputPublisher.reset();
    if (status) {
        LOGE("channel '%s' ~ Could not reset publisher, status=%d",
                connection->getInputChannelName(), status);
        abortDispatchCycleLocked(currentTime, connection, true /*broken*/);
        return;
    }

    // Start the next dispatch cycle for this connection.
    while (! connection->outboundQueue.isEmpty()) {
        DispatchEntry* dispatchEntry = connection->outboundQueue.head.next;
        if (dispatchEntry->inProgress) {
             // Finish or resume current event in progress.
            if (dispatchEntry->tailMotionSample) {
                // We have a tail of undispatched motion samples.
                // Reuse the same DispatchEntry and start a new cycle.
                dispatchEntry->inProgress = false;
                dispatchEntry->headMotionSample = dispatchEntry->tailMotionSample;
                dispatchEntry->tailMotionSample = NULL;
                startDispatchCycleLocked(currentTime, connection);
                return;
            }
            // Finished.
            connection->outboundQueue.dequeueAtHead();
            mAllocator.releaseDispatchEntry(dispatchEntry);
        } else {
            // If the head is not in progress, then we must have already dequeued the in
            // progress event, which means we actually aborted it (due to ANR).
            // So just start the next event for this connection.
            startDispatchCycleLocked(currentTime, connection);
            return;
        }
    }

    // Outbound queue is empty, deactivate the connection.
    deactivateConnectionLocked(connection);
}

bool InputDispatcher::timeoutDispatchCycleLocked(nsecs_t currentTime, Connection* connection) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("channel '%s' ~ timeoutDispatchCycle",
            connection->getInputChannelName());
#endif

    if (connection->status != Connection::STATUS_NORMAL) {
        return false;
    }

    // Enter the not responding state.
    connection->status = Connection::STATUS_NOT_RESPONDING;
    connection->lastANRTime = currentTime;
    bool deactivated = abortDispatchCycleLocked(currentTime, connection, false /*(not) broken*/);

    // Notify other system components.
    onDispatchCycleANRLocked(currentTime, connection);
    return deactivated;
}

bool InputDispatcher::abortDispatchCycleLocked(nsecs_t currentTime, Connection* connection,
        bool broken) {
#if DEBUG_DISPATCH_CYCLE
    LOGD("channel '%s' ~ abortDispatchCycle, broken=%s",
            connection->getInputChannelName(), broken ? "true" : "false");
#endif

    if (connection->status == Connection::STATUS_BROKEN) {
        return false;
    }

    // Clear the pending timeout.
    connection->nextTimeoutTime = LONG_LONG_MAX;

    // Clear the outbound queue.
    while (! connection->outboundQueue.isEmpty()) {
        DispatchEntry* dispatchEntry = connection->outboundQueue.dequeueAtHead();
        mAllocator.releaseDispatchEntry(dispatchEntry);
    }

    // Outbound queue is empty, deactivate the connection.
    deactivateConnectionLocked(connection);

    // Handle the case where the connection appears to be unrecoverably broken.
    if (broken) {
        connection->status = Connection::STATUS_BROKEN;

        // Notify other system components.
        onDispatchCycleBrokenLocked(currentTime, connection);
    }
    return true; /*deactivated*/
}

bool InputDispatcher::handleReceiveCallback(int receiveFd, int events, void* data) {
    InputDispatcher* d = static_cast<InputDispatcher*>(data);

    { // acquire lock
        AutoMutex _l(d->mLock);

        ssize_t connectionIndex = d->mConnectionsByReceiveFd.indexOfKey(receiveFd);
        if (connectionIndex < 0) {
            LOGE("Received spurious receive callback for unknown input channel.  "
                    "fd=%d, events=0x%x", receiveFd, events);
            return false; // remove the callback
        }

        nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);

        sp<Connection> connection = d->mConnectionsByReceiveFd.valueAt(connectionIndex);
        if (events & (POLLERR | POLLHUP | POLLNVAL)) {
            LOGE("channel '%s' ~ Consumer closed input channel or an error occurred.  "
                    "events=0x%x", connection->getInputChannelName(), events);
            d->abortDispatchCycleLocked(currentTime, connection.get(), true /*broken*/);
            return false; // remove the callback
        }

        if (! (events & POLLIN)) {
            LOGW("channel '%s' ~ Received spurious callback for unhandled poll event.  "
                    "events=0x%x", connection->getInputChannelName(), events);
            return true;
        }

        status_t status = connection->inputPublisher.receiveFinishedSignal();
        if (status) {
            LOGE("channel '%s' ~ Failed to receive finished signal.  status=%d",
                    connection->getInputChannelName(), status);
            d->abortDispatchCycleLocked(currentTime, connection.get(), true /*broken*/);
            return false; // remove the callback
        }

        d->finishDispatchCycleLocked(currentTime, connection.get());
        return true;
    } // release lock
}

void InputDispatcher::notifyConfigurationChanged(nsecs_t eventTime, int32_t touchScreenConfig,
        int32_t keyboardConfig, int32_t navigationConfig) {
#if DEBUG_INBOUND_EVENT_DETAILS
    LOGD("notifyConfigurationChanged - eventTime=%lld, touchScreenConfig=%d, "
            "keyboardConfig=%d, navigationConfig=%d", eventTime,
            touchScreenConfig, keyboardConfig, navigationConfig);
#endif

    bool wasEmpty;
    { // acquire lock
        AutoMutex _l(mLock);

        ConfigurationChangedEntry* newEntry = mAllocator.obtainConfigurationChangedEntry();
        newEntry->eventTime = eventTime;
        newEntry->touchScreenConfig = touchScreenConfig;
        newEntry->keyboardConfig = keyboardConfig;
        newEntry->navigationConfig = navigationConfig;

        wasEmpty = mInboundQueue.isEmpty();
        mInboundQueue.enqueueAtTail(newEntry);
    } // release lock

    if (wasEmpty) {
        mPollLoop->wake();
    }
}

void InputDispatcher::notifyLidSwitchChanged(nsecs_t eventTime, bool lidOpen) {
#if DEBUG_INBOUND_EVENT_DETAILS
    LOGD("notifyLidSwitchChanged - eventTime=%lld, open=%s", eventTime,
            lidOpen ? "true" : "false");
#endif

    // Send lid switch notification immediately and synchronously.
    mPolicy->notifyLidSwitchChanged(eventTime, lidOpen);
}

void InputDispatcher::notifyAppSwitchComing(nsecs_t eventTime) {
#if DEBUG_INBOUND_EVENT_DETAILS
    LOGD("notifyAppSwitchComing - eventTime=%lld", eventTime);
#endif

    // Remove movement keys from the queue from most recent to least recent, stopping at the
    // first non-movement key.
    // TODO: Include a detailed description of why we do this...

    { // acquire lock
        AutoMutex _l(mLock);

        for (EventEntry* entry = mInboundQueue.tail.prev; entry != & mInboundQueue.head; ) {
            EventEntry* prev = entry->prev;

            if (entry->type == EventEntry::TYPE_KEY) {
                KeyEntry* keyEntry = static_cast<KeyEntry*>(entry);
                if (isMovementKey(keyEntry->keyCode)) {
                    LOGV("Dropping movement key during app switch: keyCode=%d, action=%d",
                            keyEntry->keyCode, keyEntry->action);
                    mInboundQueue.dequeue(keyEntry);
                    mAllocator.releaseKeyEntry(keyEntry);
                } else {
                    // stop at last non-movement key
                    break;
                }
            }

            entry = prev;
        }
    } // release lock
}

void InputDispatcher::notifyKey(nsecs_t eventTime, int32_t deviceId, int32_t nature,
        uint32_t policyFlags, int32_t action, int32_t flags,
        int32_t keyCode, int32_t scanCode, int32_t metaState, nsecs_t downTime) {
#if DEBUG_INBOUND_EVENT_DETAILS
    LOGD("notifyKey - eventTime=%lld, deviceId=0x%x, nature=0x%x, policyFlags=0x%x, action=0x%x, "
            "flags=0x%x, keyCode=0x%x, scanCode=0x%x, metaState=0x%x, downTime=%lld",
            eventTime, deviceId, nature, policyFlags, action, flags,
            keyCode, scanCode, metaState, downTime);
#endif

    bool wasEmpty;
    { // acquire lock
        AutoMutex _l(mLock);

        KeyEntry* newEntry = mAllocator.obtainKeyEntry();
        newEntry->eventTime = eventTime;
        newEntry->deviceId = deviceId;
        newEntry->nature = nature;
        newEntry->policyFlags = policyFlags;
        newEntry->action = action;
        newEntry->flags = flags;
        newEntry->keyCode = keyCode;
        newEntry->scanCode = scanCode;
        newEntry->metaState = metaState;
        newEntry->repeatCount = 0;
        newEntry->downTime = downTime;

        wasEmpty = mInboundQueue.isEmpty();
        mInboundQueue.enqueueAtTail(newEntry);
    } // release lock

    if (wasEmpty) {
        mPollLoop->wake();
    }
}

void InputDispatcher::notifyMotion(nsecs_t eventTime, int32_t deviceId, int32_t nature,
        uint32_t policyFlags, int32_t action, int32_t metaState, int32_t edgeFlags,
        uint32_t pointerCount, const int32_t* pointerIds, const PointerCoords* pointerCoords,
        float xPrecision, float yPrecision, nsecs_t downTime) {
#if DEBUG_INBOUND_EVENT_DETAILS
    LOGD("notifyMotion - eventTime=%lld, deviceId=0x%x, nature=0x%x, policyFlags=0x%x, "
            "action=0x%x, metaState=0x%x, edgeFlags=0x%x, xPrecision=%f, yPrecision=%f, "
            "downTime=%lld",
            eventTime, deviceId, nature, policyFlags, action, metaState, edgeFlags,
            xPrecision, yPrecision, downTime);
    for (uint32_t i = 0; i < pointerCount; i++) {
        LOGD("  Pointer %d: id=%d, x=%f, y=%f, pressure=%f, size=%f",
                i, pointerIds[i], pointerCoords[i].x, pointerCoords[i].y,
                pointerCoords[i].pressure, pointerCoords[i].size);
    }
#endif

    bool wasEmpty;
    { // acquire lock
        AutoMutex _l(mLock);

        // Attempt batching and streaming of move events.
        if (action == MOTION_EVENT_ACTION_MOVE) {
            // BATCHING CASE
            //
            // Try to append a move sample to the tail of the inbound queue for this device.
            // Give up if we encounter a non-move motion event for this device since that
            // means we cannot append any new samples until a new motion event has started.
            for (EventEntry* entry = mInboundQueue.tail.prev;
                    entry != & mInboundQueue.head; entry = entry->prev) {
                if (entry->type != EventEntry::TYPE_MOTION) {
                    // Keep looking for motion events.
                    continue;
                }

                MotionEntry* motionEntry = static_cast<MotionEntry*>(entry);
                if (motionEntry->deviceId != deviceId) {
                    // Keep looking for this device.
                    continue;
                }

                if (motionEntry->action != MOTION_EVENT_ACTION_MOVE
                        || motionEntry->pointerCount != pointerCount) {
                    // Last motion event in the queue for this device is not compatible for
                    // appending new samples.  Stop here.
                    goto NoBatchingOrStreaming;
                }

                // The last motion event is a move and is compatible for appending.
                // Do the batching magic and exit.
                mAllocator.appendMotionSample(motionEntry, eventTime, pointerCount, pointerCoords);
#if DEBUG_BATCHING
                LOGD("Appended motion sample onto batch for most recent "
                        "motion event for this device in the inbound queue.");
#endif
                return; // done
            }

            // STREAMING CASE
            //
            // There is no pending motion event (of any kind) for this device in the inbound queue.
            // Search the outbound queues for a synchronously dispatched motion event for this
            // device.  If found, then we append the new sample to that event and then try to
            // push it out to all current targets.  It is possible that some targets will already
            // have consumed the motion event.  This case is automatically handled by the
            // logic in prepareDispatchCycleLocked by tracking where resumption takes place.
            //
            // The reason we look for a synchronously dispatched motion event is because we
            // want to be sure that no other motion events have been dispatched since the move.
            // It's also convenient because it means that the input targets are still valid.
            // This code could be improved to support streaming of asynchronously dispatched
            // motion events (which might be significantly more efficient) but it may become
            // a little more complicated as a result.
            //
            // Note: This code crucially depends on the invariant that an outbound queue always
            //       contains at most one synchronous event and it is always last (but it might
            //       not be first!).
            for (size_t i = 0; i < mActiveConnections.size(); i++) {
                Connection* connection = mActiveConnections.itemAt(i);
                if (! connection->outboundQueue.isEmpty()) {
                    DispatchEntry* dispatchEntry = connection->outboundQueue.tail.prev;
                    if (dispatchEntry->targetFlags & InputTarget::FLAG_SYNC) {
                        if (dispatchEntry->eventEntry->type != EventEntry::TYPE_MOTION) {
                            goto NoBatchingOrStreaming;
                        }

                        MotionEntry* syncedMotionEntry = static_cast<MotionEntry*>(
                                dispatchEntry->eventEntry);
                        if (syncedMotionEntry->action != MOTION_EVENT_ACTION_MOVE
                                || syncedMotionEntry->deviceId != deviceId
                                || syncedMotionEntry->pointerCount != pointerCount) {
                            goto NoBatchingOrStreaming;
                        }

                        // Found synced move entry.  Append sample and resume dispatch.
                        mAllocator.appendMotionSample(syncedMotionEntry, eventTime,
                                pointerCount, pointerCoords);
#if DEBUG_BATCHING
                        LOGD("Appended motion sample onto batch for most recent synchronously "
                                "dispatched motion event for this device in the outbound queues.");
#endif
                        nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
                        dispatchEventToCurrentInputTargetsLocked(currentTime, syncedMotionEntry,
                                true /*resumeWithAppendedMotionSample*/);
                        return; // done!
                    }
                }
            }

NoBatchingOrStreaming:;
        }

        // Just enqueue a new motion event.
        MotionEntry* newEntry = mAllocator.obtainMotionEntry();
        newEntry->eventTime = eventTime;
        newEntry->deviceId = deviceId;
        newEntry->nature = nature;
        newEntry->policyFlags = policyFlags;
        newEntry->action = action;
        newEntry->metaState = metaState;
        newEntry->edgeFlags = edgeFlags;
        newEntry->xPrecision = xPrecision;
        newEntry->yPrecision = yPrecision;
        newEntry->downTime = downTime;
        newEntry->pointerCount = pointerCount;
        newEntry->firstSample.eventTime = eventTime;
        newEntry->lastSample = & newEntry->firstSample;
        for (uint32_t i = 0; i < pointerCount; i++) {
            newEntry->pointerIds[i] = pointerIds[i];
            newEntry->firstSample.pointerCoords[i] = pointerCoords[i];
        }

        wasEmpty = mInboundQueue.isEmpty();
        mInboundQueue.enqueueAtTail(newEntry);
    } // release lock

    if (wasEmpty) {
        mPollLoop->wake();
    }
}

void InputDispatcher::resetKeyRepeatLocked() {
    if (mKeyRepeatState.lastKeyEntry) {
        mAllocator.releaseKeyEntry(mKeyRepeatState.lastKeyEntry);
        mKeyRepeatState.lastKeyEntry = NULL;
    }
}

status_t InputDispatcher::registerInputChannel(const sp<InputChannel>& inputChannel) {
    int receiveFd;
    { // acquire lock
        AutoMutex _l(mLock);

        receiveFd = inputChannel->getReceivePipeFd();
        if (mConnectionsByReceiveFd.indexOfKey(receiveFd) >= 0) {
            LOGW("Attempted to register already registered input channel '%s'",
                    inputChannel->getName().string());
            return BAD_VALUE;
        }

        sp<Connection> connection = new Connection(inputChannel);
        status_t status = connection->initialize();
        if (status) {
            LOGE("Failed to initialize input publisher for input channel '%s', status=%d",
                    inputChannel->getName().string(), status);
            return status;
        }

        mConnectionsByReceiveFd.add(receiveFd, connection);
    } // release lock

    mPollLoop->setCallback(receiveFd, POLLIN, handleReceiveCallback, this);
    return OK;
}

status_t InputDispatcher::unregisterInputChannel(const sp<InputChannel>& inputChannel) {
    int32_t receiveFd;
    { // acquire lock
        AutoMutex _l(mLock);

        receiveFd = inputChannel->getReceivePipeFd();
        ssize_t connectionIndex = mConnectionsByReceiveFd.indexOfKey(receiveFd);
        if (connectionIndex < 0) {
            LOGW("Attempted to unregister already unregistered input channel '%s'",
                    inputChannel->getName().string());
            return BAD_VALUE;
        }

        sp<Connection> connection = mConnectionsByReceiveFd.valueAt(connectionIndex);
        mConnectionsByReceiveFd.removeItemsAt(connectionIndex);

        connection->status = Connection::STATUS_ZOMBIE;

        nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
        abortDispatchCycleLocked(currentTime, connection.get(), true /*broken*/);
    } // release lock

    mPollLoop->removeCallback(receiveFd);

    // Wake the poll loop because removing the connection may have changed the current
    // synchronization state.
    mPollLoop->wake();
    return OK;
}

void InputDispatcher::activateConnectionLocked(Connection* connection) {
    for (size_t i = 0; i < mActiveConnections.size(); i++) {
        if (mActiveConnections.itemAt(i) == connection) {
            return;
        }
    }
    mActiveConnections.add(connection);
}

void InputDispatcher::deactivateConnectionLocked(Connection* connection) {
    for (size_t i = 0; i < mActiveConnections.size(); i++) {
        if (mActiveConnections.itemAt(i) == connection) {
            mActiveConnections.removeAt(i);
            return;
        }
    }
}

void InputDispatcher::onDispatchCycleStartedLocked(nsecs_t currentTime, Connection* connection) {
}

void InputDispatcher::onDispatchCycleFinishedLocked(nsecs_t currentTime,
        Connection* connection, bool recoveredFromANR) {
    if (recoveredFromANR) {
        LOGI("channel '%s' ~ Recovered from ANR.  %01.1fms since event, "
                "%01.1fms since dispatch, %01.1fms since ANR",
                connection->getInputChannelName(),
                connection->getEventLatencyMillis(currentTime),
                connection->getDispatchLatencyMillis(currentTime),
                connection->getANRLatencyMillis(currentTime));

        // TODO tell framework
    }
}

void InputDispatcher::onDispatchCycleANRLocked(nsecs_t currentTime, Connection* connection) {
    LOGI("channel '%s' ~ Not responding!  %01.1fms since event, %01.1fms since dispatch",
            connection->getInputChannelName(),
            connection->getEventLatencyMillis(currentTime),
            connection->getDispatchLatencyMillis(currentTime));

    // TODO tell framework
}

void InputDispatcher::onDispatchCycleBrokenLocked(nsecs_t currentTime, Connection* connection) {
    LOGE("channel '%s' ~ Channel is unrecoverably broken and will be disposed!",
            connection->getInputChannelName());

    // TODO tell framework
}

// --- InputDispatcher::Allocator ---

InputDispatcher::Allocator::Allocator() {
}

InputDispatcher::ConfigurationChangedEntry*
InputDispatcher::Allocator::obtainConfigurationChangedEntry() {
    ConfigurationChangedEntry* entry = mConfigurationChangeEntryPool.alloc();
    entry->refCount = 1;
    entry->type = EventEntry::TYPE_CONFIGURATION_CHANGED;
    return entry;
}

InputDispatcher::KeyEntry* InputDispatcher::Allocator::obtainKeyEntry() {
    KeyEntry* entry = mKeyEntryPool.alloc();
    entry->refCount = 1;
    entry->type = EventEntry::TYPE_KEY;
    return entry;
}

InputDispatcher::MotionEntry* InputDispatcher::Allocator::obtainMotionEntry() {
    MotionEntry* entry = mMotionEntryPool.alloc();
    entry->refCount = 1;
    entry->type = EventEntry::TYPE_MOTION;
    entry->firstSample.next = NULL;
    return entry;
}

InputDispatcher::DispatchEntry* InputDispatcher::Allocator::obtainDispatchEntry(
        EventEntry* eventEntry) {
    DispatchEntry* entry = mDispatchEntryPool.alloc();
    entry->eventEntry = eventEntry;
    eventEntry->refCount += 1;
    return entry;
}

void InputDispatcher::Allocator::releaseEventEntry(EventEntry* entry) {
    switch (entry->type) {
    case EventEntry::TYPE_CONFIGURATION_CHANGED:
        releaseConfigurationChangedEntry(static_cast<ConfigurationChangedEntry*>(entry));
        break;
    case EventEntry::TYPE_KEY:
        releaseKeyEntry(static_cast<KeyEntry*>(entry));
        break;
    case EventEntry::TYPE_MOTION:
        releaseMotionEntry(static_cast<MotionEntry*>(entry));
        break;
    default:
        assert(false);
        break;
    }
}

void InputDispatcher::Allocator::releaseConfigurationChangedEntry(
        ConfigurationChangedEntry* entry) {
    entry->refCount -= 1;
    if (entry->refCount == 0) {
        mConfigurationChangeEntryPool.free(entry);
    } else {
        assert(entry->refCount > 0);
    }
}

void InputDispatcher::Allocator::releaseKeyEntry(KeyEntry* entry) {
    entry->refCount -= 1;
    if (entry->refCount == 0) {
        mKeyEntryPool.free(entry);
    } else {
        assert(entry->refCount > 0);
    }
}

void InputDispatcher::Allocator::releaseMotionEntry(MotionEntry* entry) {
    entry->refCount -= 1;
    if (entry->refCount == 0) {
        freeMotionSampleList(entry->firstSample.next);
        mMotionEntryPool.free(entry);
    } else {
        assert(entry->refCount > 0);
    }
}

void InputDispatcher::Allocator::releaseDispatchEntry(DispatchEntry* entry) {
    releaseEventEntry(entry->eventEntry);
    mDispatchEntryPool.free(entry);
}

void InputDispatcher::Allocator::appendMotionSample(MotionEntry* motionEntry,
        nsecs_t eventTime, int32_t pointerCount, const PointerCoords* pointerCoords) {
    MotionSample* sample = mMotionSamplePool.alloc();
    sample->eventTime = eventTime;
    for (int32_t i = 0; i < pointerCount; i++) {
        sample->pointerCoords[i] = pointerCoords[i];
    }

    sample->next = NULL;
    motionEntry->lastSample->next = sample;
    motionEntry->lastSample = sample;
}

void InputDispatcher::Allocator::freeMotionSample(MotionSample* sample) {
    mMotionSamplePool.free(sample);
}

void InputDispatcher::Allocator::freeMotionSampleList(MotionSample* head) {
    while (head) {
        MotionSample* next = head->next;
        mMotionSamplePool.free(head);
        head = next;
    }
}

// --- InputDispatcher::Connection ---

InputDispatcher::Connection::Connection(const sp<InputChannel>& inputChannel) :
        status(STATUS_NORMAL), inputChannel(inputChannel), inputPublisher(inputChannel),
        nextTimeoutTime(LONG_LONG_MAX),
        lastEventTime(LONG_LONG_MAX), lastDispatchTime(LONG_LONG_MAX),
        lastANRTime(LONG_LONG_MAX) {
}

InputDispatcher::Connection::~Connection() {
}

status_t InputDispatcher::Connection::initialize() {
    return inputPublisher.initialize();
}

InputDispatcher::DispatchEntry* InputDispatcher::Connection::findQueuedDispatchEntryForEvent(
        const EventEntry* eventEntry) const {
    for (DispatchEntry* dispatchEntry = outboundQueue.tail.prev;
            dispatchEntry != & outboundQueue.head; dispatchEntry = dispatchEntry->prev) {
        if (dispatchEntry->eventEntry == eventEntry) {
            return dispatchEntry;
        }
    }
    return NULL;
}


// --- InputDispatcherThread ---

InputDispatcherThread::InputDispatcherThread(const sp<InputDispatcherInterface>& dispatcher) :
        Thread(/*canCallJava*/ true), mDispatcher(dispatcher) {
}

InputDispatcherThread::~InputDispatcherThread() {
}

bool InputDispatcherThread::threadLoop() {
    mDispatcher->dispatchOnce();
    return true;
}

} // namespace android