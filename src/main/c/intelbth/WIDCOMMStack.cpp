/**
 *  BlueCove - Java library for Bluetooth
 *  Copyright (C) 2006-2007 Vlad Skarzhevskyy
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  @version $Id$
 */

#include "common.h"

#ifndef _BTWLIB
BOOL isWIDCOMMBluetoothStackPresent() {
	return FALSE;
}
#endif

#ifdef _BTWLIB

// BTW-5_1_0_3101
// #pragma comment(lib, "BtWdSdkLib.lib")
// BTW-5_0_1_902-SDK
#pragma comment(lib, "WidcommSdklib.lib")

//#include "btwlib.h"
#include "BtIfDefinitions.h"
#include "BtIfClasses.h"
#include "com_intel_bluetooth_BluetoothStackWIDCOMM.h"

#define WIDCOMM_DLL "wbtapi.dll"
// DLL wbtapi.dll  -> WIDCOMM version 3.x and 4.x and SDK BTW-5_0_1_902-SDK
// DLL btwapi.dll  -> WIDCOMM 5.1.x and SDK BTW-5_1_0_3101
// We specify which DLLs to delay load with the /delayload:btwapi.dll linker option
// This is how it is now: wbtapi.dll;btfunc.dll;irprops.cpl

static int openConnections = 0;
static GUID test_client_service_guid = { 0x5fc2a42e, 0x144e, 0x4bb5, { 0xb4, 0x3f, 0x4e, 0x61, 0x71, 0x1d, 0x1c, 0x32 } };

void BcAddrToString(wchar_t* addressString, BD_ADDR bd_addr) {
	swprintf_s(addressString, 14, _T("%02x%02x%02x%02x%02x%02x"),
			 bd_addr[0],
             bd_addr[1],
             bd_addr[2],
             bd_addr[3],
             bd_addr[4],
             bd_addr[5]);
}

jlong BcAddrToLong(BD_ADDR bd_addr) {
	jlong l = 0;
	for (int i = 0; i < BD_ADDR_LEN; i++) {
		l = (l << 8) + bd_addr[i];
	}
	return l;
}

void LongToBcAddr(jlong addr, BD_ADDR bd_addr) {
	for (int i = BD_ADDR_LEN - 1; i >= 0; i--) {
		bd_addr[i] = (UINT8)(addr & 0xFF);
		addr >>= 8;
	}
}

jint DeviceClassToInt(DEV_CLASS devClass) {
	return (((devClass[0] << 8) + devClass[1]) << 8) + devClass[2];
}

struct deviceFound {
	jlong deviceAddr;
	jint deviceClass;
	BD_NAME bdName;
};

#define deviceRespondedMax 50
#define sdpDiscoveryRecordsUsedMax 100

class WIDCOMMStack : public CBtIf {
public:
	HANDLE hEvent;

	deviceFound deviceResponded[deviceRespondedMax];
	int deviceRespondedIdx;
	BOOL deviceInquiryTerminated;
	BOOL deviceInquiryComplete;
	BOOL deviceInquirySuccess;

	BOOL searchServicesComplete;
	int sdpDiscoveryRecordsUsed;
	CSdpDiscoveryRec sdpDiscoveryRecords[sdpDiscoveryRecordsUsedMax];

	// One CRfCommIf shared by application, lock it when connection is made
	CRITICAL_SECTION csCRfCommIf;
	CRfCommIf rfCommIf;

	WIDCOMMStack();
	virtual ~WIDCOMMStack();

	void throwExtendedErrorException(JNIEnv * env, const char *name);

    // methods to replace virtual methods in base class CBtIf
    virtual void OnDeviceResponded(BD_ADDR bda, DEV_CLASS devClass, BD_NAME bdName, BOOL bConnected);
    virtual void OnInquiryComplete(BOOL success, short num_responses);

	virtual void OnDiscoveryComplete();
};

static WIDCOMMStack* stack;

BOOL isWIDCOMMBluetoothStackPresent() {
	HMODULE h = LoadLibrary(_T(WIDCOMM_DLL));
	if (h == NULL) {
		return FALSE;
	}
	return TRUE;
}

