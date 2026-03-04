#ifndef UTILS_H
#define UTILS_H

#include <LeapC.h>

#define LEAP_FINGER_MAX_SIZE 1024
#define LEAP_HAND_POINTABLES_MAX_SIZE (5 * LEAP_FINGER_MAX_SIZE)
#define LEAP_POINTABLES_MAX_SIZE (2 * LEAP_HAND_POINTABLES_MAX_SIZE)

#define LEAP_HAND_MAX_SIZE 1024
#define LEAP_HANDS_MAX_SIZE (2 * LEAP_HAND_MAX_SIZE)

#define LEAP_FRAME_MAX_SIZE (LEAP_POINTABLES_MAX_SIZE + LEAP_HANDS_MAX_SIZE + 256)

#define JSON_LIST_FORMAT_SIZE 8

int leapHandsToJSON(char* buf, LEAP_HAND* hands, int count);
int leapFrameToJSON(const LEAP_TRACKING_EVENT* frame, LEAP_HAND * hands, char *buf);
int leapHandsToPointablesJSON(char* buf, LEAP_HAND* hands, int count);
int leapHandToJSON(char* buf, LEAP_HAND hand);
int leapHandToPointablesJSON(char* buf, LEAP_HAND hand);
int leapFingerToPointableJSON(char* buf, LEAP_DIGIT finger, uint32_t handId, int type);

float* quaternionToMatrix(float* quaternion);
float* leapQuaternionToMatrix(LEAP_QUATERNION quaternion);
float* getBoneDirection(LEAP_BONE bone);
float* getFingerDirection(LEAP_BONE tip, LEAP_BONE base);
float* getBoneBasis(LEAP_VECTOR prevJoint, LEAP_QUATERNION rotation);
float getLeapQuaternionMagnitudeSquared(LEAP_QUATERNION quaternion);
float getMagnitude(float* vector);

int createJSONListFormat(char* buf, int count);

int setBit(int data, int row);
int clearBit(int data, int row);
bool isBitSet(int data, int row);

float getAvgPointableWidth(LEAP_DIGIT finger);

const char* ultraleapResultToCharArray(eLeapRS result);

enum FLAGS {
	NEW,
	FOCUSED,
	BACKGROUND
};

#define MAX_DEVICES 4
#define DEVICE_EVENT_JSON_MAX_SIZE 256

typedef struct {
	uint32_t device_id;
	uint32_t pid;
	uint32_t status;
	bool     attached;
	uint32_t version;
} DeviceState;

typedef struct {
	int      flags;
	uint32_t deviceVersions[MAX_DEVICES];
	uint32_t lastSeenDeviceVersion;
} ClientSession;

int deviceEventToJSON(const DeviceState* dev, char* buf);
const char* devicePIDToTypeString(uint32_t pid);

#endif