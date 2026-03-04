#include <stdio.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include <utils.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

//--------------------| HAND TRACKING

static LEAP_CONNECTION connectionHandle = NULL;
static LEAP_TRACKING_EVENT frame = { 0 };
static LEAP_TRACKING_EVENT* currentFrame = &frame;
static LEAP_HAND* hands = NULL;

static pthread_t ultraleap_thread;
static pthread_mutex_t ultraleap_lock;
static volatile bool ultraleap_thread_running = false;

static uint64_t previousEventID = 0;

static DeviceState devices[MAX_DEVICES] = {0};
static volatile uint32_t deviceGlobalVersion = 0;

// Must be called with ultraleap_lock held. Returns the slot index, or -1.
static int handleDeviceEvent(const LEAP_DEVICE_EVENT* event, bool attached) {
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].attached && devices[i].device_id == event->device.id) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && attached) {
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!devices[i].attached) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return -1;

    devices[slot].device_id = event->device.id;
    devices[slot].status    = event->status;
    devices[slot].attached  = attached;
    devices[slot].version++;
    deviceGlobalVersion++;

    return slot;
}

void handleTrackingEvent(const LEAP_TRACKING_EVENT* event) {
    pthread_mutex_lock(&ultraleap_lock);

    bool nHandsChanged = event->nHands != currentFrame->nHands;

    memcpy(currentFrame, event, sizeof(LEAP_TRACKING_EVENT));

    // Reallocate the hands array if the number of hands have changed
    if (nHandsChanged) {
        if (hands != NULL) {
            free(hands);
            hands = NULL;
        }

        if (event->nHands > 0) {
            hands = malloc(event->nHands * sizeof(LEAP_HAND));
        }
    }

    if (event->nHands > 0 && hands != NULL) {
        memcpy(hands, event->pHands, event->nHands * sizeof(LEAP_HAND));
    }
    currentFrame->pHands = hands;
    
    pthread_mutex_unlock(&ultraleap_lock);
}

void* serviceMessageLoop(void* vargp) {
    LEAP_CONNECTION_MESSAGE msg;
    eLeapRS result;

    ultraleap_thread_running = true;
    while (ultraleap_thread_running) {
        result = LeapPollConnection(connectionHandle, 1000, &msg);

        if (result != eLeapRS_Success && result != eLeapRS_Timeout) {
            printf("%s\n", ultraleapResultToCharArray(result));
        }

        if (!ultraleap_thread_running) {
            break;
        }

        switch (msg.type) {
            case eLeapEventType_Connection:
                printf("Connected.\n");
                break;
            case eLeapEventType_Tracking:
                handleTrackingEvent(msg.tracking_event);
                break;
            case eLeapEventType_Device: {
                LEAP_DEVICE_REF ref = msg.device_event->device;

                // Fetch device PID before broadcasting the event to clients,
                // so they never see type "unknown" due to pid=0.
                uint32_t pid = 0;
                {
                    LEAP_DEVICE hDevice;
                    if (LeapOpenDevice(ref, &hDevice) == eLeapRS_Success) {
                        LEAP_DEVICE_INFO info = { .size = sizeof(LEAP_DEVICE_INFO) };
                        LeapGetDeviceInfo(hDevice, &info);
                        pid = info.pid;
                        LeapCloseDevice(hDevice);
                    }
                }

                pthread_mutex_lock(&ultraleap_lock);
                int slot = handleDeviceEvent(msg.device_event, true);
                if (slot >= 0) {
                    devices[slot].pid = pid;
                }
                pthread_mutex_unlock(&ultraleap_lock);

                printf("Device connected (id: %u)\n", ref.id);
                break;
            }
            case eLeapEventType_DeviceLost:
                pthread_mutex_lock(&ultraleap_lock);
                handleDeviceEvent(msg.device_event, false);
                pthread_mutex_unlock(&ultraleap_lock);
                printf("Device lost (id: %u)\n", msg.device_event->device.id);
                break;
            case eLeapEventType_DeviceStatusChange: {
                const LEAP_DEVICE_STATUS_CHANGE_EVENT* evt = msg.device_status_change_event;
                pthread_mutex_lock(&ultraleap_lock);
                for (int i = 0; i < MAX_DEVICES; i++) {
                    if (devices[i].attached && devices[i].device_id == evt->device.id) {
                        devices[i].status = evt->status;
                        devices[i].version++;
                        deviceGlobalVersion++;
                        break;
                    }
                }
                pthread_mutex_unlock(&ultraleap_lock);
                break;
            }
            default:
                break;
        }
    }

    if (currentFrame != NULL) {
        free(currentFrame);
    }

    return NULL;
}