WIDCOMMStack::WIDCOMMStack() {
	sdpDiscoveryRecordsUsed = 0;
	hEvent = CreateEvent( 
            NULL,     // no security attributes
            FALSE,    // auto-reset event
            FALSE,    // initial state is NOT signaled
            NULL);    // object not named
	InitializeCriticalSection(&csCRfCommIf);
}

WIDCOMMStack* createWIDCOMMStack() {
	return new WIDCOMMStack();
}

JNIEXPORT jboolean JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_initialize
(JNIEnv *env, jobject) {
	jboolean rc = TRUE;
	if (stack == NULL) {
		__try  {
			stack = createWIDCOMMStack();
			if (stack->hEvent == NULL) {
				throwRuntimeException(env, "fails to CreateEvent");
			}
		} __except(GetExceptionCode() == 0xC06D007E) {
			rc = FALSE;
		}
	}
	return rc;
}

WIDCOMMStack::~WIDCOMMStack() {
	SetEvent(hEvent);
	CloseHandle(hEvent);
	DeleteCriticalSection(&csCRfCommIf);
}

void WIDCOMMStack::throwExtendedErrorException(JNIEnv * env, const char *name) {
	WBtRc er = GetExtendedError();
	LPCTSTR msg = WBtRcToString(er);
	if (msg != NULL) {
		throwExceptionExt(env, name, "WIDCOMM error[%s]", msg);
	} else {
		throwException(env, name, "No error code");
	}
}

void BroadcomDebugError(JNIEnv *env, CBtIf* stack) {
	WBtRc er = stack->GetExtendedError();
	LPCTSTR msg = WBtRcToString(er);
	if (msg != NULL) {
		debugs("WIDCOMM error[%s]", msg);
	} else {
		debug("No error code");
	}
}

JNIEXPORT void JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_uninitialize
(JNIEnv *, jobject) {
	if (stack != NULL) {
		WIDCOMMStack* stackTmp = stack;
		stack = NULL;
		delete stackTmp;
	}
}


JNIEXPORT jstring JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getLocalDeviceBluetoothAddress
(JNIEnv *env, jobject peer) {
	struct CBtIf::DEV_VER_INFO info;
	if (!stack->GetLocalDeviceVersionInfo(&info)) {
		stack->throwExtendedErrorException(env, "javax/bluetooth/BluetoothStateException");
		return NULL;
	}

	wchar_t addressString[14];
	BcAddrToString(addressString, info.bd_addr);
	return env->NewString((jchar*)addressString, (jsize)wcslen(addressString));
}

JNIEXPORT jstring JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getLocalDeviceName
(JNIEnv *env, jobject peer) {
	BD_NAME name;
	if (!stack->GetLocalDeviceName(&name)) {
		BroadcomDebugError(env, stack);
		return NULL;
	}
	return env->NewStringUTF((char*)name);
}

JNIEXPORT jboolean JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_isLocalDevicePowerOn
(JNIEnv *env, jobject peer) {
	return stack->IsDeviceReady();
}

JNIEXPORT jboolean JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_isStackServerUp
(JNIEnv *env, jobject peer) {
	return stack->IsStackServerUp();
}

