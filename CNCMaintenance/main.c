#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <applibs/adc.h>
#include <applibs/log.h>
#include <applibs/gpio.h>
#include <applibs/storage.h>
#include "mt3620.h"
#include "variables.h"
#include "i2c.h"

// Declare functions
void sendTelemetry(const unsigned char* key, const unsigned char* value);
void AzureIoT_DoPeriodicTasks(void);
static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context);
static void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback);
static int ReadMutableFile(void);
static void WriteToMutableFile(int value);
void setupAzure();
void setupBoard();
void readGPIO();
void sendToHub();

int main(void)
{
	setupBoard();
	

	while (true){
		readGPIO();
		nanosleep(&sleepLong, NULL);
		if (auth) sendToHub();
		AzureIoT_DoPeriodicTasks();
	}
}


void setupBoard() {
	adcControllerFd = ADC_Open(MT3620_ADC_CONTROLLER0);
	sampleBitCount = ADC_GetSampleBitCount(adcControllerFd, MT3620_ADC_CONTROLLER0);
	ADC_SetReferenceVoltage(adcControllerFd, MT3620_ADC_CONTROLLER0, sampleMaxVoltage);
	gpio26 = GPIO_OpenAsOutput(MT3620_GPIO26, GPIO_OutputMode_PushPull, GPIO_Value_High);
	GPIO_SetValue(gpio26, GPIO_Value_Low);
	gpio2 = GPIO_OpenAsInput(MT3620_GPIO2);
	gpio28 = GPIO_OpenAsInput(MT3620_GPIO28);
	gpioB = GPIO_OpenAsInput(MT3620_GPIO13);
	initI2c();
	setupAzure();
}

void readGPIO() {
	int readFromFile = ReadMutableFile();
	ADC_Poll(adcControllerFd, MT3620_ADC_CHANNEL1, &qualValue);
	air_quality = (int)qualValue;
	GPIO_GetValue(gpio2, &door);
	GPIO_GetValue(gpio28, &spindle);
	GPIO_GetValue(gpioB, &pulsB);
	readTemp();
	Log_Debug("[****] Total spindle ON cycles %d\n", readFromFile);
	Log_Debug("[GPIO] B (reset) is %d\n", pulsB);
	Log_Debug("[GPIO] Door status is %d\n", door);
	Log_Debug("[GPIO] Spindle status is %d\n", spindle);
	Log_Debug("[SENS] Temperature is %.2f\r\n", tempC);
	Log_Debug("[SENS] Air quality is %d\n", air_quality);
	Log_Debug("----------------------------------\n");
	if (door == 1) {
		GPIO_SetValue(gpio26, GPIO_Value_High);
	}else{
		GPIO_SetValue(gpio26, GPIO_Value_Low);
	}
	if (spindle == 0) {
		int writeToFile = readFromFile + 1;
		WriteToMutableFile(writeToFile);
	}
	if (pulsB == 0) {
		int writeToFile = 0;
		WriteToMutableFile(writeToFile);
	}
}

void sendToHub() {
	char bufCYCLES[20];
	char bufQUAL[20];
	char bufDOOR[20];
	char bufSPINDLE[20];
	char bufTEMP[20];
	int readFromFile = ReadMutableFile();
	snprintf(bufCYCLES, 20, "%d", readFromFile);
	snprintf(bufTEMP, 20, "%d", air_quality);
	snprintf(bufTEMP, 20, "%d", door);
	snprintf(bufSPINDLE, 20, "%d", door);
	snprintf(bufTEMP, 20, "%3.2f", tempC);
	sendTelemetry("cycles", bufCYCLES);
	sendTelemetry("air", bufQUAL);
	sendTelemetry("door", bufDOOR);
	sendTelemetry("spindle", bufSPINDLE);
	sendTelemetry("temp", bufTEMP);
}


void setupAzure() {
	Log_Debug("Azure setup\n");
	IoTHubDeviceClient_LL_Destroy(iothubClientHandle);
	iothubClientHandle = IoTHubDeviceClient_LL_CreateFromConnectionString(MY_CONNECTION_STRING, MQTT_Protocol);
	IoTHubDeviceClient_LL_SetOption(iothubClientHandle, "TrustedCerts", azureIoTCertificatesX);
	IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_KEEP_ALIVE, &keepalivePeriodSeconds);
	IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle, HubConnectionStatusCallback, NULL);
	IoTHubDeviceClient_LL_SetMessageCallback(iothubClientHandle, SendMessageCallback, NULL);
}

void sendTelemetry(const unsigned char* key, const unsigned char* value) {
	static char eventBuffer[100] = { 0 };
	static const char* EventMsgTemplate = "{ \"%s\": \"%s\" }";
	int len = snprintf(eventBuffer, sizeof(eventBuffer), EventMsgTemplate, key, value);
	if (len < 0)
		return;
	Log_Debug("Sending IoT Hub Message: %s\n", eventBuffer);
	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(eventBuffer);
	if (messageHandle == 0) {
		Log_Debug("WARNING: unable to create a new IoTHubMessage\n");
		return;
	}
	if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, SendMessageCallback, /*&callback_param*/ 0) != IOTHUB_CLIENT_OK) {
		Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
	}
	IoTHubMessage_Destroy(messageHandle);
}

static void SendMessageCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context) {
	Log_Debug("INFO: Message received by IoT Hub. Result is: %d\n", result);
}

static void HubConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback) {
	Log_Debug("IoT Hub Authenticated\n");
	auth = true;
}

static int ReadMutableFile(void)
{
	int fd = Storage_OpenMutableFile();
	if (fd < 0) {
		Log_Debug("ERROR: Could not open mutable file:  %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	int value = 0;
	ssize_t ret = read(fd, &value, sizeof(value));
	if (ret < 0) {
		Log_Debug("ERROR: An error occurred while reading file:  %s (%d).\n", strerror(errno),
			errno);
	}
	close(fd);

	if (ret < sizeof(value)) {
		return 0;
	}

	return value;
}

static void WriteToMutableFile(int value)
{
	int fd = Storage_OpenMutableFile();
	if (fd < 0) {
		Log_Debug("ERROR: Could not open mutable file:  %s (%d).\n", strerror(errno), errno);
		return;
	}
	ssize_t ret = write(fd, &value, sizeof(value));
	if (ret < 0) {
		Log_Debug("ERROR: An error occurred while writing to mutable file:  %s (%d).\n",
			strerror(errno), errno);
	}
	else if (ret < sizeof(value)) {
		Log_Debug("ERROR: Only wrote %d of %d bytes requested\n", ret, (int)sizeof(value));
	}
	close(fd);
}

void AzureIoT_DoPeriodicTasks(void) {
	IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
}