static void* allocate(uint32_t size, eLeapAllocatorType typeHint, void* state) {
    void* ptr = malloc(size);
    return ptr;
}

static void deallocate(void* ptr, void* state) {
    if (!ptr)
        return;
    free(ptr);
}

//--------------------| WEBSOCKETS CALLBACKS

static int callback_websocket(struct lws *wsi,
                                   enum lws_callback_reasons reason,
                                   void *user, void *in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: // just log message that someone is connecting
            printf("connection established\n");

            char uribuf[32];  // Store path of uri 
            if (lws_hdr_copy(wsi, uribuf, sizeof(uribuf), WSI_TOKEN_GET_URI) > 0)
            {
                bool flag = !strcmp(uribuf, "/v6.json");
                if (flag)
                {
                    lwsl_notice("Client connect!\n");
                }
                else
                {
                    lws_set_timeout(wsi, NO_PENDING_TIMEOUT, LWS_TO_KILL_ASYNC); // Close the current connection immediately
                    lwsl_notice("Client connect Refuse !!\n");
                    return 0;
                }
            }

            // Set the first bit to 0 so we will send the version on the next frame
            {
                ClientSession* session = (ClientSession*)user;
                session->flags = clearBit(session->flags, NEW);
            }

            lws_callback_on_writable(wsi);
            break;
        case LWS_CALLBACK_RECEIVE: { // the funny part
            ClientSession* session = (ClientSession*)user;

            // log what we received.
            // that disco syntax `%.*s` is used to print just a part of our buffer
            // http://stackoverflow.com/questions/5189071/print-part-of-char-array
            printf("received data: %.*s\n", (int)len, (char*)in);

            char* answer = (char*)malloc(32 * sizeof(char));
            if (answer == NULL) {
                return 0;
            }

            snprintf(answer, len + 1, "%s", (char*)in);

            if (strcmp(answer, "{\"focused\":true}") == 0)
            {
                session->flags = setBit(session->flags, FOCUSED);

                // Program next send
                lws_callback_on_writable(wsi);
            }
            else if (strcmp(answer, "{\"focused\":false}") == 0)
            {
                session->flags = clearBit(session->flags, FOCUSED);
            }
            else if (strcmp(answer, "{\"background\":true}") == 0)
            {
                session->flags = setBit(session->flags, BACKGROUND);

                // Program next send
                lws_callback_on_writable(wsi);
            }
            else if (strcmp(answer, "{\"background\":false}") == 0)
            {
                session->flags = clearBit(session->flags, BACKGROUND);
            }
            else if (strcmp(answer, "{\"optimizeHMD\":true}") == 0) {
                LeapSetTrackingMode(connectionHandle, eLeapTrackingMode_HMD);
            }
            else if (strcmp(answer, "{\"optimizeScreentop\":true}") == 0) {
                LeapSetTrackingMode(connectionHandle, eLeapTrackingMode_ScreenTop);
            }
            else if (strcmp(answer, "{\"optimizeHMD\":false}") == 0
                || strcmp(answer, "{\"optimizeScreentop\":false}") == 0) {
                LeapSetTrackingMode(connectionHandle, eLeapTrackingMode_Desktop);
            }

            free(answer);
            break;
        }
        case LWS_CALLBACK_SERVER_WRITEABLE: {
            ClientSession* session = (ClientSession*)user;

            // Check if first bit is at zero
            if (!isBitSet(session->flags, NEW)) {
                printf("New user, need to send the 'version'\n");

                // Setting the first user bit to 1 so we won't send the version again
                session->flags = setBit(session->flags, NEW);
                session->flags = setBit(session->flags, BACKGROUND);

                // Create buffer
                unsigned char* buf = (unsigned char*)malloc(LWS_SEND_BUFFER_PRE_PADDING + 16 + LWS_SEND_BUFFER_POST_PADDING);

                if (buf == NULL) {
                    printf("An error happened when allocating the buffer for the message\n");
                    // Program next send
                    lws_callback_on_writable(wsi);

                    return 0;
                }

                int len = sprintf((char*)&buf[LWS_SEND_BUFFER_PRE_PADDING], "{\"version\":6}");

                // Send data
                lws_write(wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING], len, LWS_WRITE_TEXT);

                // Program next send
                lws_callback_on_writable(wsi);

                // Release memory
                free(buf);

                return 0;
            }

            // Send pending device events before frames (lock-free fast path)
            if (session->lastSeenDeviceVersion != deviceGlobalVersion) {
                pthread_mutex_lock(&ultraleap_lock);
                int pendingSlot = -1;
                for (int i = 0; i < MAX_DEVICES; i++) {
                    if (session->deviceVersions[i] < devices[i].version) {
                        pendingSlot = i;
                        break;
                    }
                }

                if (pendingSlot >= 0) {
                    DeviceState dev = devices[pendingSlot];
                    session->deviceVersions[pendingSlot] = dev.version;
                    pthread_mutex_unlock(&ultraleap_lock);

                    unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + DEVICE_EVENT_JSON_MAX_SIZE + LWS_SEND_BUFFER_POST_PADDING];
                    int len = deviceEventToJSON(&dev, (char*)(buf + LWS_SEND_BUFFER_PRE_PADDING));
                    lws_write(wsi, buf + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

                    lws_callback_on_writable(wsi);
                    return 0;
                }

                session->lastSeenDeviceVersion = deviceGlobalVersion;
                pthread_mutex_unlock(&ultraleap_lock);
            }

            // Check if client is in focus and want background frames, if it does not we stop sending frames
            if (!isBitSet(session->flags, FOCUSED) && !isBitSet(session->flags, BACKGROUND)) {
                return 0;
            }

            // Lock access to data (we'll read it)
            pthread_mutex_lock(&ultraleap_lock);

            if (currentFrame == NULL || currentFrame->tracking_frame_id == previousEventID) {
                // Unlock access to data
                pthread_mutex_unlock(&ultraleap_lock);

                // Program next send
                lws_callback_on_writable(wsi);

                return 0;
            }

            previousEventID = currentFrame->tracking_frame_id;

            // Create buffer
            unsigned char* buf = (unsigned char*)malloc(LWS_SEND_BUFFER_PRE_PADDING + LEAP_FRAME_MAX_SIZE + LWS_SEND_BUFFER_POST_PADDING);

            // Fill data
            int len = leapFrameToJSON(currentFrame, hands, (char*)(buf + LWS_SEND_BUFFER_PRE_PADDING));

            // Unlock access to data
            pthread_mutex_unlock(&ultraleap_lock);

            // Send data
            lws_write(wsi, (buf + LWS_SEND_BUFFER_PRE_PADDING), len, LWS_WRITE_TEXT);

            // Program next send
            lws_callback_on_writable(wsi);

            // Release memory
            free(buf);

            break;
        }
        default:
            break;
    }
    
    return 0;
}