JNIEXPORT jstring JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getBTWVersionInfo
(JNIEnv *env, jobject peer) {
	BT_CHAR p_version[256];
	if (!stack->GetBTWVersionInfo(p_version, 256)) {
		return NULL;
	}
	return env->NewStringUTF((char*)p_version);
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getDeviceVersion
(JNIEnv *, jobject) {
	CBtIf::DEV_VER_INFO dev_Ver_Info;
	if (!stack->GetLocalDeviceVersionInfo(&dev_Ver_Info)) {
		return -1;
	}
	return dev_Ver_Info.lmp_sub_version;
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getDeviceManufacturer
(JNIEnv *, jobject) {
	CBtIf::DEV_VER_INFO dev_Ver_Info;
	if (!stack->GetLocalDeviceVersionInfo(&dev_Ver_Info)) {
		return -1;
	}
	return dev_Ver_Info.manufacturer;
}

// --- Device Inquiry

void WIDCOMMStack::OnDeviceResponded(BD_ADDR bda, DEV_CLASS devClass, BD_NAME bdName, BOOL bConnected) {
	if (stack == NULL) {
		return;
	}
	int nextDevice = deviceRespondedIdx + 1;
	if (nextDevice >= deviceRespondedMax) {
		nextDevice = 0;
	}
	deviceResponded[nextDevice].deviceAddr = BcAddrToLong(bda);
    deviceResponded[nextDevice].deviceClass = DeviceClassToInt(devClass);
	memcpy(deviceResponded[nextDevice].bdName, bdName, sizeof(BD_NAME));
	deviceRespondedIdx = nextDevice;
	SetEvent(hEvent);
}

void WIDCOMMStack::OnInquiryComplete(BOOL success, short num_responses) {
	if (stack == NULL) {
		return;
	}
	deviceInquirySuccess = success;
	deviceInquiryComplete = TRUE;
	SetEvent(hEvent);
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_runDeviceInquiryImpl
(JNIEnv * env, jobject peer, jobject startedNotify, jint accessCode, jobject listener) {
	debug("StartDeviceInquiry");
	stack->deviceInquiryComplete = FALSE;
	stack->deviceInquiryTerminated = FALSE;

    memset(stack->deviceResponded, 0, sizeof(stack->deviceResponded));
 	stack->deviceRespondedIdx = -1;

	jclass peerClass = env->GetObjectClass(peer);
	if (peerClass == NULL) {
		throwRuntimeException(env, "Fail to get Object Class");
		return INQUIRY_ERROR;
	}

	jmethodID deviceDiscoveredCallbackMethod = env->GetMethodID(peerClass, "deviceDiscoveredCallback", "(Ljavax/bluetooth/DiscoveryListener;JILjava/lang/String;)V");
	if (deviceDiscoveredCallbackMethod == NULL) {
		throwRuntimeException(env, "Fail to get MethodID deviceDiscoveredCallback");
		return INQUIRY_ERROR;
	}

	jclass notifyClass = env->GetObjectClass(startedNotify);
	if (notifyClass == NULL) {
		throwRuntimeException(env, "Fail to get Object Class");
		return INQUIRY_ERROR;
	}
	jmethodID notifyMethod = env->GetMethodID(notifyClass, "deviceInquiryStartedCallback", "()V");
	if (notifyMethod == NULL) {
		throwRuntimeException(env, "Fail to get MethodID deviceInquiryStartedCallback");
		return INQUIRY_ERROR;
	}

	if (!stack->StartInquiry()) {
		debug("deviceInquiryStart error");
		stack->throwExtendedErrorException(env, "javax/bluetooth/BluetoothStateException");
		return INQUIRY_ERROR;
	}
	debug("deviceInquiryStarted");

	env->CallVoidMethod(startedNotify, notifyMethod);
	if (ExceptionCheckCompatible(env)) {
		stack->StopInquiry();
		return INQUIRY_ERROR;
	}

	int reportedIdx = -1;

	while ((stack != NULL) && ((!stack->deviceInquiryComplete) || (reportedIdx != stack->deviceRespondedIdx))) {
		DWORD  rc = WaitForSingleObject(stack->hEvent, 200);
		if (rc == WAIT_FAILED) {
			throwRuntimeException(env, "WaitForSingleObject");
			return INQUIRY_ERROR;
		}
		if ((stack != NULL) && (reportedIdx != stack->deviceRespondedIdx)) {
			reportedIdx ++;
			if (reportedIdx >= deviceRespondedMax) {
				reportedIdx = 0;
			}
			deviceFound dev = stack->deviceResponded[reportedIdx];
			env->CallVoidMethod(peer, deviceDiscoveredCallbackMethod, listener, dev.deviceAddr, dev.deviceClass, env->NewStringUTF((char*)(dev.bdName)));
			if (ExceptionCheckCompatible(env)) {
				stack->StopInquiry();
				return INQUIRY_ERROR;
			}
		}
	}

	if (stack != NULL) {
		stack->StopInquiry();
	}

	if (stack == NULL) {
		return INQUIRY_TERMINATED;
	} else if (stack->deviceInquiryTerminated) {
		return INQUIRY_TERMINATED;
	} else if (stack->deviceInquirySuccess) {
		return INQUIRY_COMPLETED;
	} else {
		return INQUIRY_ERROR;
	}
}

JNIEXPORT jboolean JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_deviceInquiryCancelImpl
(JNIEnv *env, jobject peer, jobject nativeClass) {
	stack->deviceInquiryTerminated = TRUE;
	stack->StopInquiry();
	SetEvent(stack->hEvent);
	return TRUE;
}

// --- Service search

JNIEXPORT jlongArray JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_runSearchServicesImpl
(JNIEnv *env, jobject peer, jobject startedNotify, jbyteArray uuidValue, jlong address) {
	debug("StartSearchServices");

	BD_ADDR bda;
	LongToBcAddr(address, bda);

	GUID *p_service_guid = NULL;
	GUID service_guid;
	//If uuidValue parameter is NULL, all public browseable services for the device will be reported
	if (uuidValue != NULL) {
		jbyte *bytes = env->GetByteArrayElements(uuidValue, 0);
		// build UUID
		convertUUIDBytesToGUID(bytes, &service_guid);
		env->ReleaseByteArrayElements(uuidValue, bytes, 0);
		p_service_guid = &service_guid;
		memcpy(&test_client_service_guid, &service_guid, sizeof(GUID));
	}

	stack->searchServicesComplete = FALSE;

	if (!stack->StartDiscovery(bda, p_service_guid)) {
		debug("StartSearchServices error");
		stack->throwExtendedErrorException(env, "javax/bluetooth/BluetoothStateException");
		return NULL;
	}

	jclass notifyClass = env->GetObjectClass(startedNotify);
	if (notifyClass == NULL) {
		throwRuntimeException(env, "Fail to get Object Class");
		return NULL;
	}
	jmethodID notifyMethod = env->GetMethodID(notifyClass, "searchServicesStartedCallback", "()V");
	if (notifyMethod == NULL) {
		throwRuntimeException(env, "Fail to get MethodID searchServicesStartedCallback");
		return NULL;
	}
	env->CallVoidMethod(startedNotify, notifyMethod);
	if (ExceptionCheckCompatible(env)) {
		return NULL;
	}

	while ((stack != NULL) && (!stack->searchServicesComplete)) {
		DWORD  rc = WaitForSingleObject(stack->hEvent, INFINITE);
		if (rc == WAIT_FAILED) {
			throwRuntimeException(env, "WaitForSingleObject");
			return NULL;
		}
	}
	if (stack == NULL) {
		return NULL;
	}

	UINT16 obtainedServicesRecords;
	CBtIf::DISCOVERY_RESULT searchServicesResultCode = stack->GetLastDiscoveryResult(bda, &obtainedServicesRecords);

	//todo SERVICE_SEARCH_TERMINATED

	if (searchServicesResultCode != CBtIf::DISCOVERY_RESULT_SUCCESS) {
		debugs("searchServicesResultCode %i", searchServicesResultCode);
		return NULL;
	}
	if (obtainedServicesRecords <= 0) {
		return env->NewLongArray(0);
	}

	if (obtainedServicesRecords > sdpDiscoveryRecordsUsedMax) {
		debugs("too many ServicesRecords %i", obtainedServicesRecords);
		obtainedServicesRecords = sdpDiscoveryRecordsUsedMax;
	}
	int useIdx = stack->sdpDiscoveryRecordsUsed;
	if (useIdx + obtainedServicesRecords > sdpDiscoveryRecordsUsedMax) {
		useIdx = 0;
	}
	CSdpDiscoveryRec *sdpDiscoveryRecordsList = stack->sdpDiscoveryRecords + useIdx;

	int recSize = stack->ReadDiscoveryRecords(bda, obtainedServicesRecords, sdpDiscoveryRecordsList, p_service_guid);
	if (recSize == 0) {
		debugs("ReadDiscoveryRecords returns empty, While expected %i", obtainedServicesRecords);
		return NULL;
	}
	stack->sdpDiscoveryRecordsUsed += recSize; 

	jlongArray result = env->NewLongArray(recSize);
	jlong *longs = env->GetLongArrayElements(result, 0);
	for (int r = 0; r < recSize; r ++) {
		longs[r] = useIdx + r;
	}
	env->ReleaseLongArrayElements(result, longs, 0);
	
	return result;
}

void WIDCOMMStack::OnDiscoveryComplete() {
	searchServicesComplete = TRUE;
	SetEvent(hEvent);
}

JNIEXPORT jbyteArray JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getServiceAttributes
(JNIEnv *env, jobject peer, jint attrID, jlong handle) {
	if ((handle > sdpDiscoveryRecordsUsedMax) || (handle < 0)) {
		throwIOException(env, "Invalid handle");
		return NULL;
	}
	CSdpDiscoveryRec* record = stack->sdpDiscoveryRecords + handle;

	SDP_DISC_ATTTR_VAL val;

	if (!record->FindAttribute((UINT16)attrID, &val)) {
		// attr not found
		return NULL;
	}

	jbyteArray result = env->NewByteArray(sizeof(SDP_DISC_ATTTR_VAL));
	jbyte *bytes = env->GetByteArrayElements(result, 0);
	memcpy(bytes, &val, sizeof(SDP_DISC_ATTTR_VAL));
	env->ReleaseByteArrayElements(result, bytes, 0);
	return result;
}

//	 --- Client RFCOMM connections
#define todo_buf_max 0x10000

#define MAGIC_1 0xBC1AA01
#define MAGIC_2 0xBC2BB02

class WIDCOMMStackRfCommPort : public CRfCommPort {
public:
	long magic1;
	long magic2;

	GUID service_guid;
	BT_CHAR service_name[BT_MAX_SERVICE_NAME_LEN + 1];

	BOOL isConnected;
	BOOL isConnectionError;

	HANDLE hEvents[2];

	jbyte todo_buf[todo_buf_max];
	int todo_buf_rcv_idx;
	int todo_buf_read_idx;

	WIDCOMMStackRfCommPort();
	virtual ~WIDCOMMStackRfCommPort();

	virtual void OnEventReceived (UINT32 event_code);
	virtual void OnDataReceived (void *p_data, UINT16 len);
};

WIDCOMMStackRfCommPort::WIDCOMMStackRfCommPort() {
	todo_buf_rcv_idx = 0;
	todo_buf_read_idx = 0;
	isConnected = FALSE;
	isConnectionError = FALSE;
	service_name[0] = '\0';
    hEvents[0] = CreateEvent( 
            NULL,     // no security attributes
            FALSE,     // auto-reset event
            FALSE,    // initial state is NOT signaled
            NULL);    // object not named
    hEvents[1] = CreateEvent( 
            NULL,     // no security attributes
            FALSE,     // auto-reset event
            FALSE,     // initial state is NOT signaled
            NULL);    // object not named

	magic1 = MAGIC_1;
	magic2 = MAGIC_2;
	openConnections ++;
}

WIDCOMMStackRfCommPort::~WIDCOMMStackRfCommPort() {
	magic1 = 0;
	magic2 = 0;
	isConnected = FALSE;
	CloseHandle(hEvents[0]);
	CloseHandle(hEvents[1]);
	openConnections --;
}

WIDCOMMStackRfCommPort* validRfCommHandle(JNIEnv *env, jlong handle) {
	if (handle == 0) {
		throwIOException(env, "Invalid handle");
		return NULL;
	}
	WIDCOMMStackRfCommPort* rf = (WIDCOMMStackRfCommPort*)handle;
	if ((rf->magic1 != MAGIC_1) || (rf->magic2 != MAGIC_2)) {
		throwIOException(env, "Invalid or destroyed handle");
		return NULL;
	}
	return rf;
}

void WIDCOMMStackRfCommPort::OnEventReceived (UINT32 event_code) {
	if ((magic1 != MAGIC_1) || (magic2 != MAGIC_2)) {
		return;
	}
	if (PORT_EV_CONNECTED & event_code) {
        isConnected = TRUE;
		SetEvent(hEvents[0]);
	}
	if (PORT_EV_CONNECT_ERR & event_code) {
		isConnectionError = TRUE;
		isConnected = FALSE;
		SetEvent(hEvents[0]);
	}
}

void WIDCOMMStackRfCommPort::OnDataReceived(void *p_data, UINT16 len) {
	if ((magic1 != MAGIC_1) || (magic2 != MAGIC_2)) {
		return;
	}
	if (isConnected) {
		int accept = todo_buf_max - todo_buf_rcv_idx;
		if (len > accept) {
			len = accept;
		}
		memcpy((todo_buf + todo_buf_rcv_idx), p_data, len);
		todo_buf_rcv_idx += len;
		SetEvent(hEvents[1]);
	}
}

JNIEXPORT jlong JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfOpenClientConnection
(JNIEnv *env, jobject peer, jlong address, jint channel, jboolean authenticate, jboolean encrypt) {
	BD_ADDR bda;
	LongToBcAddr(address, bda);

	WIDCOMMStackRfCommPort* rf = new WIDCOMMStackRfCommPort();
	if ((rf->hEvents[0] == NULL) || (rf->hEvents[1] == NULL)) {
		throwRuntimeException(env, "fails to CreateEvent");
		delete rf;
		return 0;
	}
	//debug("AssignScnValue");
	// What GUID do we need in call to CRfCommIf.AssignScnValue() if we don't have any?
	memcpy(&(rf->service_guid), &test_client_service_guid, sizeof(GUID));
	EnterCriticalSection(&stack->csCRfCommIf);
	if (!stack->rfCommIf.AssignScnValue(&(rf->service_guid), (UINT8)channel)) {
		LeaveCriticalSection(&stack->csCRfCommIf);
		delete rf;
		throwIOException(env, "failed to assign SCN");
		return 0;
	}
	//debug("SetSecurityLevel");
	UINT8 sec_level = BTM_SEC_NONE;
	if (!stack->rfCommIf.SetSecurityLevel("bluecovesrv"/*rf->service_name*/, sec_level, FALSE)) {
        LeaveCriticalSection(&stack->csCRfCommIf);
		throwIOException(env, "Error setting security level");
        delete rf;
		return 0;
    }
	//debug("OpenClient");
	CRfCommPort::PORT_RETURN_CODE rc = rf->OpenClient((UINT8)channel, bda);
	if (rc != CRfCommPort::SUCCESS) {
		LeaveCriticalSection(&stack->csCRfCommIf);
		throwIOException(env, "Failed to OpenClient");
		delete rf;
		return 0;
	}
	LeaveCriticalSection(&stack->csCRfCommIf);

	while ((stack != NULL) && !rf->isConnected && !rf->isConnectionError) {
		DWORD  rc = WaitForSingleObject(rf->hEvents[0], 500);
		if (rc == WAIT_FAILED) {
			throwRuntimeException(env, "WaitForSingleObject");
			delete(rf);
			return 0;
		}
	}
	if ((stack == NULL) || rf->isConnectionError) {
		throwIOException(env, "Failed to connect");
		delete rf;
		return 0;
	}
	debug("connected");
	return (jlong)rf;
}

JNIEXPORT void JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfCloseClientConnection
(JNIEnv *env, jobject peer, jlong handle) {
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return;
	}
	//debug("CloseClientConnection");
	CRfCommPort::PORT_RETURN_CODE rc = rf->Close();
	if (rc != CRfCommPort::SUCCESS && rc != CRfCommPort::NOT_OPENED) {
		throwIOException(env, "Failed to Close");
	}
	delete rf;
	debugs("openConnections %i", openConnections);
}

JNIEXPORT jlong JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_getConnectionRfRemoteAddress
(JNIEnv *env, jobject peer, jlong handle) {
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return 0;
	}
	if (!rf->isConnected) {
		throwIOException(env, "Connection is closed");
		return 0;
	}
	BD_ADDR connected_bd_addr;
    if (rf->IsConnected(&connected_bd_addr)) {
		return BcAddrToLong(connected_bd_addr);
	} else {
		throwIOException(env, "Connection down");
		return 0;
	}
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfRead__J
(JNIEnv *env, jobject peer, jlong handle) {
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return -1;
	}
	debug("->read()");
	while (rf->isConnected && (rf->todo_buf_read_idx == rf->todo_buf_rcv_idx)) {
		if (todo_buf_max == rf->todo_buf_rcv_idx) {
			throwIOException(env, "rcv buffer overflown, Fix me");
			return 0;
		}
		DWORD  rc = WaitForMultipleObjects(2, rf->hEvents, FALSE, INFINITE);
		if (rc == WAIT_FAILED) {
			throwRuntimeException(env, "WaitForMultipleObjects");
			return 0;
		}
	}
	if (!rf->isConnected && (rf->todo_buf_read_idx == rf->todo_buf_rcv_idx)) {
		// See InputStream.read();
		return -1;
	}
	jint result = (unsigned char)rf->todo_buf[rf->todo_buf_read_idx];
	rf->todo_buf_read_idx ++;
	return result;
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfRead__J_3BII
(JNIEnv *env, jobject peer, jlong handle, jbyteArray b, jint off, jint len) {
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return -1;
	}
	debug("->read(byte[])");
	jbyte *bytes = env->GetByteArrayElements(b, 0);

	int done = 0;

	while (rf->isConnected && done < len) {
		while (rf->isConnected && rf->todo_buf_read_idx == rf->todo_buf_rcv_idx) {
			if (todo_buf_max == rf->todo_buf_rcv_idx) {
				throwIOException(env, "rcv buffer overflown, Fix me");
				return 0;
			}
			DWORD  rc = WaitForMultipleObjects(2, rf->hEvents, FALSE, INFINITE);
			if (rc == WAIT_FAILED) {
				throwRuntimeException(env, "WaitForMultipleObjects");
				return 0;
			}
		}
		int count = rf->todo_buf_rcv_idx - rf->todo_buf_read_idx;
		if (count > len - done) {
			count = len - done;
		}

		memcpy(bytes + off + done , (rf->todo_buf + rf->todo_buf_read_idx), count);
		rf->todo_buf_read_idx += count;

		done += count;
	}
	if (!rf->isConnected && done == 0) {
		// See InputStream.read();
		done = -1;
	}
	env->ReleaseByteArrayElements(b, bytes, 0);
	return done;
}

JNIEXPORT jint JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfReadAvailable
(JNIEnv *env, jobject peer, jlong handle) {
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return 0;
	}
	return (rf->todo_buf_rcv_idx - rf->todo_buf_read_idx);
}