static struct lws_protocols protocols[] = {
    {
        "v6.json",
        callback_websocket,
        sizeof(ClientSession)
    },
    {
        NULL, NULL, 0   /* End of list */
    }
};


//--------------------| MAIN PROGRAM


int main(void) {
    eLeapRS result = LeapCreateConnection(NULL, &connectionHandle);
    if (result == eLeapRS_Success) {
        result = LeapOpenConnection(connectionHandle);
        if (result == eLeapRS_Success) {
            {
                LEAP_ALLOCATOR allocator = { allocate, deallocate, NULL };
                LeapSetAllocator(connectionHandle, &allocator);
            }

            // Create the message poll thread and the data mutex
            pthread_mutex_init(&ultraleap_lock, NULL);
            pthread_create(&ultraleap_thread, NULL, serviceMessageLoop, NULL);
        }
        else {
            printf("LeapOpenConnection failed with: %s\n", ultraleapResultToCharArray(result));
        }
    }
    else {
        printf("LeapCreateConnection failed with: %s\n", ultraleapResultToCharArray(result));
    }

    // server url will be http://localhost:6437
    int port = 6437;
    struct lws_context *context;
    struct lws_context_creation_info context_info =
    {
        .port = port, .iface = NULL, .protocols = protocols, .extensions = NULL,
        .ssl_cert_filepath = NULL, .ssl_private_key_filepath = NULL, .ssl_ca_filepath = NULL,
        .gid = -1, .uid = -1, .options = 0, NULL, .ka_time = 0, .ka_probes = 0, .ka_interval = 0
    };
    // create lws context representing this server
    context = lws_create_context(&context_info);

    if (context == NULL) {
        fprintf(stderr, "lws init failed\n");
        return -1;
    }
    
    printf("Starting server...\n");
    
    // infinite loop, to end this server send SIGTERM. (CTRL+C)
    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);

    ultraleap_thread_running = false;
    pthread_join(ultraleap_thread, NULL);
    pthread_mutex_destroy(&ultraleap_lock);

    LeapCloseConnection(connectionHandle);
    LeapDestroyConnection(connectionHandle);
    
    return 0;
}