JNIEXPORT void JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfWrite__JI
(JNIEnv *env, jobject peer, jlong handle, jint b) {
	debug("->write(int)");
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return;
	}
	if (!rf->isConnected) {
		throwIOException(env, "Failed to write to closed connection");
		return;
	}
	char c = (char)b;
	UINT16 written = 0;
	while ((written == 0) && rf->isConnected) {
		CRfCommPort::PORT_RETURN_CODE rc = rf->Write((void*)(&c), 1, &written);
		if (rc != CRfCommPort::SUCCESS) {
			throwIOException(env, "Failed to write");
			return;
		}
	}
}

JNIEXPORT void JNICALL Java_com_intel_bluetooth_BluetoothStackWIDCOMM_connectionRfWrite__J_3BII
(JNIEnv *env, jobject peer, jlong handle, jbyteArray b, jint off, jint len) {
	debug("->write(byte[])");
	WIDCOMMStackRfCommPort* rf = validRfCommHandle(env, handle);
	if (rf == NULL) {
		return;
	}
	if (!rf->isConnected) {
		throwIOException(env, "Failed to write to closed connection");
		return;
	}

	jbyte *bytes = env->GetByteArrayElements(b, 0);

	UINT16 done = 0;

	while ((done < len) && rf->isConnected) {
		UINT16 written = 0;
		CRfCommPort::PORT_RETURN_CODE rc = rf->Write((void*)(bytes + off + done), (UINT16)(len - done), &written);
		if (rc != CRfCommPort::SUCCESS) {
			env->ReleaseByteArrayElements(b, bytes, 0);
			throwIOException(env, "Failed to write");
			return;
		}
		done += written;
	}

	env->ReleaseByteArrayElements(b, bytes, 0);
}

#endif