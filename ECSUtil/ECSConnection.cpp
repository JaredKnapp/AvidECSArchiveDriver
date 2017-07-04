//
// Copyright (C) 1994 - 2017 EMC Corporation
// All rights reserved.
//


#include "stdafx.h"

using namespace std;

#include <atlbase.h>
#include <atlstr.h>
#include <atlenc.h>
#include <Winhttp.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <VersionHelpers.h>
#include "generic_defs.H"
#include "XmlLiteUtil.h"
#include "CngAES_GCM.h"
#include "UriUtils.h"
#include "NTERRTXT.H"
#include "ECSConnection.h"

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Crypt32.lib")


// WinHttp notifications that we register for:
#define ECS_CONN_WINHTTP_CALLBACK_FLAGS (WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE | WINHTTP_CALLBACK_FLAG_HANDLES)

bool CECSConnection::bInitialized = false;						// starts out false. If false, timeouts are very short. must call SetInitialized to get regular timeouts
map<CString, CECSConnection::THROTTLE_REC> CECSConnection::ThrottleMap;	// global map used by all CECSConnection objects
CCriticalSection CECSConnection::csThrottleMap;				// critical section protecting ThrottleMap
list<CECSConnection *> CECSConnection::ECSConnectionList;			// list of all CECSConnection objects (protected by csThrottleMap)
CECSConnection::CThrottleTimerThread CECSConnection::TimerThread;	// throttle timer thread
DWORD CECSConnection::dwGlobalHttpsProtocol = 0;

CCriticalSection CECSConnection::csBadIPMap;
map<CECSConnection::BAD_IP_KEY,CECSConnection::BAD_IP_ENTRY> CECSConnection::BadIPMap;		// protected by csBadIPMap
map<CString,UINT> CECSConnection::LoadBalMap;										// protected by csBadIPMap
list<CECSConnection::XML_DIR_LISTING_CONTEXT *> CECSConnection::DirListList;	// listing of current dir listing operations
CCriticalSection CECSConnection::csDirListList;				// critical section protecting DirListList
CCriticalSection CECSConnection::csSessionMap;
map<CECSConnection::SESSION_MAP_KEY, CECSConnection::SESSION_MAP_VALUE> CECSConnection::SessionMap;			// protected by rwlSessionMap
long CECSConnection::lSessionKeyValue;
CString CECSConnection::sAmzMetaPrefix(TEXT("x-amz-meta-"));						// just a place to hold "x-amz-meta-"

class CSignQuerySet
{
	static CSimpleRWLock lwrSignQuerySet;
	static set<CString> SignQuerySet;
	static LPCTSTR InitArray[];

public:
	CSignQuerySet()
	{}
	void Init()
	{
		CSimpleRWLockAcquire lock(&lwrSignQuerySet, true);		// write lock
		if (SignQuerySet.empty())
		{
			for (UINT i = 0; InitArray[i][0] != NUL; i++)
				SignQuerySet.insert(InitArray[i]);
		}
	}
	bool IfQuery(const CString& sQuery)
	{
		CSimpleRWLockAcquire lock(&lwrSignQuerySet);		// read lock
		return SignQuerySet.find(sQuery) != SignQuerySet.end();
	}
};

CSimpleRWLock CSignQuerySet::lwrSignQuerySet;
set<CString> CSignQuerySet::SignQuerySet;
LPCTSTR CSignQuerySet::InitArray[] =
{
	_T("acl"),
	_T("lifecycle"),
	_T("searchmetadata"),
	_T("location"),
	_T("logging"),
	_T("notification"),
	_T("partnumber"),
	_T("policy"),
	_T("requestpayment"),
	_T("torrent"),
	_T("uploadid"),
	_T("uploads"),
	_T("versionid"),
	_T("versioning"),
	_T("versions"),
	_T("website"),
	_T("response-content-type"),
	_T("response-content-language"),
	_T("response-expires"),
	_T("response-cache-control"),
	_T("response-content-disposition"),
	_T("response-content-encoding"),
	_T("delete"),
	_T("endpoint"),
	_T("query"),
	_T("")							// sentinel
};
CSignQuerySet SignQuerySet;

static bool IfServerReached(DWORD dwError)
{
	switch (dwError)
	{
	case ERROR_WINHTTP_HEADER_NOT_FOUND:
	case ERROR_WINHTTP_INVALID_HEADER:
	case ERROR_WINHTTP_INVALID_QUERY_REQUEST:
	case ERROR_WINHTTP_HEADER_ALREADY_EXISTS:
	case ERROR_WINHTTP_REDIRECT_FAILED:
	case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
	case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
	case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
	case ERROR_WINHTTP_UNHANDLED_SCRIPT_TYPE:
	case ERROR_WINHTTP_SCRIPT_EXECUTION_ERROR:
		break;
	default:
		return false;
	}
	return true;
}

CECSConnection::CECSConnectionState& CECSConnection::GetStateBuf(DWORD dwThreadID)
{
	if (dwThreadID == 0)
		dwThreadID = GetCurrentThreadId();
	{
		CSingleLock lock(&Events.csStateMap, true);
		// this will either insert it or if it already exists, return the existing entry for this thread
		pair<map<DWORD, CECSConnectionState>::iterator, bool> ret = Events.StateMap.insert(make_pair(dwThreadID, CECSConnectionState()));
		if (ret.second)
			ret.first->second.pECSConnection = this;
		else
			ASSERT(ret.first->second.pECSConnection == this);
		return ret.first->second;
	}
}

void CECSConnection::PrepareCmd()
{
	CECSConnectionState& State(GetStateBuf());
	ASSERT(State.dwCurrentThread == 0);
	State.dwCurrentThread = GetCurrentThreadId();
	VERIFY(State.CallbackContext.Event.evCmd.ResetEvent());
	State.CallbackContext.Reset();
	State.CallbackContext.bDisableSecureLog = State.bDisableSecureLog;
	State.CallbackContext.dwSecureError = State.dwSecureError;
	State.CallbackContext.pHost = this;
	DEBUGF(TEXT("CECSConnection::PrepareCmd: entry: State.hRequest: %x"), (HINTERNET)State.hRequest);
}

void CECSConnection::CleanupCmd()
{
	CECSConnectionState& State(GetStateBuf());
	ASSERT(State.dwCurrentThread != 0);
	State.dwCurrentThread = 0;
	State.dwSecureError = State.CallbackContext.dwSecureError;		// any security error is passed back to the main class
}

bool CECSConnection::WaitComplete(DWORD dwCallbackExpected)
{
	CECSConnectionState& State(GetStateBuf());
	DEBUGF(TEXT("CECSConnection::WaitComplete: entry, %x"), dwCallbackExpected);
	const UINT MaxWaitInterval = SECONDS(1);			// max time to wait before checking for abort
	DWORD dwError;
	UINT iTimeout = 0;
	UINT iTimeoutMax = max(dwLongestTimeout, SECONDS(90))/MaxWaitInterval + 2;
	for (;;)
	{
		// wait at most 500 ms. then check if we are terminating
		dwError = WaitForSingleObject(State.CallbackContext.Event.evCmd.m_hObject, MaxWaitInterval);
		// check for thread exit (but not if we are waiting for the handle to close)
		if ((dwCallbackExpected != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) && TestAbort())
		{
			CSingleLock lock(&State.CallbackContext.csContext, true);
			DEBUGF(_T("CECSConnection::WaitComplete: ABORT!"));
			State.CallbackContext.Result.dwError = ERROR_OPERATION_ABORTED;
			State.CallbackContext.Result.dwResult = 0;
			CleanupCmd();
			return false;
		}
		if (dwError == WAIT_OBJECT_0)
		{
			CSingleLock lock(&State.CallbackContext.csContext, true);
			bool bGotIt = false;
			// command finished!
			for (UINT i=0 ; i<State.CallbackContext.CallbacksReceived.size() ; i++)
				if (State.CallbackContext.CallbacksReceived[i] == dwCallbackExpected)
					bGotIt = true;
			State.CallbackContext.CallbacksReceived.clear();
			// if it is waiting for WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, don't exit until we get it
			// if it is waiting for anything else, return on failure, or the receipt of the correct callback
			if ((dwCallbackExpected != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) || bGotIt)
			{
				if (bGotIt || State.CallbackContext.bFailed)
				{
					CleanupCmd();
					if (State.CallbackContext.bFailed)
						return false;
					return true;
				}
			}
		}
		else if (dwError == WAIT_TIMEOUT)
		{
			CSingleLock lock(&State.CallbackContext.csContext, true);
			iTimeout++;
			if (iTimeout > iTimeoutMax)
			{
				DEBUGF(TEXT("CECSConnection::WaitComplete: TIMEOUT, %x"), dwCallbackExpected);
				State.CallbackContext.Result.dwError = WAIT_TIMEOUT;
				State.CallbackContext.Result.dwResult = 0;
				CleanupCmd();
				return false;
			}
		}
		else if (dwError == WAIT_FAILED)
		{
			CSingleLock lock(&State.CallbackContext.csContext, true);
			State.CallbackContext.Result.dwError = GetLastError();
			State.CallbackContext.Result.dwResult = 0;
			CleanupCmd();
			return false;
		}
		else
		{
			ASSERT(false);
			break;
		}
	}
	CleanupCmd();
	return false;
}

bool CECSConnection::WaitStreamSend(STREAM_CONTEXT *pStreamSend, const CSharedQueueEvent& MsgEvent)
{
	const UINT MaxWaitInterval = SECONDS(1);			// max time to wait before checking for abort
	DWORD dwError;
	UINT iTimeout = 0;
	UINT iTimeoutMax = max(dwLongestTimeout, SECONDS(90)) / MaxWaitInterval + 2;
	for (;;)
	{
		// wait then check if we are terminating
		dwError = WaitForSingleObject(MsgEvent.Event.evQueue.m_hObject, MaxWaitInterval);
		if (!pStreamSend->StreamData.empty())
		{
//			DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("WaitStreamSend: SUCCESS %d"), pStreamSend->StreamData.GetCount());
			return true;
		}
		// check for thread exit (but not if we are waiting for the handle to close)
		if (TestAbort())
		{
//			DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("WaitStreamSend: FAIL ABORT %d"), pStreamSend->StreamData.GetCount());
			return false;
		}
		if (dwError == WAIT_OBJECT_0)
			continue;							// wait some more
		if (dwError == WAIT_TIMEOUT)
		{
			iTimeout++;
			if (iTimeout > iTimeoutMax)
			{
//				DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("WaitStreamSend: FAIL TIMEOUT %d"), pStreamSend->StreamData.GetCount());
				return false;
			}
		}
		else if (dwError == WAIT_FAILED)
		{
//			DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("WaitStreamSend: FAIL FAILED %d"), pStreamSend->StreamData.GetCount());
			return false;
		}
		else
		{
			ASSERT(false);
			break;
		}
	}
//	DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("WaitStreamSend: FAIL FAILED DEFAULT %d"), pStreamSend->StreamData.GetCount());
	return false;
}

CECSConnection::CThrottleTimerThread::CThrottleTimerThread()
{
	ZeroFT(ftLast);
}

CECSConnection::CThrottleTimerThread::~CThrottleTimerThread()
{
	KillThreadWait();
};

void CECSConnection::CThrottleTimerThread::DoWork()
{
	if (dwEventRet != WAIT_TIMEOUT)
		return;
	FILETIME ftNow;

	GetSystemTimeAsFileTime(&ftNow);
	ULARGE_INTEGER LastTime;
	if (IfFTZero(ftLast))
		LastTime.QuadPart = FT_SECONDS(1);
	else
		LastTime.QuadPart = FTtoULarge(ftNow).QuadPart - FTtoULarge(ftLast).QuadPart;
	ftLast = ftNow;
	map<CString,CECSConnection::THROTTLE_REC>::iterator itMap;
	CSingleLock lock(&csThrottleMap, true);
	for (itMap=CECSConnection::ThrottleMap.begin() ; itMap!=CECSConnection::ThrottleMap.end() ; ++itMap)
	{
		if (itMap->second.Upload.iBytesSec != 0)
		{
			int iBytesSec = (int)((itMap->second.Upload.iBytesSec * LastTime.QuadPart)/FT_SECONDS(1));
			itMap->second.Upload.iBytesCurInterval += iBytesSec;		// add it in case the current count was negative, which indicates that in the previous second someone sent too many bytes, we'll make up for it now
			if (itMap->second.Upload.iBytesCurInterval > iBytesSec)
				itMap->second.Upload.iBytesCurInterval = iBytesSec;
		}
		else
			itMap->second.Upload.iBytesCurInterval = 0;
		if (itMap->second.Download.iBytesSec != 0)
		{
			int iBytesSec = (int)((itMap->second.Download.iBytesSec * LastTime.QuadPart)/FT_SECONDS(1));
			itMap->second.Download.iBytesCurInterval += iBytesSec;		// add it in case the current count was negative, which indicates that in the previous second someone sent too many bytes, we'll make up for it now
			if (itMap->second.Download.iBytesCurInterval > iBytesSec)
				itMap->second.Download.iBytesCurInterval = iBytesSec;
		}
		else
			itMap->second.Download.iBytesCurInterval = 0;
		// now notify any current objects waiting for this moment
		list<CECSConnection *>::iterator itList;
		for (itList = CECSConnection::ECSConnectionList.begin() ; itList != CECSConnection::ECSConnectionList.end() ; ++itList)
		{
			if ((*itList)->sHost == itMap->first)
			{
				CSingleLock lockStateMap(&(*itList)->Events.csStateMap, true);
				map<DWORD,CECSConnectionState>::iterator itStateMap;
				for (itStateMap = (*itList)->Events.StateMap.begin(); itStateMap != (*itList)->Events.StateMap.end(); ++itStateMap)
					VERIFY(itStateMap->second.evThrottle.SetEvent());
			}
		}
	}
}

bool CECSConnection::CThrottleTimerThread::InitInstance()
{
	SetCycleTime(SECONDS(1));
	ZeroFT(ftLast);
	return true;		// return FALSE to abort the thread
}

CECSConnection::CECSConnection()
	: Port(INTERNET_DEFAULT_HTTP_PORT)
	, bSSL(false)
	, bUseDefaultProxy(true)
	, bTestConnection(false)
	, dwProxyPort(0)
	, dwHttpsProtocol(0)
	, bCheckShutdown(true)
	, DisconnectCB(NULL)
	, sS3Region(_T("us-east-1"))
	, dwWinHttpOptionConnectRetries(0)
	, dwWinHttpOptionConnectTimeout(0)
	, dwWinHttpOptionReceiveResponseTimeout(0)
	, dwWinHttpOptionReceiveTimeout(0)
	, dwWinHttpOptionSendTimeout(0)
	, dwLongestTimeout(0)
	, dwBadIPAddrAge(0)
	, dwMaxRetryCount(MaxRetryCount)
	, dwMaxWriteRequest(MaxWriteRequest)
	, dwHttpSecurityFlags(0)
{
	CSingleLock lock(&csThrottleMap, true);
	ECSConnectionList.push_back(this);
}

void CECSConnection::Init()
{
	SignQuerySet.Init();
	bInitialized = true;
};

void CECSConnection::SetGlobalHttpsProtocol(DWORD dwGlobalHttpsProtocolParam)
{
	dwGlobalHttpsProtocol = dwGlobalHttpsProtocolParam;
}

void CECSConnection::SetHttpsProtocol(DWORD dwHttpsProtocolParam)
{
	dwHttpsProtocol = dwHttpsProtocolParam;
}

CECSConnection::~CECSConnection()
{
	CloseAll();
	CSingleLock lock(&csThrottleMap, true);
#ifdef DEBUG
	list<CECSConnection *>::size_type SizeBefore = ECSConnectionList.size();
#endif
	ECSConnectionList.remove(this);
#ifdef DEBUG
	list<CECSConnection *>::size_type SizeAfter = ECSConnectionList.size();
	ASSERT(SizeBefore == (SizeAfter + 1));
#endif
}

CECSConnection::CECSConnection(const CECSConnection& Rec)
{
	*this = Rec;
	CSingleLock lock(&csThrottleMap, true);
	ECSConnectionList.push_back(this);
}

BOOL CECSConnection::WinHttpQueryHeadersBuffer(
	__in      HINTERNET hHandle,
	__in      DWORD dwInfoLevel,
	__in_opt  LPCTSTR pwszName,
	__inout   CBuffer& RetBuf,
	__inout   LPDWORD lpdwIndex
	)
{
	DWORD dwLen = 0;
	if (WinHttpQueryHeaders(hHandle, dwInfoLevel, (pwszName == WINHTTP_HEADER_NAME_BY_INDEX) ? WINHTTP_HEADER_NAME_BY_INDEX : (LPCWSTR)TO_UNICODE(pwszName), WINHTTP_NO_OUTPUT_BUFFER, &dwLen, lpdwIndex))
		return TRUE;
	DWORD dwError = GetLastError();
	if (dwError != ERROR_INSUFFICIENT_BUFFER)
		return FALSE;
	RetBuf.SetBufSize(dwLen);
	return WinHttpQueryHeaders(hHandle, dwInfoLevel, (pwszName == WINHTTP_HEADER_NAME_BY_INDEX) ? WINHTTP_HEADER_NAME_BY_INDEX : (LPCWSTR)TO_UNICODE(pwszName), RetBuf.GetData(), &dwLen, lpdwIndex);
}

// GetCanonicalTime
// output current time in the format:
//   Thu, 05 Jun 2008 16:38:19 GMT
CString CECSConnection::GetCanonicalTime() const
{
	SYSTEMTIME stNow;

	GetSystemTime(&stNow);
	CStringW sTimeStr;
	LPWSTR pszTimeStr = sTimeStr.GetBuffer(WINHTTP_TIME_FORMAT_BUFSIZE / sizeof(WCHAR));

	if (!WinHttpTimeFromSystemTime(&stNow, pszTimeStr))
	{
		sTimeStr.ReleaseBuffer();
		return _T("");
	}
	sTimeStr.ReleaseBuffer();
	return FROM_UNICODE(sTimeStr);
}

// ParseCanonicalTime
// output current time in the format:
//   Thu, 05 Jun 2008 16:38:19 GMT
FILETIME CECSConnection::ParseCanonicalTime(LPCTSTR pszCanonTime)
{
	CStringW sCanonTime(TO_UNICODE(pszCanonTime));
	SYSTEMTIME stTime;
	FILETIME ftTime;

	ZeroFT(ftTime);
	if (!WinHttpTimeToSystemTime(sCanonTime, &stTime))
		return ftTime;
	if (!SystemTimeToFileTime(&stTime, &ftTime))
		return ftTime;
	return ftTime;
}

// ParseISO8601Date
// convert ISO 8601 date to FILETIME
//   yyyy-mm-ddThh:mm:ss.mmmZ
DWORD CECSConnection::ParseISO8601Date(LPCTSTR pszDate, FILETIME& ftDate, bool bLocal)
{
	SYSTEMTIME stDate, stDateUTC;

	ZeroMemory(&stDate, sizeof(stDate));
	ZeroFT(ftDate);
	if (_stscanf_s(pszDate, _T("%hd-%hd-%hdT%hd:%hd:%hd.%hd"),
		&stDate.wYear, &stDate.wMonth, &stDate.wDay, &stDate.wHour, &stDate.wMinute, &stDate.wSecond, &stDate.wMilliseconds) >= 3)
	{
		if (bLocal)
		{
			if (!TzSpecificLocalTimeToSystemTime(nullptr, &stDate, &stDateUTC))
				return GetLastError();
			stDate = stDateUTC;
		}
		if (!SystemTimeToFileTime(&stDate, &ftDate))
			return GetLastError();
	}
	return ERROR_SUCCESS;
}

CString CECSConnection::FormatISO8601Date(const FILETIME& ftDate, bool bLocal)
{
	SYSTEMTIME stDateUTC, stDate;
	if (!FileTimeToSystemTime(&ftDate, &stDateUTC))
		return _T("");
	if (bLocal)
	{
		if (!SystemTimeToTzSpecificLocalTime(nullptr, &stDateUTC, &stDate))
			return _T("");
	}
	else
		stDate = stDateUTC;
	return FmtNum(stDate.wYear, 4, true) + _T("-") + FmtNum(stDate.wMonth, 2, true) + _T("-") + FmtNum(stDate.wDay, 2, true)
		+ _T("T") + FmtNum(stDate.wHour, 2, true) + _T(":") + FmtNum(stDate.wMinute, 2, true) + _T(":") + FmtNum(stDate.wSecond, 2, true) + _T(".") + FmtNum(stDate.wMilliseconds, 2, true) + _T("Z");
}

static void CheckQuery(const CString& sQuery, map<CString, CString>& QueryMap)
{
	int iEqual = sQuery.Find(L'=');
	CString sToken;
	if (iEqual < 0)
		sToken = sQuery;
	else
		sToken = sQuery.Left(iEqual);
	sToken.MakeLower();
	sToken.TrimLeft();
	sToken.TrimRight();
	if (SignQuerySet.IfQuery(sToken))
	{
		(void)QueryMap.insert(make_pair(sToken, sQuery));
	}
}

// CECSConnection::signRequestS3v2
//
//Authorization = "AWS" + " " + AWSAccessKeyId + ":" + Signature;
//
//Signature = Base64( HMAC-SHA1( YourSecretAccessKeyID, UTF-8-Encoding-Of( StringToSign ) ) );
//
//StringToSign = HTTP-Verb + "\n" +
//	Content-MD5 + "\n" +
//	Content-Type + "\n" +
//	Date + "\n" +
//	CanonicalizedAmzHeaders +
//	CanonicalizedResource;
//
//CanonicalizedResource = [ "/" + Bucket ] +
//	<HTTP-Request-URI, from the protocol name up to the query string> +
//	[ subresource, if present. For example "?acl", "?location", "?logging", or "?torrent"];
//
CString CECSConnection::signRequestS3v2(const CString& secretStr, const CString& method, const CString& resource, const map<CString, HEADER_STRUCT>& headers, LPCTSTR pszExpires)
{
	map<CString, HEADER_STRUCT>::const_iterator iter;
	//		<HTTPMethod>\n
	CString sCanonical(method);
	sCanonical.MakeUpper();
	sCanonical += _T("\n");
	//		Content-MD5
	if ((iter = headers.find(_T("content-md5"))) != headers.end())
		sCanonical += iter->second.sContents + _T("\n");
	else
		sCanonical += _T("\n");

	//		Content-Type
	if ((iter = headers.find(_T("content-type"))) != headers.end())
		sCanonical += iter->second.sContents + _T("\n");
	else
		sCanonical += _T("\n");

	//		Date
	if (pszExpires != nullptr)
		sCanonical += CString(pszExpires) + _T("\n");
	else if ((iter = headers.find(_T("x-amz-date"))) != headers.end())
		sCanonical += iter->second.sContents + _T("\n");
	else if ((iter = headers.find(_T("date"))) != headers.end())
		sCanonical += iter->second.sContents + _T("\n");
	else
		sCanonical += _T("\n");

	//	CanonicalizedAmzHeaders +
	CString sToken;
	for (iter = headers.lower_bound(_T("x-")); iter != headers.end(); ++iter) {
		if ((iter->first.Find(_T("x-amz")) == 0) || (iter->first.Find(_T("x-emc")) == 0))
		{
			sToken = iter->second.sContents;
			// now get rid of multiple spaces
			int iBlank;
			while ((iBlank = sToken.Find(_T("  "))) >= 0)
				(void)sToken.Delete(iBlank);
			(void)sToken.TrimRight();
			(void)sToken.TrimLeft();
			sCanonical += iter->first + L':' + sToken + _T("\n");
		}
	}
	//	CanonicalizedResource;
	CString sResource(resource);
	int iQuestion = sResource.Find(L'?');
	if (iQuestion >= 0)
	{
		CString sQueryString = resource.Mid(iQuestion);
		(void)sResource.Delete(iQuestion, resource.GetLength() - iQuestion);
		int pos = 0;
		CString sQuery;
		map<CString,CString> QueryMap;
		for (;;)
		{
			sQuery = sQueryString.Tokenize(_T("?&"), pos);
			if (sQuery.IsEmpty())
				break;
			CheckQuery(sQuery, QueryMap);
		}
		if (!QueryMap.empty())
		{
			for (map<CString, CString>::const_iterator itMap = QueryMap.begin(); itMap != QueryMap.end(); ++itMap)
			{
				if (itMap == QueryMap.begin())
					sResource += _T("?");
				else
					sResource += _T("&");
				sResource += itMap->second;
			}
		}
	}
	//sCanonical += UriEncode(sResource);
	sCanonical += sResource;
	// Signature = Base64( HMAC-SHA1( YourSecretAccessKeyID, UTF-8-Encoding-Of( StringToSign ) ) );
	CAnsiString AnsiPassword;
	CBuffer Outbuf;

#ifdef _UNICODE
	// convert input string to UTF-8
	AnsiPassword.Set(secretStr, -1, CP_UTF8);
	// convert canonical string to UTF-8
	CAnsiString UTF8Buf;
	UTF8Buf.Set(sCanonical, -1, CP_UTF8);
#else
	AnsiPassword.Load(secretStr, (DWORD)strlen(secretStr) + 1);
	CAnsiString UTF8Buf(sCanonical);
#endif
	// HMAC-SHA1
	CCngAES_GCM Hash;
	Hash.CreateHash(BCRYPT_SHA1_ALGORITHM, AnsiPassword.GetData(), AnsiPassword.GetBufSize());
	Hash.AddHashData(UTF8Buf.GetData(), (UINT)strlen(UTF8Buf));
	Hash.GetHashData(Outbuf);

	// now convert to base64
	// Authorization = "AWS" + " " + AWSAccessKeyId + ":" + Signature;
	CString sAuthorization;
	if (pszExpires == NULL)
		sAuthorization = _T("AWS ") + sS3KeyID + _T(":") + Outbuf.EncodeBase64();
	else
		sAuthorization = Outbuf.EncodeBase64();
	return sAuthorization;
}

void CALLBACK CECSConnection::HttpStatusCallback(
	__in  HINTERNET hInternet,
	__in  DWORD_PTR dwContext,
	__in  DWORD dwInternetStatus,
	__in  LPVOID lpvStatusInformation,
	__in  DWORD dwStatusInformationLength)
{
	HTTP_CALLBACK_CONTEXT *pContext = (HTTP_CALLBACK_CONTEXT *)dwContext;
	ASSERT(pContext != NULL);
	CSingleLock lock(&pContext->csContext, true);
#ifdef DEBUG
	if (dwInternetStatus != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
	{
		ASSERT(!pContext->bComplete);
		ASSERT((dwInternetStatus == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CREATED)
				|| (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING));
	}
#endif
	DEBUGF(TEXT("CECSConnection::HttpStatusCallback: entry %x, %x, %s"), hInternet, dwInternetStatus, (LPCTSTR)FmtNum(dwContext, 0, false, true));
	pContext->CallbacksReceived.push_back(dwInternetStatus);
	switch (dwInternetStatus)
	{
	case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_SECURE_FAILURE"));
		{
			ASSERT((dwStatusInformationLength == 4) && (lpvStatusInformation != NULL));
			if ((dwStatusInformationLength == 4) && (lpvStatusInformation != NULL))
			{
				DWORD dwStatus = *((DWORD *)lpvStatusInformation);
				pContext->dwSecureError |= dwStatus;
				if (!pContext->bDisableSecureLog)
				{
					CString sMsg;
					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED))
						sMsg += _T("Certification revocation checking has been enabled, but the revocation check failed to verify whether a certificate has been revoked. The server used to check for revocation might be unreachable.\r\n");
					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT))
						sMsg += _T("SSL certificate is invalid.\r\n");
					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED))
						sMsg += _T("SSL certificate was revoked.\r\n");
 					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA))
						sMsg += _T("The function is unfamiliar with the Certificate Authority that generated the server's certificate.\r\n");
 					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID))
						sMsg += _T("SSL certificate common name (host name field) is incorrect, for example, if you entered www.microsoft.com and the common name on the certificate says www.msn.com.\r\n");
					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID))
						sMsg += _T("SSL certificate date that was received from the server is bad. The certificate is expired.\r\n");
					if (TST_BIT(dwStatus, WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR))
						sMsg += _T("The application experienced an internal error loading the SSL libraries.\r\n");
					if (!sMsg.IsEmpty())
					{
						bool bDisconnected = false;
						// check if the host is currently disconnected. if it is, suppress the error log
						if (pContext->pHost != nullptr)
						{
							if (pContext->pHost->DisconnectCB != nullptr)
							{
								pContext->pHost->DisconnectCB(pContext->pHost, nullptr, &bDisconnected);
							}
						}
						if (!bDisconnected)
							LogMessage(_T(__FILE__), __LINE__, _T("%1"), ERROR_WINHTTP_SECURE_FAILURE, (LPCTSTR)sMsg);
					}
				}
			}
		}
		break;
	case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:				// WinHttpQueryDataAvailable finished
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE %x"), hInternet);
		{
			ASSERT(dwStatusInformationLength == 4);
			pContext->dwReadLength = *((DWORD *)lpvStatusInformation);
			pContext->bComplete = true;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:				// WinHttpReceiveResponse finished
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE %x"), hInternet);
		{
			pContext->bHeadersAvail = true;
			pContext->bComplete = true;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:					// WinHttpReadData finished
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_READ_COMPLETE %x"), hInternet);
		{
			pContext->pReadData = (BYTE *)lpvStatusInformation;
			pContext->dwReadLength = dwStatusInformationLength;
			pContext->bComplete = true;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:			// WinHttpSendRequest finished
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE %x"), hInternet);
		{
			pContext->bSendRequestComplete = true;
			pContext->bComplete = true;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:				// WinHttpWriteData finished
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE %x"), hInternet);
		{
			ASSERT(dwStatusInformationLength == 4);
			pContext->dwBytesWritten = *((DWORD *)lpvStatusInformation);
			pContext->bComplete = true;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:					// Any of the above functions when an error occurs.
		{
			pContext->bFailed = true;
			pContext->bComplete = true;
			pContext->Result = *((WINHTTP_ASYNC_RESULT *)lpvStatusInformation);
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, error:%d, result:%d %x"), pContext->Result.dwError, pContext->Result.dwResult, hInternet);
		break;
	case WINHTTP_CALLBACK_STATUS_HANDLE_CREATED:
		{
			ASSERT(dwStatusInformationLength == sizeof(HINTERNET));
			HINTERNET hIntHandle = *((HINTERNET *)lpvStatusInformation);
			DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_HANDLE_CREATED: %s"), (LPCTSTR)FmtNum(hIntHandle, 0, false, true));
			pContext->bHandleCreated = true;
		}
		break;
	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		{
			ASSERT(dwStatusInformationLength == sizeof(HINTERNET));
			HINTERNET hIntHandle = *((HINTERNET *)lpvStatusInformation);
			DEBUGF(TEXT("CECSConnection::HttpStatusCallback: WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: %s %x"), (LPCTSTR)FmtNum(hIntHandle, 0, false, true), hInternet);
			pContext->bHandleClosing = true;
			pContext->bComplete = true;
			(void)WinHttpSetStatusCallback(hIntHandle, NULL, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
			if (pContext->pbCallbackRegistered != NULL)
				*(pContext->pbCallbackRegistered) = false;
			VERIFY(pContext->Event.evCmd.SetEvent());
		}
		break;
	default:
		break;
	}
}

DWORD CECSConnection::InitSession()
{
	CECSConnectionState& State(GetStateBuf());
	State.Session.AllocSession(sHost, GetCurrentServerIP());
	DWORD dwError = ERROR_SUCCESS;
	// Use WinHttpOpen to obtain a session handle.
	CString sVer;
	if (sUserAgent.IsEmpty())
		sVer = _T("Test/1.0");
	else
		sVer = sUserAgent;
	DEBUGF(TEXT("WinHttpOpen: version string:\r\n%s\r\n"), (LPCTSTR)sVer);
	if (!State.Session.pValue->hSession.IfOpen() || !State.Session.pValue->hConnect.IfOpen())
	{
		State.Session.pValue->CloseAll();
		CString sProxyString(sProxy);
		if (!sProxyString.IsEmpty())
		{
			if (dwProxyPort != 0)
				sProxyString += _T(":") + FmtNum(dwProxyPort);
			State.Session.pValue->hSession = WinHttpOpen(TO_UNICODE(sVer),
				WINHTTP_ACCESS_TYPE_NAMED_PROXY,
				TO_UNICODE(sProxyString),
				L"<local>",
				WINHTTP_FLAG_ASYNC);
		}
		else
		{
			State.Session.pValue->hSession = WinHttpOpen(TO_UNICODE(sVer),
				bUseDefaultProxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				WINHTTP_FLAG_ASYNC);
		}
		if (!State.Session.pValue->hSession.IfOpen())
			return GetLastError();
		// the protocol specific for this connection takes precedence
		DWORD dwTempProtocol = dwHttpsProtocol;
		if (dwTempProtocol == 0)
			dwTempProtocol = dwGlobalHttpsProtocol;
		if (dwTempProtocol != 0)
		{
			if (!WinHttpSetOption(State.Session.pValue->hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwTempProtocol, sizeof(dwTempProtocol)))
			{
				dwError = GetLastError();
				State.Session.pValue->CloseAll();
				return dwError;
			}
		}
		ASSERT(!State.Session.pValue->hConnect.IfOpen());
		// Specify an HTTP server.
		State.Session.pValue->hConnect = WinHttpConnect(State.Session.pValue->hSession, TO_UNICODE(GetCurrentServerIP()), Port, 0);
		if (!State.Session.pValue->hConnect.IfOpen())
		{
			dwError = GetLastError();
			State.Session.pValue->CloseAll();
			return dwError;
		}
	}
	return ERROR_SUCCESS;
}

void CECSConnection::CECSConnectionState::CloseRequest(bool bSaveCert) throw()
{
	// Close any request handle.
	if (hRequest.IfOpen())
	{
		if (bSaveCert)
		{
			// if unknown CA, get details about the certificate and get the certificate itself
			// we can present the details to the user for validation, and install the certificate
			// in the root store so this certificate will be accepted in the future
			CERT_CONTEXT *pCert;
			DWORD dwCertError;
			DWORD dwLen = sizeof pCert;
			if (!WinHttpQueryOption(hRequest, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &pCert, &dwLen))
			{
				dwCertError = GetLastError();
				if (!bDisableSecureLog)
					LogMessage(_T(__FILE__), __LINE__, _T("Error getting certificate context"), dwCertError);
			}
			else
			{
				TCHAR NameBuf[1024];
				(void)CertGetNameString(pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, NameBuf, _countof(NameBuf));
				CertInfo.sCertName = NameBuf;
				DWORD dwStrLen;
				LPTSTR pszSubject = CertInfo.sCertSubject.GetBuffer(dwStrLen = CertNameToStr(pCert->dwCertEncodingType, &(pCert->pCertInfo->Subject), CERT_X500_NAME_STR | CERT_NAME_STR_CRLF_FLAG, NULL, 0) + 2);
				(void)CertNameToStr(pCert->dwCertEncodingType, &(pCert->pCertInfo->Subject), CERT_X500_NAME_STR | CERT_NAME_STR_CRLF_FLAG, pszSubject, dwStrLen);
				CertInfo.sCertSubject.ReleaseBuffer();
 
				// Find out how much memory to allocate for the serialized element.
				DWORD dwElementLen = 0;
				if (!CertSerializeCertificateStoreElement(pCert, 0, NULL, &dwElementLen))
				{
					dwCertError = GetLastError();
					if (!bDisableSecureLog)
						LogMessage(_T(__FILE__), __LINE__, _T("CertSerializeCertificateStoreElement error"), dwCertError);
				}
				else
				{
					CertInfo.SerializedCert.SetBufSize(dwElementLen);
					if (!CertSerializeCertificateStoreElement(pCert, 0, CertInfo.SerializedCert.GetData(), &dwElementLen))
					{
						dwCertError = GetLastError();
						if (!bDisableSecureLog)
							LogMessage(_T(__FILE__), __LINE__, _T("CertSerializeCertificateStoreElement error"), dwCertError);
						CertInfo.SerializedCert.Empty();
					}
					else
					{
						CertInfo.SerializedCert.SetBufSize(dwElementLen);				// it might have gotten smaller. make sure we have the exact size
					}
				}
				(void)CertFreeCertificateContext(pCert);
			}
		}
#ifdef DEBUG
		HINTERNET hRequestSave = hRequest;
		DEBUGF(TEXT("CloseRequest: entry %s\r\n"), (LPCTSTR)FmtNum((HINTERNET)hRequest, 0, false, true));
#endif
		if (bCallbackRegistered)
			if (pECSConnection != NULL)
				pECSConnection->PrepareCmd();
		(void)hRequest.CloseHandle();
		if (bCallbackRegistered)
			if (pECSConnection != NULL)
				(void)pECSConnection->WaitComplete(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING);
		bCallbackRegistered = false;
#ifdef DEBUG
		DEBUGF(TEXT("CloseRequest: done %s\r\n"), (LPCTSTR)FmtNum(hRequestSave, 0, false, true));
#endif
	}
}

void CECSConnection::CloseAll() throw()
{
	CECSConnectionState& State(GetStateBuf());
	// Close any open handles.
	State.CloseRequest();
}

// InitHeader
// set headers common to all requests
void CECSConnection::InitHeader(void)
{
	CECSConnectionState& State(GetStateBuf());
	State.Headers.clear();
	// add the list of headers common to all requests
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
}

void CECSConnection::AddHeader(LPCTSTR pszHeaderLabel, LPCTSTR pszHeaderText, bool bOverride)
{
	CECSConnectionState& State(GetStateBuf());
	CString sLabelLower(pszHeaderLabel);
	sLabelLower.MakeLower();
	HEADER_STRUCT Hdr;
	Hdr.sLabel = pszHeaderLabel;
	Hdr.sContents = pszHeaderText;
	pair<map<CString, HEADER_STRUCT>::iterator,bool> ret = State.Headers.insert(make_pair(sLabelLower, Hdr));
	if (!ret.second && bOverride)
	{
		ret.first->second.sLabel = pszHeaderLabel;
		ret.first->second.sContents = pszHeaderText;
	}
}

// GetCurrentServerIP
// pick from one of the ip addrs going to this host
LPCTSTR CECSConnection::GetCurrentServerIP(void)
{
	CECSConnectionState& State(GetStateBuf());
	if (State.IPListLocal.empty())
		return _T("?");
	if (State.iIPList >= State.IPListLocal.size())
		State.iIPList = 0;
	return State.IPListLocal[State.iIPList];
}

// GetNextECSIP
// pick from one of the ip addrs going to this host
// returns false if there are no available IPs for this host
bool CECSConnection::GetNextECSIP(map<CString,BAD_IP_ENTRY>& IPUsed)
{
	CECSConnectionState& State(GetStateBuf());
	if (dwBadIPAddrAge == 0)
	{
		dwBadIPAddrAge = HOURS(2);
	}
	__int64 liBadIPAge = (__int64)dwBadIPAddrAge * 10000;			// convert to FILETIME units
	if (State.IPListLocal.empty())
		return false;
	// first see if any of the entries are too old to be in there
	{
		CSingleLock csBad(&csBadIPMap, true);
		FILETIME ftNow;
		GetSystemTimeAsFileTime(&ftNow);
		for (map<BAD_IP_KEY,BAD_IP_ENTRY>::iterator itMap=BadIPMap.begin() ; itMap!=BadIPMap.end() ; )
		{
			if (ftNow > (itMap->second.ftError + liBadIPAge))
				itMap = BadIPMap.erase(itMap);
			else
				++itMap;
		}

		// insert new LoadBalMap record, or retrieve existing record
		pair<map<CString, UINT>::iterator, bool> Ret = LoadBalMap.insert(make_pair(sHost, 0));
		if (++Ret.first->second >= State.IPListLocal.size())
			Ret.first->second = 0;
		State.iIPList = Ret.first->second;
	}
	bool bBad;
	UINT i;
	for (i=0 ; i<State.IPListLocal.size() ; i++)
	{
		bBad = false;
		if (State.iIPList >= State.IPListLocal.size())
			State.iIPList = 0;
		{
			CSingleLock lock(&csBadIPMap, true);
			// check if this entry is BAD
			if (BadIPMap.find(BAD_IP_KEY(sHost, State.IPListLocal[State.iIPList])) != BadIPMap.end())			//lint !e864 (Info -- Expression involving variable 'CECSConnection::BadIPMap' possibly depends on order of evaluation)
				bBad = true;					// bad - move to the next
		}
		// check if we've already tried this one
		if (!bBad && (IPUsed.find(State.IPListLocal[State.iIPList]) != IPUsed.end()))				//lint !e864 (Info -- Expression involving variable 'IPUsed' possibly depends on order of evaluation)
			bBad = true;
		if (!bBad)
			break;
		++State.iIPList;					// try the next
	}
	return i < State.IPListLocal.size();
}

// TestAllIPBad
// check if all IPs are marked bad for this host
// if so, remove them all from the bad map so it can be retried
void CECSConnection::TestAllIPBad()
{
	CECSConnectionState& State(GetStateBuf());
	deque<CString> IPList = State.IPListLocal;
	{
		// we need to test and reset a rare possibilty that all of the IPs for this host have been marked bad
		// normally that shouldn't happen because an IP is only marked bad if another IP was successful
		// but due to multiple overlapped requests, it is a possibility
		// in this case, the failure that caused the bad IPs was likely intermittent, so reset them and try again
		{
			CSingleLock csBad(&csBadIPMap, true);

			// run through the map, for each entry for this host, delete any corresponding entry in IPList
			// if, after going through the whole map, if IPList is empty that means that all of the IPs were marked bad
			for (map<BAD_IP_KEY, BAD_IP_ENTRY>::iterator itMap = BadIPMap.begin(); itMap != BadIPMap.end(); ++itMap)
			{
				if (sHost == itMap->first.sHostName)
				{
					for (deque<CString>::iterator it = IPList.begin(); it != IPList.end(); )
					{
						if (*it == itMap->first.sIP)
							it = IPList.erase(it);
						else
							++it;
					}
				}
			}
			// if IPList is now empty, all entries were marked bad. unmark them
			if (IPList.empty())
			{
				for (map<BAD_IP_KEY, BAD_IP_ENTRY>::iterator itMap = BadIPMap.begin(); itMap != BadIPMap.end();)
				{
					if (sHost == itMap->first.sHostName)
						itMap = BadIPMap.erase(itMap);
					else
						++itMap;
				}
			}
		}
	}
}

// LogBadIPAddr
// log the list of addresses that were unsuccessful
// this is only called if it eventually did connect via one of the addresses
// if all of the addresses failed, it calls the 'disconnect' callback and will try again later
// there is no point in blaming the problem on each individual address
void CECSConnection::LogBadIPAddr(const map<CString,BAD_IP_ENTRY>& IPUsed)
{
	CSingleLock csBad(&csBadIPMap, true);
	FILETIME ftNow;

	BAD_IP_ENTRY Entry;
	GetSystemTimeAsFileTime(&ftNow);
	for (map<CString,BAD_IP_ENTRY>::const_iterator itUsed=IPUsed.begin() ; itUsed != IPUsed.end() ; ++itUsed)
	{
		Entry.ftError = ftNow;
		Entry.ErrorInfo = itUsed->second.ErrorInfo;
		pair<map<BAD_IP_KEY,BAD_IP_ENTRY>::iterator,bool> Ret = BadIPMap.insert(make_pair(BAD_IP_KEY(sHost, itUsed->first), Entry));
		if (!Ret.second)				// already in the map
			Ret.first->second = Entry;
		else
		{
			LogMessage(itUsed->second.ErrorInfo.sFile, itUsed->second.ErrorInfo.dwLine, _T("Server (%1) error caused a failover to a different connection: %2\r\nConnection that failed: %3\r\nConnection that is now in use: %4"),
					itUsed->second.ErrorInfo.Error.dwError, (LPCTSTR)GetHost(), (LPCTSTR)itUsed->second.ErrorInfo.Format(), (LPCTSTR)itUsed->first, GetCurrentServerIP());
		}
	}
}

// SendRequest
// complete the request and send it, and get the response
// adds the following headers:
//	if dwDatalen != 0: content-length: <dwDataLen>
CECSConnection::S3_ERROR CECSConnection::SendRequest(
	LPCTSTR pszMethod,
	LPCTSTR pszResource,
	const void *pData,
	DWORD dwDataLen,
	CBuffer& RetData,
	list<HEADER_REQ> *pHeaderReq,
	DWORD dwReceivedDataHint,				// an approximate size of the received data to aid in allocation
	DWORD dwBufOffset,						// reserve dwBufOffset bytes at the start of the buffer
	STREAM_CONTEXT *pStreamSend,			// if supplied, don't use pData/dwDataLen. wait on this queue for data to send
	STREAM_CONTEXT *pStreamReceive,			// is supplied, don't return data through RetData, push it on this queue
	ULONGLONG ullTotalLen)					// for StreamSend the total length of the transfer, for StreamReceive, the starting offset into the object
{
	CECSConnectionState& State(GetStateBuf());
	bool bGotServerResponse = false;
	CS3ErrorInfo Error;
	map<CString,BAD_IP_ENTRY> IPUsed;
	try
	{
		// first get a local copy of the IPList for use by this request
		{
			CSimpleRWLockAcquire lock(&rwlIPListHost);			// read lock
			State.IPListLocal = IPListHost;
		}
		for (bool bFirst = true;;)
		{
			if (!GetNextECSIP(IPUsed))
			{
				if (!bFirst)
					break;
				TestAllIPBad();					// if first time through and there are no IPs it could be that they are all marked bad
				if (!GetNextECSIP(IPUsed))
					throw CS3ErrorInfo((DWORD)SEC_E_NO_IP_ADDRESSES);		// no IP addresses!
			}
			bFirst = false;
			for (UINT i = 0; i<dwMaxRetryCount; i++)
			{
				bGotServerResponse = false;
				Error = SendRequestInternal(pszMethod, pszResource, pData, dwDataLen, RetData, pHeaderReq, dwReceivedDataHint, dwBufOffset, &bGotServerResponse, pStreamSend, pStreamReceive, ullTotalLen);
				if (!Error.IfError())
				{
					// no error - but let's look a little closer
					// we MUST have gotten to the server, AND there should be an HTTP error code returned which should be 200
					if (bGotServerResponse && (Error.Error.dwHttpError == HTTP_STATUS_OK))
					{
						if (!IPUsed.empty())
							LogBadIPAddr(IPUsed);
						return Error.Error;							// success!
					}
					if (!bGotServerResponse)
						Error.dwError = ERROR_HOST_UNREACHABLE;			// fake an error code if it was successful, but didn't reach the server
					else if (Error.Error.dwHttpError != HTTP_STATUS_OK)
						Error.dwError = (DWORD)HTTP_E_STATUS_UNEXPECTED;
					else
						Error.dwError = ERROR_UNEXP_NET_ERR;			// should never get here
				}
				// don't retry if this is a stream operation
				// we can't retry because the data stream would need to be reset
				if ((pStreamSend != NULL) || (pStreamReceive != NULL))
					throw CS3ErrorInfo(Error);
				if (bGotServerResponse || (Error.Error.dwError == ERROR_WINHTTP_SECURE_FAILURE))
					throw CS3ErrorInfo(Error);					// if we got through to the server, no need to retry
				// if timeout, try reducing the max write size
				if ((!bInitialized || bTestConnection) && (Error.Error.dwError == ERROR_WINHTTP_TIMEOUT))
					throw CS3ErrorInfo(Error);					// during initialization - don't retry. if it fails right away too bad
				if (Error.Error.dwError == ERROR_WINHTTP_TIMEOUT)
				{
					DWORD dwMaxWriteRequestSave = dwMaxWriteRequest;
					DWORD dwWinHttpOptionReceiveResponseTimeoutSave = dwWinHttpOptionReceiveResponseTimeout;
					if (CString(pszMethod).CompareNoCase(_T("PUT")) == 0)
					{
						if (dwMaxWriteRequest == 0)
							dwMaxWriteRequest = MaxWriteRequest;
						if (dwMaxWriteRequest == 0)
							dwMaxWriteRequest = 0x10000;					// if 0, start out reducing it to 65k
						else
							dwMaxWriteRequest = dwMaxWriteRequest/2;
						if (dwMaxWriteRequest < 8192)
							dwMaxWriteRequest = 8192;
					}
					// add a little more time for waiting
					if (dwWinHttpOptionReceiveResponseTimeout < SECONDS(60))
						dwWinHttpOptionReceiveResponseTimeout += SECONDS(10);
					// now propagate this to all current entries
					if ((dwMaxWriteRequestSave != dwMaxWriteRequest)
						|| (dwWinHttpOptionReceiveResponseTimeoutSave != dwWinHttpOptionReceiveResponseTimeout))
					{
						CSingleLock lock(&csThrottleMap, true);
						list<CECSConnection *>::iterator itList;
						for (itList = ECSConnectionList.begin() ; itList != ECSConnectionList.end() ; ++itList)
						{
							if ((*itList)->sHost == sHost)
							{
								(*itList)->dwWinHttpOptionReceiveResponseTimeout = dwWinHttpOptionReceiveResponseTimeout;
								(*itList)->dwMaxWriteRequest = dwMaxWriteRequest;
							}
						}
					}
					else
						break;
				}
				if (Error.Error.dwError == ERROR_OPERATION_ABORTED)		// if aborted, don't retry.
					return Error.Error;
			}
			// save the error for the log message
			pair<map<CString,BAD_IP_ENTRY>::iterator,bool> Ret = IPUsed.insert(make_pair(GetCurrentServerIP(), BAD_IP_ENTRY(Error)));
			ASSERT(Ret.second);
			// try another IP address in the list
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E;
	}
	// must be some kind of comm error, call the "disconnect" callback
	if (Error.IfError() && (DisconnectCB != NULL))
	{
		if ((!bGotServerResponse
				&& (Error.Error.dwError != ERROR_WINHTTP_INVALID_SERVER_RESPONSE))
			|| (Error.Error.S3Error == S3_ERROR_InvalidAccessKeyId)
			|| (Error.Error.S3Error == S3_ERROR_RequestTimeTooSkewed))
		{
			if (Error.Error.dwError != ERROR_OPERATION_ABORTED)
			{
				Error.sAdditionalInfo = CString(pszMethod) + _T("|") + pszResource;
				DisconnectCB(this, &Error, nullptr);
			}
		}
	}
	if (!Error.Error.IfError() && !IPUsed.empty())
	{
		// got through but had problems with at least one IP address
		LogBadIPAddr(IPUsed);
	}
	return Error.Error;
}

struct XML_ECS_ERROR_CONTEXT
{
	CECSConnection::S3_ERROR *pAtError;
	XML_ECS_ERROR_CONTEXT()
		: pAtError(NULL)
	{}
};

const WCHAR * const XML_ECS_ADMIN_ERROR_CODE = L"//error/code";
const WCHAR * const XML_ECS_ADMIN_ERROR_DESC = L"//error/description";
const WCHAR * const XML_ECS_ADMIN_ERROR_DETAILS = L"//error/details";
const WCHAR * const XML_ECS_ADMIN_ERROR_RETRYABLE = L"//error/retryable";

HRESULT XmlECSAdminErrorCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_ECS_ERROR_CONTEXT *pInfo = (XML_ECS_ERROR_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_ECS_ADMIN_ERROR_CODE) == 0)
			{
				pInfo->pAtError->S3Error = S3TranslateError(FROM_UNICODE(*psValue));
				pInfo->pAtError->sS3Code = FROM_UNICODE(*psValue);
			}
			else if ((sXmlPath.CompareNoCase(XML_ECS_ADMIN_ERROR_DESC) == 0)
				|| (sXmlPath.CompareNoCase(XML_ECS_ADMIN_ERROR_DETAILS) == 0))
			{
				if (!pInfo->pAtError->sDetails.IsEmpty())
					pInfo->pAtError->sDetails += _T(": ");
				pInfo->pAtError->sDetails += FROM_UNICODE(*psValue);
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

const WCHAR * const XML_S3_ERROR_CODE = L"//Error/Code";
const WCHAR * const XML_S3_ERROR_MESSAGE = L"//Error/Message";
const WCHAR * const XML_S3_ERROR_RESOURCE = L"//Error/Resource";
const WCHAR * const XML_S3_ERROR_REQUEST_ID = L"//Error/RequestId";

HRESULT XmlS3ErrorCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_ECS_ERROR_CONTEXT *pInfo = (XML_ECS_ERROR_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_S3_ERROR_CODE) == 0)
			{
				pInfo->pAtError->S3Error = S3TranslateError(FROM_UNICODE(*psValue));
				pInfo->pAtError->sS3Code = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_ERROR_MESSAGE) == 0)
				pInfo->pAtError->sDetails = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ERROR_RESOURCE) == 0)
				pInfo->pAtError->sS3Resource = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ERROR_REQUEST_ID) == 0)
				pInfo->pAtError->sS3RequestID = FROM_UNICODE(*psValue);
		}
		break;

	default:
		break;
	}
	return 0;
}

// SetTimeouts
// set timeouts if there are any that are not default
void CECSConnection::SetTimeouts(const CInternetHandle& hRequest)
{
	// if any of the timeout values are zero, replace them with the winhttp default
	if (dwWinHttpOptionConnectRetries == 0)
		dwWinHttpOptionConnectRetries = 5;
	if (dwWinHttpOptionConnectTimeout == 0)
		dwWinHttpOptionConnectTimeout = SECONDS(60);
	if (dwWinHttpOptionReceiveResponseTimeout == 0)
		dwWinHttpOptionReceiveResponseTimeout = SECONDS(90);
	if (dwWinHttpOptionReceiveTimeout == 0)
		dwWinHttpOptionReceiveTimeout = SECONDS(30);
	if (dwWinHttpOptionSendTimeout == 0)
		dwWinHttpOptionSendTimeout = SECONDS(30);

	// figure out the longest timeout value for all of them
	dwLongestTimeout = __max(dwWinHttpOptionConnectTimeout, __max(dwWinHttpOptionReceiveResponseTimeout, __max(dwWinHttpOptionReceiveTimeout, dwWinHttpOptionSendTimeout)));

	if (bInitialized && !bTestConnection)
	{
		if (!WinHttpSetTimeouts(hRequest, 0, dwWinHttpOptionConnectTimeout, dwWinHttpOptionSendTimeout, dwWinHttpOptionReceiveTimeout))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());

		if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_RETRIES, &dwWinHttpOptionConnectRetries, sizeof(dwWinHttpOptionConnectRetries))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &dwWinHttpOptionConnectTimeout, sizeof(dwWinHttpOptionConnectTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &dwWinHttpOptionReceiveResponseTimeout, sizeof(dwWinHttpOptionReceiveResponseTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &dwWinHttpOptionReceiveTimeout, sizeof(dwWinHttpOptionReceiveTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &dwWinHttpOptionSendTimeout, sizeof(dwWinHttpOptionSendTimeout)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
	}
	else
	{
		DWORD dwTimeout = SECONDS(2);
		if (bTestConnection)
			dwTimeout = SECONDS(15);
		// short timeouts used only during initialization
		if (!WinHttpSetTimeouts(hRequest, 0, dwTimeout, dwTimeout, dwTimeout))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		DWORD dwRetries = 1;
		if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_RETRIES, &dwRetries, sizeof(dwRetries))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &dwTimeout, sizeof(dwTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &dwTimeout, sizeof(dwTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &dwTimeout, sizeof(dwTimeout))
			|| !WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &dwTimeout, sizeof(dwTimeout)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
	}
}

// used where a class method can't be used
static bool TestAbortStatic(void *pContext)
{
	CECSConnection *pHost = (CECSConnection *)pContext;
	return pHost->TestAbort();
}

// SendRequestInternal
// complete the request and send it, and get the response
// adds the following headers:
//	if dwDatalen != 0: content-length: <dwDataLen>
CECSConnection::CS3ErrorInfo CECSConnection::SendRequestInternal(
	LPCTSTR pszMethod,
	LPCTSTR pszResource,
	const void *pData,
	DWORD dwDataLen,
	CBuffer& RetData,
	list<HEADER_REQ> *pHeaderReq,
	DWORD dwReceivedDataHint,				// an approximate size of the received data to aid in allocation
	DWORD dwBufOffset,						// reserve dwBufOffset bytes at the start of the buffer
	bool *pbGotServerResponse,				// if this is set, that means it got to the server. don't retry it. Also, don't call the disconnected callback
	STREAM_CONTEXT *pStreamSend,			// if supplied, don't use pData/dwDataLen. wait on this queue for data to send
	STREAM_CONTEXT *pStreamReceive,			// is supplied, don't return data through RetData, push it on this queue
	ULONGLONG ullTotalLen)					// for StreamSend the total length of the transfer, for StreamReceive, the starting offset into the object
{
	CECSConnectionState& State(GetStateBuf());
	S3_ERROR Error;
	// use const version of pStreamSend to use "read" locks instead of "write" locks when only reading is being done
	const STREAM_CONTEXT *pConstStreamSend = (const STREAM_CONTEXT *)pStreamSend;
	try
	{
		// fixup the host header line (if it exists)
		{
			map<CString, HEADER_STRUCT>::iterator itHeader = State.Headers.find(_T("host"));
			if (itHeader != State.Headers.end())
				itHeader->second.sContents = CString(GetCurrentServerIP()) + _T(":") + FmtNum(Port);
		}
		State.dwSecureError = 0;
		if (pbGotServerResponse != NULL)
			*pbGotServerResponse = false;
		if (pConstStreamSend != NULL)
		{
			AddHeader(_T("content-length"), FmtNum(ullTotalLen));
			if (!pConstStreamSend->bMultiPart)
				AddHeader(_T("content-type"), _T("application/octet-stream"), false);
		}
		else if (dwDataLen != 0)
		{
			AddHeader(_T("content-length"), FmtNum(dwDataLen));
			AddHeader(_T("content-type"), _T("application/octet-stream"), false);			// set to a generic value, but if there is already a header there, leave it
		}
		// now construct the signature for the header
		CString sMethod(pszMethod), sResource(pszResource);
		CString sSignature;
		sSignature = signRequestS3v2(sSecret, sMethod, sResource, State.Headers);

		DWORD dwError = InitSession();
		if (dwError != ERROR_SUCCESS)
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
		// close any current request
		State.CloseRequest();
		// Create an HTTP request handle.
		State.hRequest = WinHttpOpenRequest(State.Session.pValue->hConnect, TO_UNICODE(pszMethod), TO_UNICODE(pszResource),
			NULL, WINHTTP_NO_REFERER, 
			WINHTTP_DEFAULT_ACCEPT_TYPES, 
			(bSSL ? WINHTTP_FLAG_SECURE : 0));
		State.bCallbackRegistered = false;
		State.CallbackContext.pbCallbackRegistered = &State.bCallbackRegistered;
		if (!State.hRequest.IfOpen())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		if (WinHttpSetStatusCallback(State.hRequest, CECSConnection::HttpStatusCallback, ECS_CONN_WINHTTP_CALLBACK_FLAGS, NULL) == WINHTTP_INVALID_STATUS_CALLBACK)
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		DWORD_PTR dwpContext = (DWORD_PTR)&State.CallbackContext;
		if (!WinHttpSetOption(State.hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &dwpContext, sizeof(dwpContext)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		State.bCallbackRegistered = true;
		DWORD dwAutoLogon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		if (!WinHttpSetOption(State.hRequest, WINHTTP_OPTION_AUTOLOGON_POLICY, &dwAutoLogon, sizeof(dwAutoLogon)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		DWORD dwSecurityFlags = dwHttpSecurityFlags | State.dwSecurityFlagsAdd & ~State.dwSecurityFlagsSub;
		if (!WinHttpSetOption(State.hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
		SetTimeouts(State.hRequest);
		CString sHeaders;
		map<CString,HEADER_STRUCT>::const_iterator iter;
		for (iter=State.Headers.begin() ; iter != State.Headers.end() ; ++iter)
		{
			sHeaders += iter->second.sLabel + _T(":") + iter->second.sContents + _T("\r\n");
		}
		if (!State.bS3Admin)
			sHeaders += _T("Authorization: ") + sSignature + _T("\r\n");
		DEBUGF(TEXT("WinHttpSendRequest: headers:\r\n%s\r\n"), (LPCTSTR)sHeaders);
		DWORD dwIndex;
		CBuffer RetBuf;
		bool bUploadThrottle;
		bool bDownloadThrottle;
		bool bAuthFailure = false;
		IfThrottle(&bDownloadThrottle, &bUploadThrottle);
		// loop here in case the request needs to be resent because the proxy server needs authorization
		for (UINT iRetryAuth=0 ; iRetryAuth<3 ; iRetryAuth++)
		{
			// send authorization, if we've already determined it was necessary
			if ((State.dwProxyAuthScheme != 0) && !sProxyUser.IsEmpty())
			{
				// if passport, we must use WinHttpSetOption instead of WinHttpSetCredentials
				if (State.dwProxyAuthScheme == WINHTTP_AUTH_SCHEME_PASSPORT)
				{
					DEBUGF(_T("SetCredentials - passport: '%s', '%s'"), (LPCTSTR)sProxyUser, (LPCTSTR)sProxyPassword);
					if (!WinHttpSetOption(State.hRequest, WINHTTP_OPTION_PROXY_USERNAME, (void *)(LPCTSTR)sProxyUser, sProxyUser.GetLength()))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
					if (!WinHttpSetOption(State.hRequest, WINHTTP_OPTION_PROXY_PASSWORD, (void *)(LPCTSTR)sProxyPassword, sProxyPassword.GetLength()))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
				}
				else
				{
					DEBUGF(_T("WinHttpSetCredentials: '%s', '%s'"), (LPCTSTR)sProxyUser, (LPCTSTR)sProxyPassword);
					if (!WinHttpSetCredentials(State.hRequest, WINHTTP_AUTH_TARGET_PROXY, State.dwProxyAuthScheme, TO_UNICODE(sProxyUser), TO_UNICODE(sProxyPassword), NULL))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
				}
			}
			if ((State.dwAuthScheme != 0) && !State.sHTTPUser.IsEmpty())
			{
				DEBUGF(_T("WinHttpSetCredentials: '%s', '%s'"), (LPCTSTR)State.sHTTPUser, (LPCTSTR)State.sHTTPPassword);
				if (!WinHttpSetCredentials(State.hRequest, WINHTTP_AUTH_TARGET_SERVER, State.dwAuthScheme, TO_UNICODE(State.sHTTPUser), TO_UNICODE(State.sHTTPPassword), NULL))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
			}
			{
				DWORD dwDataPartLen = dwDataLen;
				ULONGLONG ullCurDataSent = 0ULL;
				if ((dwMaxWriteRequest > 0) && (dwDataPartLen > dwMaxWriteRequest))
					dwDataPartLen = dwMaxWriteRequest;
				if (bUploadThrottle && (pConstStreamSend == NULL))
					dwDataPartLen = 0;							// just start the request but send the data below where it is controlled by the throttle
				for (;;)
				{
					PrepareCmd();
					bool bSendReturn;
					if (pConstStreamSend == NULL)
						bSendReturn = WinHttpSendRequest(State.hRequest, TO_UNICODE((LPCTSTR)sHeaders), (DWORD)sHeaders.GetLength(), const_cast<void *>(pData), dwDataPartLen, dwDataLen, (DWORD_PTR)&State.CallbackContext) != FALSE;
					else
						bSendReturn = WinHttpSendRequest(State.hRequest, TO_UNICODE((LPCTSTR)sHeaders), (DWORD)sHeaders.GetLength(), WINHTTP_NO_REQUEST_DATA, 0, (ullTotalLen > ULONG_MAX) ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : (DWORD)ullTotalLen, (DWORD_PTR)&State.CallbackContext) != FALSE;
					if (!bSendReturn)
					{
						dwError = GetLastError();
						CleanupCmd();
						if (dwError == ERROR_WINHTTP_RESEND_REQUEST)
							continue;
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
					}
					if (!WaitComplete(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE))
					{
						if (State.CallbackContext.Result.dwError == ERROR_WINHTTP_RESEND_REQUEST)
							continue;
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, State.CallbackContext.Result.dwError);
					}
					break;
				}
				if (pConstStreamSend != NULL)
				{
					dwDataPartLen = 0;							// didn't send any data yet
					ullCurDataSent = 0ULL;
				}
				ullCurDataSent += (ULONGLONG)dwDataPartLen;
				CSharedQueueEvent MsgEvent;		// event that a new message arrived on pStreamSend
				if (pConstStreamSend != NULL)
				{
					MsgEvent.Link(&pStreamSend->StreamData);					// link the queue to the event
					MsgEvent.DisableAllTriggerEvents();
					MsgEvent.EnableTriggerEvents(TRIGGEREVENTS_PUSH | TRIGGEREVENTS_INSERTAT);
					MsgEvent.SetAllEvents();
					MsgEvent.Enable();
				}
//				if (pConstStreamSend != NULL)
//					DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("SendRequestInternal: %s, before WriteData, %d"), pszResource, pConstStreamSend->StreamData.IsEmpty() ? 0 : pConstStreamSend->StreamData.GetAt(0)->Data.GetBufSize());
				// stream data available
				bool bStreamBufAvailable = false;
				for (;;)
				{
					bool bWriteDataSuccess;
					bool bDoWriteData = true;
					State.CallbackContext.dwBytesWritten = 0;
					if (pConstStreamSend == NULL)
					{
						if (ullCurDataSent >= (ULONGLONG)dwDataLen)
							break;
						dwDataPartLen = dwDataLen - (DWORD)ullCurDataSent;
						if ((dwMaxWriteRequest > 0) && (dwDataPartLen > dwMaxWriteRequest))
							dwDataPartLen = dwMaxWriteRequest;
						PrepareCmd();
						bWriteDataSuccess = WinHttpWriteData(State.hRequest, (BYTE *)pData + (DWORD)ullCurDataSent, dwDataPartLen, NULL) != FALSE;
					}
					else
					{
						if (!bStreamBufAvailable)
						{
							// need more data
//							DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("StreamSend: %s, %d"), pszResource, pConstStreamSend->StreamData.GetCount());
							// if queue is empty, wait for something to happen
							if (pConstStreamSend->StreamData.empty())
								if (!WaitStreamSend(pStreamSend, MsgEvent))
									throw CS3ErrorInfo(_T(__FILE__), __LINE__, WAIT_TIMEOUT);
							if (pConstStreamSend->StreamData.empty())
								throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_NO_DATA_DETECTED);
							dwDataPartLen = 0;
							ullCurDataSent = 0ULL;
							bStreamBufAvailable = true;
						}
						if (!pConstStreamSend->StreamData.front().Data.IsEmpty())
						{
							if (ullCurDataSent >= (ULONGLONG)pConstStreamSend->StreamData.front().Data.GetBufSize())
							{
								bWriteDataSuccess = true;				// empty data, don't write it but say it was successful
								bDoWriteData = false;
							}
							else
							{
								dwDataPartLen = pConstStreamSend->StreamData.front().Data.GetBufSize() - (DWORD)ullCurDataSent;
								if ((dwMaxWriteRequest > 0) && (dwDataPartLen > dwMaxWriteRequest))
									dwDataPartLen = dwMaxWriteRequest;
								PrepareCmd();
								bWriteDataSuccess = WinHttpWriteData(State.hRequest, pConstStreamSend->StreamData.front().Data.GetData() + ullCurDataSent, dwDataPartLen, NULL) != FALSE;
							}
						}
						else
						{
							bWriteDataSuccess = true;				// empty data, don't write it but say it was successful
							bDoWriteData = false;
						}
					}
					if (!bWriteDataSuccess)
					{
						dwError = GetLastError();
//						DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("StreamSend: %s, ERROR %s"), pszResource, NTLT(dwError));
						CleanupCmd();
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
					}
					if (bDoWriteData)
					{
						if (!WaitComplete(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE))
							throw CS3ErrorInfo(_T(__FILE__), __LINE__, State.CallbackContext.Result.dwError);

						ullCurDataSent += (ULONGLONG)State.CallbackContext.dwBytesWritten;
					}
					bool bLast = false;
					if (pConstStreamSend == NULL)
						ASSERT(State.CallbackContext.dwBytesWritten == dwDataPartLen);
					else
					{
						if (pConstStreamSend->UpdateProgressCB != NULL)
						{
							pStreamSend->iAccProgress += (int)State.CallbackContext.dwBytesWritten;
							pConstStreamSend->UpdateProgressCB((int)State.CallbackContext.dwBytesWritten, pConstStreamSend->pContext);
						}
						if (ullCurDataSent >= (ULONGLONG)pConstStreamSend->StreamData.front().Data.GetBufSize())
						{
							CRWLockAcquire lockQueue(&pConstStreamSend->StreamData.GetLock(), true);			// write lock
							bLast = pConstStreamSend->StreamData.front().bLast;
							pStreamSend->StreamData.pop_front();
							bStreamBufAvailable = false;
						}
					}
					if (bLast)
						break;
					// process download throttle
					// see if we've used up all of our quota for this interval
					if (bUploadThrottle)
					{
						map<CString, THROTTLE_REC>::iterator itThrottle;
						{
							CSingleLock lock(&csThrottleMap, true);
							itThrottle = ThrottleMap.find(sHost);
							if ((itThrottle != ThrottleMap.end()) && (itThrottle->second.Upload.iBytesSec != 0))
							{
								// okay, we need to throttle
								itThrottle->second.Upload.iBytesCurInterval -= (int)State.CallbackContext.dwBytesWritten;
							}
						}
						for (;;)
						{
							if (TestAbort())
								throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_OPERATION_ABORTED);
							// now check if the bytes for this interval is positive
							{
								CSingleLock lock(&csThrottleMap, true);
								itThrottle = ThrottleMap.find(sHost);
								if ((itThrottle == ThrottleMap.end()) || (itThrottle->second.Upload.iBytesSec == 0))
								{
									// the throttle disappeared! The user must have turned throttle off
									break;					// just go full speed
								}
								// okay, we need to throttle
								if (itThrottle->second.Upload.iBytesCurInterval >= 0)
									break;
							}
							// wait for the next time slot where hopefully we can receive more bytes
							dwError = WaitForSingleObject(State.evThrottle.m_hObject, SECONDS(2));
							switch (dwError)
							{
							case WAIT_FAILED:
								throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
							case WAIT_ABANDONED:
								throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
							default:
								ASSERT(false);
								break;
							case WAIT_TIMEOUT:
							case WAIT_OBJECT_0:
								break;
							}
						}
					}
				}
			}
			// wait for response
			PrepareCmd();
			if (!WinHttpReceiveResponse(State.hRequest, NULL))
			{
				dwError = GetLastError();
				if (IfServerReached(dwError) && (pbGotServerResponse != NULL))
					*pbGotServerResponse = true;
				CleanupCmd();
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
			}
			if (!WaitComplete(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
			{
				if (IfServerReached(State.CallbackContext.Result.dwError) && (pbGotServerResponse != NULL))
					*pbGotServerResponse = true;
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, State.CallbackContext.Result.dwError);
			}
			if (pbGotServerResponse != NULL)
				*pbGotServerResponse = true;
			dwIndex = 0;
			if (!WinHttpQueryHeadersBuffer(State.hRequest, WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, RetBuf, &dwIndex))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
			if (RetBuf.IsEmpty())
				Error.dwHttpError = 0;
			else
				Error.dwHttpError = _wtoi((LPCWSTR)RetBuf.GetData());
			// if proxy authentication error, figure out what to do here
			if ((Error.dwHttpError != HTTP_STATUS_DENIED) && (Error.dwHttpError != HTTP_STATUS_PROXY_AUTH_REQ))
				break;
			{
				bAuthFailure = true;
				DWORD dwSupportedSchemes;
				DWORD dwFirstScheme;
				DWORD dwTarget;
				// Obtain the supported and preferred schemes.
				if (!WinHttpQueryAuthSchemes(State.hRequest, &dwSupportedSchemes, &dwFirstScheme, &dwTarget))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
				DEBUGF(_T("WinHttpQueryAuthSchemes: Supported: %d, First: %d, Target: %d"),
					dwSupportedSchemes, dwFirstScheme, dwTarget);
				// Set the credentials before re-sending the request.
				if (Error.dwHttpError == HTTP_STATUS_PROXY_AUTH_REQ)
					State.dwProxyAuthScheme = ChooseAuthScheme(dwSupportedSchemes);
				else
					State.dwAuthScheme = ChooseAuthScheme(dwSupportedSchemes);
				DEBUGF(_T("ChooseAuthScheme: %d"), State.dwProxyAuthScheme);
			}
		}
		// if it required authorization and was successful
		// and we have secure logs disabled meaning this is a test connection
		// log an INFO message detailing the proxy connection
		if (bAuthFailure
			&& !Error.IfError()
			&& State.bDisableSecureLog
			&& (State.dwProxyAuthScheme != 0))
		{
			CString sProxyString(sProxy);
			if (dwProxyPort != 0)
				sProxyString += _T(":") + FmtNum(dwProxyPort);
			if (bSSL || (State.dwProxyAuthScheme != WINHTTP_AUTH_SCHEME_BASIC))
				LogMessage(_T(__FILE__), __LINE__, _T("Connection to %1 successfully authenticated through proxy server.\r\nProxy: %2\r\nAuthentication: %3\r\n"), ERROR_SUCCESS, (LPCTSTR)sHost, (LPCTSTR)sProxyString, (LPCTSTR)FormatAuthScheme());
			else
				LogMessage(_T(__FILE__), __LINE__, _T("Connection to %1 successfully authenticated through proxy server - NOT SECURE.\r\nWarning: This server is requesting your username and password to be sent in an insecure manner (basic authentication without a secure connection).\r\nProxy: %2\r\nAuthentication: %3\r\n"), ERROR_SUCCESS, (LPCTSTR)sHost, (LPCTSTR)sProxyString, (LPCTSTR)FormatAuthScheme());
		}
		if (pHeaderReq != NULL)
		{
			if (pHeaderReq->empty())
			{
				dwIndex = 0;
				if (!WinHttpQueryHeadersBuffer(State.hRequest, WINHTTP_QUERY_RAW_HEADERS, WINHTTP_HEADER_NAME_BY_INDEX, RetBuf, &dwIndex))
				{
					dwError = GetLastError();
					LogMessage(_T(__FILE__), __LINE__, _T("WinHttpQueryHeadersBuffer Error"), dwError);
				}
				else
				{
					list<CString> AllHeaders;
#ifdef _UNICODE
					LoadNullTermStringArray((LPCWSTR)RetBuf.GetData(), AllHeaders);
#else
					CAnsiString HeadersStr;
					HeadersStr.Set((LPCWSTR)RetBuf.GetData(), RetBuf.GetBufSize()/sizeof(WCHAR));
					LoadNullTermStringArray((LPCSTR)HeadersStr.GetData(), AllHeaders);
#endif
					CString sLabel, sContent;
					for (list<CString>::const_iterator it = AllHeaders.begin(); it != AllHeaders.end(); ++it)
					{
						int iSep = it->Find(L':');
						if (iSep >= 0)
						{
							sLabel = it->Left(iSep);
							sLabel.TrimRight();
							sLabel.TrimLeft();
							sContent = it->Mid(iSep + 1);
							sContent.TrimRight();
							sContent.TrimLeft();
							// see if we've already seen this label
							list<HEADER_REQ>::iterator itReq;
							for (itReq = pHeaderReq->begin(); itReq != pHeaderReq->end(); ++itReq)
								if (itReq->sLabel.CompareNoCase(sLabel) == 0)
									break;
							if (itReq == pHeaderReq->end())
							{
								HEADER_REQ Rec;
								Rec.sLabel = sLabel;
								Rec.ContentList.push_back(sContent);
								pHeaderReq->push_back(Rec);
							}
							else
								itReq->ContentList.push_back(sContent);
						}
					}
				}
			}
			else
			{
				for (list<HEADER_REQ>::iterator itReq = pHeaderReq->begin(); itReq != pHeaderReq->end(); ++itReq)
				{
					dwIndex = 0;
					for (;;)
					{
						if (!WinHttpQueryHeadersBuffer(State.hRequest, WINHTTP_QUERY_CUSTOM, itReq->sLabel, RetBuf, &dwIndex))
						{
							dwError = GetLastError();
							if (dwError == ERROR_WINHTTP_HEADER_NOT_FOUND)
								break;
							LogMessage(_T(__FILE__), __LINE__, _T("WinHttpQueryHeadersBuffer error: %1"), dwError, (LPCTSTR)(itReq->sLabel + _T(" #") + FmtNum(dwIndex)));
							break;
						}
						itReq->ContentList.push_back(FROM_UNICODE((LPCWSTR)RetBuf.GetData()));
					}
				}
			}
		}
		if (dwReceivedDataHint == 0)
			dwReceivedDataHint = GDReadWriteChunkMax;
		RetData.SetBufSize(dwReceivedDataHint + dwBufOffset + 1024);			// pre-allocate this buffer
		RetData.SetBufSize(0);
		DWORD dwLen = dwBufOffset;
		DWORD dwDownloaded = 0;
		DWORD dwSize = 0;
		STREAM_DATA_ENTRY RcvBuf;
		for (;;)
		{
			// Check for available data.
			PrepareCmd();
			if (!WinHttpQueryDataAvailable(State.hRequest, NULL))
			{
				Error.dwError = GetLastError();
				CleanupCmd();
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			}
			if (!WaitComplete(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, State.CallbackContext.Result.dwError);
			dwSize = State.CallbackContext.dwReadLength;
			if (dwSize == 0)
			{
				if (pStreamReceive != NULL)
				{
					if (Error.dwHttpError < 400)
					{
						RcvBuf.Data.Empty();
						RcvBuf.ullOffset = dwLen;
						RcvBuf.bLast = true;
						pStreamReceive->StreamData.push_back(RcvBuf,
							MaxStreamQueueSize,
							TestAbortStatic,
							this);
					}
				}
				break;
			}
			bool bReadReturn;
			PrepareCmd();
			if (pStreamReceive == NULL)
			{
				// Allocate space for the buffer.
				// if the buffer must be enlarged (hopefully should never occur if Hint is accurate)
				// enlarge it by GDReadWriteChunkMax (a big number)
				while (RetData.GetAllocSize() < (dwLen + dwSize))
					RetData.SetBufSize(RetData.GetBufSize() + GDReadWriteChunkMax);
				RetData.SetBufSize(dwLen + dwSize);
				bReadReturn = WinHttpReadData(State.hRequest, RetData.GetData() + dwLen, dwSize, NULL) != FALSE;
			}
			else
			{
				RcvBuf.Data.SetBufSize(dwSize);
				RcvBuf.bLast = false;
				bReadReturn = WinHttpReadData(State.hRequest, RcvBuf.Data.GetData(), dwSize, NULL) != FALSE;
			}
			if (!bReadReturn)
			{
				Error.dwError = GetLastError();
				CleanupCmd();
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			}
			if (!WaitComplete(WINHTTP_CALLBACK_STATUS_READ_COMPLETE))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, State.CallbackContext.Result.dwError);
			dwDownloaded = State.CallbackContext.dwReadLength;
			if (pStreamReceive != NULL)
			{
				if (Error.dwHttpError < 400)
				{
					RcvBuf.Data.SetBufSize(dwDownloaded);
					RcvBuf.ullOffset = dwLen;
					pStreamReceive->ullTotalSize += (ULONGLONG)dwDownloaded;
					State.ullReadBytes += (ULONGLONG)dwDownloaded;
					pStreamReceive->StreamData.push_back(RcvBuf,
						MaxStreamQueueSize,
						TestAbortStatic,
						this);
				}
				else
					RetData = RcvBuf.Data;
			}
			else
			{
				RetData.SetBufSize(dwLen + dwDownloaded);
			}
			dwLen += dwDownloaded;
			// process download throttle
			// see if we've used up all of our quota for this interval
			if (bDownloadThrottle)
			{
				map<CString,THROTTLE_REC>::iterator itThrottle;
				{
					CSingleLock lock(&csThrottleMap, true);
					itThrottle = ThrottleMap.find(sHost);
					if ((itThrottle != ThrottleMap.end()) && (itThrottle->second.Download.iBytesSec != 0))
					{
						itThrottle->second.Download.iBytesCurInterval -= (int)dwDownloaded;
					}
				}
				for (;;)
				{
					if (TestAbort())
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_OPERATION_ABORTED);
					// now check if the bytes for this interval has become positive
					{
						CSingleLock lock(&csThrottleMap, true);
						itThrottle = ThrottleMap.find(sHost);
						if ((itThrottle == ThrottleMap.end()) || (itThrottle->second.Download.iBytesSec == 0))
						{
							// throttle has been canceled during this download
							// go full speed
							break;
						}
						if (itThrottle->second.Download.iBytesCurInterval >= 0)
							break;
					}
					// wait for the next time slot where hopefully we can receive more bytes
					dwError = WaitForSingleObject(State.evThrottle.m_hObject, SECONDS(2));
					switch (dwError)
					{
					case WAIT_FAILED:
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, GetLastError());
					case WAIT_ABANDONED:
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, dwError);
					default:
						ASSERT(false);
						break;
					case WAIT_TIMEOUT:
					case WAIT_OBJECT_0:
						break;
					}
				}
			}
		}
		// if error, get error detail from XML response
		if ((Error.dwHttpError >= 400) && !RetData.IsEmpty())
		{
			XML_ECS_ERROR_CONTEXT Context;
			Context.pAtError = &Error;
			if (!State.bS3Admin)
			{
				(void)ScanXml(&RetData, &Context, XmlS3ErrorCB);
			}
			else if (State.bS3Admin)
			{
				(void)ScanXml(&RetData, &Context, XmlECSAdminErrorCB);
			}
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		// if the progress has been incremented, cancel it out
		if ((pConstStreamSend != NULL) && (pConstStreamSend->UpdateProgressCB != NULL) && (pConstStreamSend->iAccProgress != 0))
		{
			pConstStreamSend->UpdateProgressCB(-pConstStreamSend->iAccProgress, pConstStreamSend->pContext);
			pStreamSend->iAccProgress = 0;
		}
		State.CloseRequest((E.Error.dwError == ERROR_WINHTTP_SECURE_FAILURE) && TST_BIT(State.dwSecureError, WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA));
		State.Session.pValue->bKillWhenDone = true;
		State.Session.ReleaseSession();
		return E;
	}
	State.CloseRequest();
	State.Session.ReleaseSession();
	return Error;
}

void CECSConnection::SetSecret(LPCTSTR pszSecret)		// set shared secret string in base64
{
	if (sSecret != pszSecret)
	{
		sSecret = pszSecret;
		KillHostSessions();
	}
}

void CECSConnection::SetUser(LPCTSTR pszUser)			// set user ID
{
	if (sUser != pszUser)
	{
		sUser = pszUser;
		KillHostSessions();
	}
}

void CECSConnection::SetHost(LPCTSTR pszHost)				// set Host
{
	if (sHost != pszHost)
	{
		KillHostSessions();
		sHost = pszHost;
		KillHostSessions();
	}
}

void CECSConnection::SetIPList(const deque<CString>& IPListParam)
{
	CECSConnectionState& State(GetStateBuf());
	CSimpleRWLockAcquire lock(&rwlIPListHost, true);			// write lock because we might change it
	if (IPListHost != IPListParam)
	{
		IPListHost = IPListParam;
		State.iIPList = 0;
		lock.Unlock();
		KillHostSessions();
	}
}

void CECSConnection::GetIPList(deque<CString>& IPListParam)
{
	CSimpleRWLockAcquire lock(&rwlIPListHost, false);			// read lock
	IPListParam = IPListHost;
}

void CECSConnection::SetSSL(bool bSSLParam)
{
	INTERNET_PORT PortParam;
	if (bSSLParam)
		PortParam = INTERNET_DEFAULT_HTTPS_PORT;
	else
		PortParam = INTERNET_DEFAULT_HTTP_PORT;
	if ((bSSLParam != bSSL) || (Port != PortParam))
	{
		bSSL = bSSLParam;
		Port = PortParam;
		KillHostSessions();
	}
}

void CECSConnection::SetRegion(LPCTSTR pszS3Region)
{
	sS3Region = pszS3Region;
}

void CECSConnection::SetS3KeyID(LPCTSTR pszS3KeyID)
{
	sS3KeyID = pszS3KeyID;
}

CString CECSConnection::GetS3KeyID()
{
	return sS3KeyID;
}

void CECSConnection::SetUserAgent(LPCTSTR pszUserAgent)
{
	sUserAgent = pszUserAgent;
}

void CECSConnection::SetPort(INTERNET_PORT PortParam)
{
	if (Port != PortParam)
	{
		Port = PortParam;
		KillHostSessions();
	}
}

void CECSConnection::SetProxy(bool bUseDefaultProxyParam, LPCTSTR pszProxy, DWORD dwPort, LPCTSTR pszProxyUser, LPCTSTR pszProxyPassword)
{
	if ((sProxy != pszProxy)
		|| (dwProxyPort != dwPort)
		|| (sProxyUser != pszProxyUser)
		|| (sProxyPassword != pszProxyPassword)
		|| (bUseDefaultProxyParam != bUseDefaultProxy))
	{
		bUseDefaultProxy = bUseDefaultProxyParam;
		sProxy = pszProxy;
		dwProxyPort = dwPort;
		sProxyUser = pszProxyUser;
		sProxyPassword = pszProxyPassword;
		KillHostSessions();
	}
}

void CECSConnection::SetTest(bool bTestParam)
{
	bTestConnection = bTestParam;
}

CECSConnection::S3_ERROR CECSConnection::Create(
	LPCTSTR pszPath,
	const void *pData,
	DWORD dwLen,
	const list<S3_METADATA_ENTRY> *pMDList,
	const CBuffer *pChecksum,
	STREAM_CONTEXT *pStreamSend,
	ULONGLONG ullTotalLen,								// only used if stream send
	LPCTSTR pIfNoneMatch)								// creates if-none-match header. use "*" to prevent an object from overwriting an existing object
{
	S3_ERROR Error;
	try
	{
		CBuffer RetData;

		InitHeader();
		if (pChecksum != NULL && !pChecksum->IsEmpty())
			AddHeader(_T("Content-MD5"), pChecksum->EncodeBase64());
		if (pMDList != nullptr)
		{
			for (list<S3_METADATA_ENTRY>::const_iterator itList = pMDList->begin(); itList != pMDList->end(); ++itList)
			{
				if (!itList->bS3Header)
					AddHeader(sAmzMetaPrefix + itList->sTag, itList->sData);
				else
					AddHeader(itList->sTag, itList->sData);
			}
		}
		if (pIfNoneMatch != nullptr)
			AddHeader(_T("if-none-match"), pIfNoneMatch);
		Error = SendRequest(_T("PUT"), (LPCTSTR)UriEncode(pszPath), pData, dwLen, RetData, NULL, 0, 0, pStreamSend, NULL, ullTotalLen);
		if (Error.IfError())
			return Error;
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::RenameS3(
	LPCTSTR pszOldPath,
	LPCTSTR pszNewPath,
	LPCTSTR pszVersionId,		// nonNULL: version ID to copy
	bool bCopy,
	const list<CECSConnection::S3_METADATA_ENTRY> *pMDList,
	const list<CString> *pDeleteTagParam)
{
	CECSConnection::S3_ERROR Error;
	CBuffer RetData;
	CString sOldPath(pszOldPath), sNewPath(pszNewPath);
	CString sOldPathS3, sNewPathS3;								// translated paths to S3
	// first figure out if this an object or a folder
	// if it ends in '\' it's a folder
	if (sOldPath.IsEmpty() || sNewPath.IsEmpty())
		return ERROR_INVALID_NAME;

	if (sOldPath[sOldPath.GetLength() - 1] != L'\\')
	{
		deque<ACL_ENTRY> Acls;
		// first get the ACL of the "before" object
		S3_ERROR AclAtError = ReadACL(sOldPath, Acls, pszVersionId);

		// object - copy to new path and then delete the old object
		InitHeader();
		bool bCopyMD = true;
		list<CECSConnection::S3_METADATA_ENTRY> MDList;
		if (pMDList != nullptr)
			MDList = *pMDList;
		// we are ALWAYS going to read the current headers and never do a CopyMD
		// this is because we don't know at this point if we need to do a multipart copy
		// and multipart copy does not seem to support copying of the MD
		// it is really complicated to do this again later on, so we'll just always do it here
		{
			InitHeader();
			// read all the headers of the source file
			list<HEADER_REQ> Req;
			CString sHeadPath(sOldPathS3);
			if (pszVersionId != nullptr)
				sHeadPath += CString(_T("?versionId=")) + pszVersionId;
			Error = SendRequest(_T("HEAD"), UriEncode(sHeadPath), NULL, 0, RetData, &Req);
			if (Error.IfError())
				return Error;
			for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
			{
				if ((it->sLabel.Find(sAmzMetaPrefix) == 0) && !it->ContentList.empty())
				{
					CString sTag(it->sLabel.Mid(sAmzMetaPrefix.GetLength()));				// strip off x-amz-meta-
																							// if this tag is already in the MDList, don't transfer it over from the old file
					bool bFound = false;
					for (list<S3_METADATA_ENTRY>::const_iterator itMD = MDList.begin(); itMD != MDList.end(); ++itMD)
					{
						if (sTag == itMD->sTag)
						{
							bFound = true;
							break;
						}
					}
					if (!bFound)
					{
						Error = WriteMetadataEntry(MDList, sTag, it->ContentList.front());
						if (Error.IfError())
							return Error;
					}
				}
			}
			if (!MDList.empty() && pDeleteTagParam != nullptr && !pDeleteTagParam->empty())
			{
				for (list<S3_METADATA_ENTRY>::const_iterator itList = MDList.begin(); itList != MDList.end(); )
				{
					bool bDeleted = false;
					for (list<CString>::const_iterator itDel = pDeleteTagParam->begin(); itDel != pDeleteTagParam->end(); ++itDel)
					{
						if (*itDel == itList->sTag)
						{
							bDeleted = true;
							break;
						}
					}
					if (bDeleted)
						itList = MDList.erase(itList);
					else
						++itList;
				}
			}
			bCopyMD = false;
		}
		Error = CopyS3(sOldPathS3, sNewPathS3, pszVersionId, bCopyMD, nullptr, &MDList);
		if (Error.IfError())
			return Error;
		if (!AclAtError.IfError())
			(void)WriteACL(sNewPath, Acls);		// if this fails, don't fail the rename since it is mostly done

		if (!bCopy)
		{
			InitHeader();
			Error = SendRequest(_T("DELETE"), UriEncode(sOldPathS3), NULL, 0, RetData);
		}
	}
	else
	{
		// rename folder
		return ERROR_INVALID_FUNCTION;
	}
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::DeleteS3(LPCTSTR pszPath)
{
	list<CECSConnection::S3_DELETE_ENTRY> PathList;
	PathList.push_back(CECSConnection::S3_DELETE_ENTRY(pszPath));
	return DeleteS3(PathList);
}

CECSConnection::S3_ERROR CECSConnection::DeleteS3(const list<CECSConnection::S3_DELETE_ENTRY>& PathList)
{
	CECSConnectionState& State(GetStateBuf());
	S3_ERROR Error;
	try
	{
		State.S3DeletePathList.clear();
		DeleteS3Internal(PathList);
		DeleteS3Send();								// delete any straggelers
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

void CECSConnection::DeleteS3Internal(const list<CECSConnection::S3_DELETE_ENTRY>& PathList)
{
	CECSConnectionState& State(GetStateBuf());
	S3_ERROR Error;

	list<CECSConnection::S3_DELETE_ENTRY>::const_iterator itPath;
	for (itPath = PathList.begin(); itPath != PathList.end(); ++itPath)
	{
		if (itPath->sKey[itPath->sKey.GetLength() - 1] == L'\\')
		{
			list<CECSConnection::S3_DELETE_ENTRY> PathListAdd;
			DirEntryList_t DirList;
			Error = DirListing(itPath->sKey, DirList, false);
			if (Error.IfError() && !Error.IfNotFound())
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			if (!Error.IfError())
			{
				for (DirEntryList_t::const_iterator itDir = DirList.begin(); itDir != DirList.end(); ++itDir)
				{
					PathListAdd.push_back(S3_DELETE_ENTRY(itPath->sKey + itDir->sName + (itDir->bDir ? _T("\\") : _T(""))));
				}
				DeleteS3Internal(PathListAdd);
			}
		}
		State.S3DeletePathList.push_back(S3_DELETE_ENTRY(itPath->sKey, itPath->sVersionId));
		if (State.S3DeletePathList.size() > MaxS3DeleteObjects)
			DeleteS3Send();
	}
}

const WCHAR * const XML_DELETES3_ERROR = L"//DeleteResult/Error";
const WCHAR * const XML_DELETES3_ERROR_CODE = L"//DeleteResult/Error/Code";
const WCHAR * const XML_DELETES3_ERROR_MESSAGE = L"//DeleteResult/Error/Message";
const WCHAR * const XML_DELETES3_ERROR_REQUESTID = L"//DeleteResult/Error/RequestId";
const WCHAR * const XML_DELETES3_ERROR_HOSTID = L"//DeleteResult/Error/HostId";
const WCHAR * const XML_DELETES3_ERROR_KEY = L"//DeleteResult/Error/Key";

struct XML_DELETES3_ENTRY
{
	CString sKey;
	E_S3_ERROR_TYPE Error;
	CString sCode;
	CString sMessage;
	CString sRequestId;
	CString sHostId;

	void Clear(void)
	{
		sKey.Empty();
		Error = S3_ERROR_UNKNOWN;
		sCode.Empty();
		sMessage.Empty();
		sRequestId.Empty();
		sHostId.Empty();
	}
};

struct XML_DELETES3_CONTEXT
{
	list<XML_DELETES3_ENTRY> ErrorList;
	XML_DELETES3_ENTRY Rec;
};

HRESULT XmlDeleteS3CB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_DELETES3_CONTEXT *pInfo = (XML_DELETES3_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (psValue != NULL)
		{
			if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR_CODE) == 0)
			{
				pInfo->Rec.sCode = FROM_UNICODE(*psValue);
				pInfo->Rec.Error = S3TranslateError(FROM_UNICODE(*psValue));
			}
			else if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR_MESSAGE) == 0)
			{
				pInfo->Rec.sMessage = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR_REQUESTID) == 0)
			{
				pInfo->Rec.sRequestId = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR_HOSTID) == 0)
			{
				pInfo->Rec.sHostId = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR_KEY) == 0)
			{
				pInfo->Rec.sKey = FROM_UNICODE(*psValue);
			}
		}
		break;
	case XmlNodeType_Element:
		if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR) == 0)
		{
			pInfo->Rec.Clear();
		}
		break;
	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_DELETES3_ERROR) == 0)
		{
			pInfo->ErrorList.push_back(pInfo->Rec);
			pInfo->Rec.Clear();
		}
		break;

	default:
		break;
	}
	return 0;
}

void CECSConnection::DeleteS3Send()
{
	CECSConnectionState& State(GetStateBuf());
	if (State.S3DeletePathList.empty())
		return;
	CBufferStream *pBufStream = new CBufferStream;
	CComPtr<IStream> pOutFileStream = pBufStream;
	CComPtr<IXmlWriter> pWriter;
	S3_ERROR Error;
	CString sBucket, sKey;

	if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->SetOutput(pOutFileStream)))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->WriteStartElement(NULL, L"Delete", NULL)))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

	if (FAILED(pWriter->WriteStartElement(NULL, L"Quiet", NULL)))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->WriteString(L"true")))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->WriteFullEndElement()))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

	// now put out the list of objects to delete
	State.S3DeletePathList.sort();
	State.S3DeletePathList.unique();
	UINT iCount = 0;								// keep a count. don't let it go over MaxS3DeleteObjects
	list<S3_DELETE_ENTRY>::iterator itPath;
	for (itPath = State.S3DeletePathList.end(); itPath != State.S3DeletePathList.begin();)
	{
		--itPath;															// run through the list from the end to the beginning
		// extract the bucket
		if (sBucket.IsEmpty())
		{
			int iSlash = itPath->sKey.Find(L'/', 1);
			if (iSlash < 0)
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_INVALID_DATA);
			sBucket = itPath->sKey.Mid(1, iSlash - 1);
		}
		// verify we are always in the same bucket
		if (itPath->sKey.Find(L'/' + sBucket + L'/') != 0)
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_INVALID_DATA);
		sKey = itPath->sKey.Mid(sBucket.GetLength() + 2);
		if (FAILED(pWriter->WriteStartElement(NULL, L"Object", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"Key", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(sKey))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (!itPath->sVersionId.IsEmpty())
		{
			if (FAILED(pWriter->WriteStartElement(NULL, L"VersionId", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(itPath->sVersionId))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		}
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		itPath = State.S3DeletePathList.erase(itPath);
		iCount++;
		if (iCount >= MaxS3DeleteObjects)
			break;
	}
	if (FAILED(pWriter->WriteFullEndElement()))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->WriteEndDocument()))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	if (FAILED(pWriter->Flush()))
		throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
	CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
	CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
	CAnsiString XmlUTF8(sXmlOut);
#endif
	XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
	{
		CBuffer RetData;
		InitHeader();
		CCngAES_GCM HashObj;
		HashObj.CreateHash(BCRYPT_MD5_ALGORITHM);
		HashObj.AddHashData(XmlUTF8);
		CBuffer MD5Hash;
		HashObj.GetHashData(MD5Hash);
		AddHeader(_T("Content-MD5"), MD5Hash.EncodeBase64());
		AddHeader(_T("Content-Type"), _T("application/xml"));
		Error = SendRequest(_T("POST"), _T("/") + sBucket + _T("/?delete"), XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);

		// now interpret the returned XML
		XML_DELETES3_CONTEXT Context;
		HRESULT hr = ScanXml(&RetData, &Context, XmlDeleteS3CB);
		if (FAILED(hr))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, hr);
		if (!Context.ErrorList.empty())
		{
			Error.sDetails.Empty();
			for (list<XML_DELETES3_ENTRY>::const_iterator itList = Context.ErrorList.begin(); itList != Context.ErrorList.end(); ++itList)
			{
				Error.sDetails += itList->sCode + _T(":") + itList->sMessage + _T(": ") + itList->sKey + _T("\n");
			}
			Error.dwHttpError = 500;
			Error.S3Error = Context.ErrorList.front().Error;
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
		}
	}
}

// CECSConnection::Read
// if lwOffset == 0 and dwLen == 0, read entire file
// if dwLen != 0, read 'dwLen' bytes starting from lwOffset
// if lwOffset != 0 and dwLen == 0, read from lwOffset to the end of the file
// dwBufOffset reserves that many bytes at the start of the buffer. The data is written starting at dwBufOffset
CECSConnection::S3_ERROR CECSConnection::Read(LPCTSTR pszPath, ULONGLONG lwLen, ULONGLONG lwOffset, CBuffer& RetData, DWORD dwBufOffset, STREAM_CONTEXT *pStreamReceive)
{
	const UINT READ_RETRY_MAX_TRIES = 5;
	CECSConnectionState& State(GetStateBuf());
	S3_ERROR Error;
	try
	{
		list<HEADER_REQ> HeaderReq;
		InitHeader();
		CString sRange;
		for (UINT iRetry = 0; iRetry < READ_RETRY_MAX_TRIES; iRetry++)
		{
			(void)State.Headers.erase(_T("range"));			// erase it in case of a retry
			HeaderReq.clear();
			HeaderReq.push_back(HEADER_REQ(_T("Content-Length")));
			HeaderReq.push_back(HEADER_REQ(_T("Content-Range")));
			if ((lwOffset != 0) || (lwLen != 0))
			{
				sRange = _T("bytes=") + FmtNum(lwOffset) + _T("-");
				if (lwLen != 0)
					sRange += FmtNum(lwOffset + lwLen - 1);
				AddHeader(_T("range"), sRange);
			}
			State.ullReadBytes = 0ULL;
			Error = SendRequest(_T("GET"), (LPCTSTR)UriEncode(pszPath), NULL, 0, RetData, &HeaderReq,
				((pStreamReceive == nullptr) && (lwLen != 0)) ? ((DWORD)lwLen + 1024) : 0, dwBufOffset, NULL, pStreamReceive, lwOffset);
			if (!Error.IfError())
			{
				LONGLONG llStartRange, llExpectedLength, llTotalSize;
				ULONGLONG ullTotalLength;
				bool bWrongSize = false;

				if (pStreamReceive == nullptr)
					ullTotalLength = (ULONGLONG)RetData.GetBufSize();
				else
					ullTotalLength = State.ullReadBytes;
				// make sure we have all the bytes we asked for
				for (list<HEADER_REQ>::const_iterator it = HeaderReq.begin(); it != HeaderReq.end(); ++it)
				{
					if (it->sLabel == _T("Content-Length"))
					{
						if (it->ContentList.size() == 1)				// there should only be 1
						{
							llExpectedLength = _ttoi64(it->ContentList.front());
							bWrongSize = ullTotalLength != (ULONGLONG)llExpectedLength;
							if (bWrongSize)
								break;
						}
					}
					else if (it->sLabel == _T("Content-Range"))
					{
						if (it->ContentList.size() == 1)				// there should only be 1
						{
							if (_stscanf_s(it->ContentList.front(), _T("bytes %I64d-%I64d/%I64d"), &llStartRange, &llExpectedLength, &llTotalSize) == 3)
							{
								bWrongSize = ullTotalLength != (ULONGLONG)(llExpectedLength + 1 - llStartRange);
								if (bWrongSize)
									break;
							}
						}
					}
				}
				if (bWrongSize)
				{
					if ((pStreamReceive == nullptr) && (iRetry < (READ_RETRY_MAX_TRIES - 1)))
					{
//						DumpDebugFileFmt(_T(__FILE__), __LINE__, _T("CECSConnection::S3_ERROR CECSConnection::Read: %s - RETRY:%d"),
//							(LPCTSTR)sPath, iRetry);
						continue;
					}
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_BAD_LENGTH);
				}
			}
			break;
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

// XML for Versions response
/*
<ListVersionsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01">
    <Name>bucket</Name>
    <Prefix>my</Prefix>
    <KeyMarker/>
    <VersionIdMarker/>
    <MaxKeys>5</MaxKeys>
    <IsTruncated>false</IsTruncated>
    <Version>
        <Key>my-image.jpg</Key>
        <VersionId>3/L4kqtJl40Nr8X8gdRQBpUMLUo</VersionId>
        <IsLatest>true</IsLatest>
         <LastModified>2009-10-12T17:50:30.000Z</LastModified>
        <ETag>&quot;fba9dede5f27731c9771645a39863328&quot;</ETag>
        <Size>434234</Size>
        <StorageClass>STANDARD</StorageClass>
        <Owner>
            <ID>75aa57f09aa0c8caeab4f8c24e99d10f8e7faeebf76c078efc7c6caea54ba06a</ID>
            <DisplayName>mtd@amazon.com</DisplayName>
        </Owner>
    </Version>
    <DeleteMarker>
        <Key>my-second-image.jpg</Key>
        <VersionId>03jpff543dhffds434rfdsFDN943fdsFkdmqnh892</VersionId>
        <IsLatest>true</IsLatest>
        <LastModified>2009-11-12T17:50:30.000Z</LastModified>
        <Owner>
            <ID>75aa57f09aa0c8caeab4f8c24e99d10f8e7faeebf76c078efc7c6caea54ba06a</ID>
            <DisplayName>mtd@amazon.com</DisplayName>
        </Owner>    
    </DeleteMarker>
*/
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_Prefix = L"//ListVersionsResult/Prefix";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_IsTruncated = L"//ListVersionsResult/IsTruncated";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_NextKeyMarker = L"//ListVersionsResult/NextKeyMarker";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_NextVersionIdMarker = L"//ListVersionsResult/NextVersionIdMarker";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_Key = L"//ListVersionsResult/Version/Key";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_VersionId = L"//ListVersionsResult/Version/VersionId";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_IsLatest = L"//ListVersionsResult/Version/IsLatest";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_LastModified = L"//ListVersionsResult/Version/LastModified";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_ETag = L"//ListVersionsResult/Version/ETag";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_Size = L"//ListVersionsResult/Version/Size";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_Owner_ID = L"//ListVersionsResult/Version/Owner/ID";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_Owner_DisplayName = L"//ListVersionsResult/Version/Owner/DisplayName";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_ELEMENT_Contents = L"//ListVersionsResult/Version";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_ELEMENT = L"//ListVersionsResult/DeleteMarker";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_Key = L"//ListVersionsResult/DeleteMarker/Key";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_VersionId = L"//ListVersionsResult/DeleteMarker/VersionId";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_IsLatest = L"//ListVersionsResult/DeleteMarker/IsLatest";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_LastModified = L"//ListVersionsResult/DeleteMarker/LastModified";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_Owner_ID = L"//ListVersionsResult/DeleteMarker/Owner/ID";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_DELETED_Owner_DisplayName = L"//ListVersionsResult/DeleteMarker/Owner/DisplayName";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_CommonPrefixes_Prefix = L"//ListVersionsResult/CommonPrefixes/Prefix";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_ELEMENT_CommonPrefixes = L"//ListVersionsResult/CommonPrefixes";
const WCHAR * const XML_S3_DIR_LISTING_VERSIONS_ROOT_ELEMENT = L"//ListVersionsResult";

HRESULT XmlDirListingS3VersionsCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	CECSConnection::XML_DIR_LISTING_CONTEXT *pInfo = (CECSConnection::XML_DIR_LISTING_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (psValue != NULL)
		{
			if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_Prefix) == 0)
			{
				pInfo->sPrefix = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_IsTruncated) == 0)
			{
				pInfo->bIsTruncated = FROM_UNICODE(*psValue) == _T("true");
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_NextKeyMarker) == 0)
			{
				pInfo->sS3NextKeyMarker = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_NextVersionIdMarker) == 0)
			{
				pInfo->sS3NextVersionIdMarker = FROM_UNICODE(*psValue);
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_Key) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_Key) == 0))
			{
				pInfo->bGotKey = true;
				pInfo->Rec.bDir = false;
				pInfo->Rec.sName = FROM_UNICODE(*psValue);
				ASSERT(pInfo->sPrefix.CompareNoCase(pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength())) == 0);
				if (pInfo->sPrefix.CompareNoCase(pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength())) == 0)
					(void)pInfo->Rec.sName.Delete(0, pInfo->sPrefix.GetLength());
				if (!pInfo->Rec.sName.IsEmpty())
				{
					if (pInfo->Rec.sName[pInfo->Rec.sName.GetLength() - 1] == L'/')
					{
						(void)pInfo->Rec.sName.Delete(pInfo->Rec.sName.GetLength() - 1);
						pInfo->Rec.bDir = true;
					}
				}
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_LastModified) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_LastModified) == 0))
			{
				CECSConnection::S3_ERROR Error = CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Rec.Properties.ftLastMod);
				if (Error.IfError())
					return Error.dwError;
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_ETag) == 0)
			{
				pInfo->Rec.Properties.sETag = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_Size) == 0)
			{
				_stscanf_s(FROM_UNICODE(*psValue), _T("%I64u"), &pInfo->Rec.Properties.llSize);
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_Owner_ID) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_Owner_ID) == 0))
			{
				pInfo->Rec.Properties.sOwnerID = FROM_UNICODE(*psValue);
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_Owner_DisplayName) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_Owner_DisplayName) == 0))
			{
				pInfo->Rec.Properties.sOwnerDisplayName = FROM_UNICODE(*psValue);
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_VersionId) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_VersionId) == 0))
			{
				pInfo->Rec.Properties.sVersionId = FROM_UNICODE(*psValue);
				if (pInfo->Rec.Properties.sVersionId == _T("null"))
					pInfo->Rec.Properties.sVersionId.Empty();
			}
			else if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_IsLatest) == 0)
				|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_IsLatest) == 0))
			{
				pInfo->Rec.Properties.bIsLatest = FROM_UNICODE(*psValue) == _T("true");
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_CommonPrefixes_Prefix) == 0)
			{
				pInfo->Rec.sName = FROM_UNICODE(*psValue);
				if (pInfo->sPrefix == pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength()))
				{
					(void)pInfo->Rec.sName.Delete(0, pInfo->sPrefix.GetLength());
					if (!pInfo->Rec.sName.IsEmpty() && (pInfo->Rec.sName[pInfo->Rec.sName.GetLength() - 1] == L'/'))
						(void)pInfo->Rec.sName.Delete(pInfo->Rec.sName.GetLength() - 1);
				}
				pInfo->Rec.bDir = true;
			}
		}
		break;

	case XmlNodeType_EndElement:
		if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_ELEMENT_Contents) == 0)
			|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_ELEMENT) == 0)
			|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_ELEMENT_CommonPrefixes) == 0))
		{
//			if (!pInfo->Rec.sName.IsEmpty())
			{
				bool bDup = false;
				// first search the list to make sure this entry isn't duplicated (only for folders)
				if (pInfo->Rec.bDir)
				{
					for (CECSConnection::DirEntryList_t::const_iterator itList = pInfo->pDirList->begin(); itList != pInfo->pDirList->end(); ++itList)
						if (itList->bDir && itList->sName.CompareNoCase(pInfo->Rec.sName) == 0)
						{
							bDup = true;
							break;
						}
				}
				if (!bDup)
				{
					if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_DELETED_ELEMENT) == 0)
						pInfo->Rec.Properties.bDeleted = true;
					pInfo->pDirList->push_back(pInfo->Rec);
					if (pInfo->pszSearchName != NULL)
					{
						ASSERT(pInfo->psRetSearchName != NULL);
						if (pInfo->Rec.sName.CompareNoCase(pInfo->pszSearchName) == 0)
							*pInfo->psRetSearchName = pInfo->Rec.sName;
					}
				}
				pInfo->EmptyRec();
			}
		}
		break;

	case XmlNodeType_Element:
		if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_VERSIONS_ROOT_ELEMENT) == 0)
		{
			pInfo->bGotRootElement = true;
		}
		break;

	default:
		break;
	}
	return 0;
}

/*
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>example-bucket</Name>
  <Prefix></Prefix>
  <Marker></Marker>
  <MaxKeys>1000</MaxKeys>
  <Delimiter>/</Delimiter>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>sample.html</Key>
    <LastModified>2011-02-26T01:56:20.000Z</LastModified>
    <ETag>&quot;bf1d737a4d46a19f3bced6905cc8b902&quot;</ETag>
    <Size>142863</Size>
    <Owner>
      <ID>canonical-user-id</ID>
      <DisplayName>display-name</DisplayName>
    </Owner>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <CommonPrefixes>
    <Prefix>photos/</Prefix>
  </CommonPrefixes>
</ListBucketResult>
*/

const WCHAR * const XML_S3_DIR_LISTING_Prefix = L"//ListBucketResult/Prefix";
const WCHAR * const XML_S3_DIR_LISTING_IsTruncated = L"//ListBucketResult/IsTruncated";
const WCHAR * const XML_S3_DIR_LISTING_NextMarker = L"//ListBucketResult/NextMarker";
const WCHAR * const XML_S3_DIR_LISTING_Key = L"//ListBucketResult/Contents/Key";
const WCHAR * const XML_S3_DIR_LISTING_LastModified = L"//ListBucketResult/Contents/LastModified";
const WCHAR * const XML_S3_DIR_LISTING_ETag = L"//ListBucketResult/Contents/ETag";
const WCHAR * const XML_S3_DIR_LISTING_Size = L"//ListBucketResult/Contents/Size";
const WCHAR * const XML_S3_DIR_LISTING_Owner_ID = L"//ListBucketResult/Contents/Owner/ID";
const WCHAR * const XML_S3_DIR_LISTING_Owner_DisplayName = L"//ListBucketResult/Contents/Owner/DisplayName";
const WCHAR * const XML_S3_DIR_LISTING_ELEMENT_Contents = L"//ListBucketResult/Contents";
const WCHAR * const XML_S3_DIR_LISTING_CommonPrefixes_Prefix = L"//ListBucketResult/CommonPrefixes/Prefix";
const WCHAR * const XML_S3_DIR_LISTING_ELEMENT_CommonPrefixes = L"//ListBucketResult/CommonPrefixes";
const WCHAR * const XML_S3_DIR_LISTING_ROOT_ELEMENT = L"//ListBucketResult";

HRESULT XmlDirListingS3CB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	CECSConnection::XML_DIR_LISTING_CONTEXT *pInfo = (CECSConnection::XML_DIR_LISTING_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (psValue != NULL)
		{
			if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_Prefix) == 0)
			{
				pInfo->sPrefix = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_IsTruncated) == 0)
			{
				pInfo->bIsTruncated = FROM_UNICODE(*psValue) == _T("true");
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_NextMarker) == 0)
			{
				pInfo->sS3NextMarker = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_Key) == 0)
			{
				pInfo->bGotKey = true;
				pInfo->Rec.bDir = false;
				pInfo->Rec.sName = FROM_UNICODE(*psValue);
				ASSERT(pInfo->sPrefix.CompareNoCase(pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength())) == 0);
				if (pInfo->sPrefix.CompareNoCase(pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength())) == 0)
					(void)pInfo->Rec.sName.Delete(0, pInfo->sPrefix.GetLength());
				if (!pInfo->Rec.sName.IsEmpty())
				{
					if (pInfo->Rec.sName[pInfo->Rec.sName.GetLength() - 1] == L'/')
					{
						(void)pInfo->Rec.sName.Delete(pInfo->Rec.sName.GetLength() - 1);
						pInfo->Rec.bDir = true;
					}
				}
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_LastModified) == 0)
			{
				CECSConnection::S3_ERROR Error = CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Rec.Properties.ftLastMod);
				if (Error.IfError())
					return Error.dwError;
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_ETag) == 0)
			{
				pInfo->Rec.Properties.sETag = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_Size) == 0)
			{
				_stscanf_s(FROM_UNICODE(*psValue), _T("%I64u"), &pInfo->Rec.Properties.llSize);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_Owner_ID) == 0)
			{
				pInfo->Rec.Properties.sOwnerID = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_Owner_DisplayName) == 0)
			{
				pInfo->Rec.Properties.sOwnerDisplayName = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_CommonPrefixes_Prefix) == 0)
			{
				pInfo->Rec.sName = FROM_UNICODE(*psValue);
				if (pInfo->sPrefix == pInfo->Rec.sName.Left(pInfo->sPrefix.GetLength()))
				{
					(void)pInfo->Rec.sName.Delete(0, pInfo->sPrefix.GetLength());
					if (!pInfo->Rec.sName.IsEmpty() && (pInfo->Rec.sName[pInfo->Rec.sName.GetLength() - 1] == L'/'))
						(void)pInfo->Rec.sName.Delete(pInfo->Rec.sName.GetLength() - 1);
				}
				pInfo->Rec.bDir = true;
			}
		}
		break;

	case XmlNodeType_EndElement:
		if ((sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_ELEMENT_Contents) == 0)
			|| (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_ELEMENT_CommonPrefixes) == 0))
		{
			if (!pInfo->Rec.sName.IsEmpty())
			{
				bool bDup = false;
				// first search the list to make sure this entry isn't duplicated (only for folders)
				if (pInfo->Rec.bDir)
				{
					for (CECSConnection::DirEntryList_t::const_iterator itList = pInfo->pDirList->begin(); itList != pInfo->pDirList->end(); ++itList)
						if (itList->bDir && (itList->sName == pInfo->Rec.sName))
						{
							bDup = true;
							break;
						}
				}
				if (!bDup)
				{
					pInfo->pDirList->push_back(pInfo->Rec);
					if (pInfo->pszSearchName != NULL)
					{
						ASSERT(pInfo->psRetSearchName != NULL);
						if (pInfo->Rec.sName.CompareNoCase(pInfo->pszSearchName) == 0)
							*pInfo->psRetSearchName = pInfo->Rec.sName;
					}
				}
				pInfo->EmptyRec();
			}
		}
		break;

	case XmlNodeType_Element:
		if (sXmlPath.CompareNoCase(XML_S3_DIR_LISTING_ROOT_ELEMENT) == 0)
		{
			pInfo->bGotRootElement = true;
		}
		break;

	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::DirListingInternal(
	LPCTSTR pszPathIn,
	DirEntryList_t& DirList,
	LPCTSTR pszSearchName,
	CString& sRetSearchName,
	bool bS3Versions,
	bool bSingle,							// if true, don't keep going back for more files. we just want to know if there are SOME
	DWORD *pdwGetECSRetention)				// if non-NULL, check for ECS bucket retention. return retention period
{
	CECSConnectionState& State(GetStateBuf());
	S3_ERROR Error;
	XML_DIR_LISTING_CONTEXT Context;
	list<HEADER_REQ> Req;
	CString sS3NextKeyMarker;			// passed to key-marker in next request
	CString sS3NextVersionIdMarker;		// passed to version-id-marker in next request

	try
	{
		Context.sPathIn = pszPathIn;
		Context.pszSearchName = pszSearchName;
		Context.psRetSearchName = &sRetSearchName;
		Context.pDirList = &DirList;
		Context.bValid = false;
		Context.bS3Versions = bS3Versions;
		Context.bSingle = bSingle;
		{
			CSingleLock lockDir(&Context.csDirList, true);
			DirList.clear();
		}
		CBuffer RetData;
		State.sEmcToken.Empty();
		do
		{
			Req.clear();
			InitHeader();
			CString sResource;
			if (Context.sPathIn.IsEmpty())
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_INVALID_NAME);
			{
				// gotta take the bucket off the first component of the path
				CString sBucket, sPrefix;
				int iSlash = Context.sPathIn.Find(L'/', 1);
				if (iSlash < 0)
					sBucket = Context.sPathIn;
				else
				{
					sBucket = Context.sPathIn.Left(iSlash);					// don't include terminating slash
					sPrefix = Context.sPathIn.Mid(iSlash + 1);
					sPrefix = UriEncode(sPrefix, true);
				}
				sResource = sBucket + _T("/");
				if (bS3Versions)
					sResource += _T("?versions&delimiter=/");
				else
					sResource += _T("?delimiter=/");
				if (bSingle || (MaxS3ListingSize < 1000))						//lint !e774	// (Info -- Boolean within 'if' always evaluates to True
					sResource += CString(_T("&max-keys=")) + (bSingle ? _T("10") : (LPCTSTR)FmtNum(MaxS3ListingSize));
				if (!sPrefix.IsEmpty())
					sResource += _T("&prefix=") + sPrefix;
				if (!State.sEmcToken.IsEmpty())
					sResource += UriEncode(_T("&marker=") + State.sEmcToken);
				if (pdwGetECSRetention != nullptr)
				{
					Req.push_back(HEADER_REQ(_T("x-emc-retention-period")));
					*pdwGetECSRetention = 0;
				}
			}
			Error = SendRequest(_T("GET"), (LPCTSTR)sResource, NULL, 0, RetData, &Req);
			if (Error.IfError())
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			if (pdwGetECSRetention != nullptr)
			{
				// get the retention status of this bucket
				CString sRetentionPeriod;
				for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
				{
					if (it->sLabel == _T("x-emc-retention-period"))
					{
						for (list<CString>::const_iterator itResp = it->ContentList.begin(); itResp != it->ContentList.end(); ++itResp)
						{
							sRetentionPeriod = *itResp;
							sRetentionPeriod.TrimLeft();
							sRetentionPeriod.TrimRight();
							*pdwGetECSRetention = (DWORD)_ttoi(sRetentionPeriod);
							break;
						}
					}
				}
			}
			if (RetData.IsEmpty())
			{
				// it returns SUCCESS but there is no data!
				// this seems to be an invalid condition, return Internal Error
				Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
				Error.S3Error = S3_ERROR_UNKNOWN;
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			}
			if (strncmp((LPCSTR)RetData.GetData(), "<?xml", 5) != 0)
			{
				// XML doesn't look valid. maybe we are connected to the wrong server?
				// maybe there is a man-in-middle attack?
				Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
				Error.S3Error = S3_ERROR_MalformedXML;
				Error.sS3Code = _T("MalformedXML");
				Error.sS3RequestID = _T("GET");
				Error.sS3Resource = sResource;
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			}
			Context.bGotRootElement = false;
			{
				XMLLITE_READER_CB procXmlDirListingCB;
				State.sEmcToken.Empty();
				sS3NextKeyMarker.Empty();
				sS3NextVersionIdMarker.Empty();
				if (bS3Versions)
				{
					procXmlDirListingCB = XmlDirListingS3VersionsCB;
				}
				else
				{
					procXmlDirListingCB = XmlDirListingS3CB;
				}
				{
					CSingleLock lockDir(&Context.csDirList, true);
					Context.bGotSysMetadata = true;						// with S3, it always returns metadata
					HRESULT hr = ScanXml(&RetData, &Context, procXmlDirListingCB);
					if (FAILED(hr))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, hr);
					if (!Context.sPrefix.IsEmpty() && !Context.bIsTruncated && Context.pDirList->empty() && !Context.bGotKey)
					{
						CECSConnection::S3_ERROR Error;
						Error.dwHttpError = HTTP_STATUS_NOT_FOUND;
						Error.S3Error = S3_ERROR_NoSuchKey;
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
					}
					if (Context.bIsTruncated)
					{
						// listing was truncated
						State.sEmcToken = Context.sS3NextMarker;
						sS3NextKeyMarker = Context.sS3NextKeyMarker;
						sS3NextVersionIdMarker = Context.sS3NextVersionIdMarker;
					}
				}
			}
			// validate XML
			// make sure that it appears to be basically correct
			// we don't want total garbage making it look like there are NO entries in the directory
			// and therefore locally deleting all the files in the dir
			if (!Context.bGotRootElement)
			{
				// XML doesn't look valid. maybe we are connected to the wrong server?
				// maybe there is a man-in-middle attack?
				Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
				Error.S3Error = S3_ERROR_MalformedXML;
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			}
			{
				CSingleLock lockDirList(&csDirListList, true);
				for (list<LISTING_CONTEXT_MONITOR *>::iterator itMon = Context.DirMonitorList.begin()
					; itMon != Context.DirMonitorList.end() ; ++itMon)
				{
					VERIFY((*itMon)->evUpdate.SetEvent());
				}
				// if we are searching for a name and we found one, we can get out now
				// no need to keep reading names (if this is a long dir)
				if ((Context.pszSearchName != NULL) && !Context.psRetSearchName->IsEmpty())
				{
					// we can stop only if there are no other threads hanging on our output
					if (Context.DirMonitorList.empty())
					{
						// no other threads are waiting on us!
						// we need to remove us from the list now to prevent another thread from attaching to us
						list<XML_DIR_LISTING_CONTEXT *>::iterator itList;
						for (itList=DirListList.begin() ; itList!=DirListList.end() ; ++itList)
						{
							if (*itList == &Context)
							{
								(void)DirListList.erase(itList);
								break;
							}
						}
						break;
					}
				}
			}
		} while (!State.sEmcToken.IsEmpty() && !bSingle);
		DirList.sort();
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	catch (const CErrorInfo& E)
	{
		Error.dwError = E.dwError;
	}
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::DirListing(LPCTSTR pszPath, DirEntryList_t& DirList, bool bSingle, DWORD *pdwGetECSRetention)
{
	CString sRetSearchName;
	return DirListingInternal(pszPath, DirList, nullptr, sRetSearchName, false, bSingle, pdwGetECSRetention);
}

CECSConnection::S3_ERROR CECSConnection::DirListingS3Versions(LPCTSTR pszPath, DirEntryList_t& DirList)
{
	CString sRetSearchName;
	return DirListingInternal(pszPath, DirList, nullptr, sRetSearchName, true, false, nullptr);
}

bool CECSConnection::S3_ERROR::IfError() const
{
	return (dwError != ERROR_SUCCESS)
		|| (dwHttpError >= 400)
		|| (S3Error != S3_ERROR_SUCCESS);
}

CString CECSConnection::S3_ERROR::Format(bool bOneLine) const
{
	CString sMsg, sLineEnd;
	if (bOneLine)
		sLineEnd = _T(", ");
	else
		sLineEnd = _T("\r\n");
	if (dwError != ERROR_SUCCESS)
		sMsg = GetNTErrorText(dwError);
	// if HTTP error unexpected, be sure to display it
	if (((dwHttpError != 0) && (dwHttpError != 200)) || (dwError == (DWORD)HTTP_E_STATUS_UNEXPECTED))
	{
		CString sHttpMsg;
		sHttpMsg = FmtNum(dwHttpError);
		if (!sMsg.IsEmpty())
			sMsg += sLineEnd;
		sMsg += _T("Http: ") + sHttpMsg;
	}
	if (!sS3Code.IsEmpty())
	{
		if (!sMsg.IsEmpty())
			sMsg += sLineEnd;
		sMsg += _T("S3 Code: ") + sS3Code;
	}
	if (!sS3Resource.IsEmpty())
	{
		if (!sMsg.IsEmpty())
			sMsg += sLineEnd;
		sMsg += _T("S3 Resource: ") + sS3Resource;
	}
	if (!sS3RequestID.IsEmpty())
	{
		if (!sMsg.IsEmpty())
			sMsg += sLineEnd;
		sMsg += _T("S3 RequestID: ") + sS3RequestID;
	}
	if (!sDetails.IsEmpty())
	{
		if (!sMsg.IsEmpty())
			sMsg += sLineEnd;
		sMsg += _T("Details: ") + sDetails;
	}
	if (bOneLine)
	{
		// there still may be line ends in there because some error text generated by MS contain line ends
		int iEnd;
		for (;;)
		{
			iEnd = sMsg.Find(_T("\r\n"));
			if (iEnd < 0)
				break;
			(void)sMsg.Delete(iEnd);
			sMsg.SetAt(iEnd, L' ');
		} 
		for (;;)
		{
			iEnd = sMsg.Find(_T("\n\r"));
			if (iEnd < 0)
				break;
			(void)sMsg.Delete(iEnd);
			sMsg.SetAt(iEnd, L' ');
		}
		for (;;)
		{
			iEnd = sMsg.Find(_T("\n"));
			if (iEnd < 0)
				break;
			sMsg.SetAt(iEnd, L' ');
		}
		for (;;)
		{
			iEnd = sMsg.Find(_T("\r"));
			if (iEnd < 0)
				break;
			sMsg.SetAt(iEnd, L' ');
		}
	}
	return sMsg;
}

void CECSConnection::SetThrottle(
	LPCTSTR pszHost,						// host name to apply throttle to
	int iUploadThrottleRate,				// upload throttle rate in bytes/sec. set to 0 to remove throttle for this host
	int iDownloadThrottleRate)				// download throttle
{
	CSingleLock lock(&csThrottleMap, true);

	if ((iUploadThrottleRate == 0) && (iDownloadThrottleRate == 0))
	{
		(void)ThrottleMap.erase(pszHost);		// erase any entry for the specified host
		if (ThrottleMap.empty())
		{
			lock.Unlock();
			TimerThread.KillThreadWait();
		}
	}
	else
	{
		THROTTLE_REC Rec;
		Rec.Upload.iBytesCurInterval = Rec.Upload.iBytesSec = iUploadThrottleRate;
		Rec.Download.iBytesCurInterval = Rec.Download.iBytesSec = iDownloadThrottleRate;
		pair<map<CString,THROTTLE_REC>::iterator, bool> ret = ThrottleMap.insert(make_pair(pszHost, Rec));
		if (!ret.second)
		{
			ret.first->second.Upload.iBytesCurInterval = ret.first->second.Upload.iBytesSec = iUploadThrottleRate;
			ret.first->second.Download.iBytesCurInterval = ret.first->second.Download.iBytesSec = iDownloadThrottleRate;
		}
		if (!TimerThread.IfActive())
			(void)TimerThread.CreateThread();
		TimerThread.StartWork();
	}
}

void CECSConnection::TerminateThrottle(void)
{
	TimerThread.KillThreadWait();
	CSingleLock lock(&csThrottleMap, true);
	ThrottleMap.clear();
}

void CECSConnection::IfThrottle(bool *pbDownloadThrottle, bool *pbUploadThrottle)
{
	bool bUploadThrottle = false;
	bool bDownloadThrottle = false;
	CSingleLock lock(&csThrottleMap, true);
	map<CString, THROTTLE_REC>::iterator itMap;
	itMap = ThrottleMap.find(sHost);
	if (itMap != ThrottleMap.end())
	{
		if (itMap->second.Upload.iBytesSec != 0)
			bUploadThrottle = true;
		if (itMap->second.Download.iBytesSec != 0)
			bDownloadThrottle = true;
	}
	if (pbDownloadThrottle != NULL)
		*pbDownloadThrottle = bDownloadThrottle;
	if (pbUploadThrottle != NULL)
		*pbUploadThrottle = bUploadThrottle;
}

struct XML_S3_SERVICE_INFO_CONTEXT
{
	CECSConnection::S3_SERVICE_INFO *pServiceInfo;
	CECSConnection::S3_BUCKET_INFO Entry;
};

const WCHAR * const XML_S3_SERVICE_OWNER_ID = L"//ListAllMyBucketsResult/Owner/ID";
const WCHAR * const XML_S3_SERVICE_OWNER_NAME = L"//ListAllMyBucketsResult/Owner/DisplayName";
const WCHAR * const XML_S3_SERVICE_BUCKET_NAME = L"//ListAllMyBucketsResult/Buckets/Bucket/Name";
const WCHAR * const XML_S3_SERVICE_BUCKET_DATE = L"//ListAllMyBucketsResult/Buckets/Bucket/CreationDate";
const WCHAR * const XML_S3_SERVICE_BUCKET_ELEMENT = L"//ListAllMyBucketsResult/Buckets/Bucket";

HRESULT XmlS3ServiceInfoCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_S3_SERVICE_INFO_CONTEXT *pInfo = (XML_S3_SERVICE_INFO_CONTEXT *)pContext;
	if ((pInfo == NULL) || (pInfo->pServiceInfo == NULL))
		return ERROR_INVALID_DATA;

	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_S3_SERVICE_OWNER_ID) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->pServiceInfo->sOwnerID = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_S3_SERVICE_OWNER_NAME) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->pServiceInfo->sOwnerDisplayName = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_S3_SERVICE_BUCKET_NAME) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->Entry.sName = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_S3_SERVICE_BUCKET_DATE) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
			{
				CECSConnection::S3_ERROR Error = CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Entry.ftCreationDate);
				if (Error.IfError())
					ZeroFT(pInfo->Entry.ftCreationDate);
			}
		}
		break;

	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_S3_SERVICE_BUCKET_ELEMENT) == 0)
		{
			// finished receiving a BUCKET element
			if (!pInfo->Entry.sName.IsEmpty())
			{
				pInfo->pServiceInfo->BucketList.push_back(pInfo->Entry);
				pInfo->Entry.Empty();
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

// S3ServiceInformation
// return service version information
// this is used mostly as way to know if the server is up and can be connected to
CECSConnection::S3_ERROR CECSConnection::S3ServiceInformation(S3_SERVICE_INFO& ServiceInfo)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	Error = SendRequest(_T("GET"), _T("/"), NULL, 0, RetData, &Req);
	if (Error.IfError())
		return Error;
	ServiceInfo.BucketList.clear();
	ServiceInfo.sOwnerDisplayName.Empty();
	ServiceInfo.sOwnerID.Empty();
	if ((RetData.GetBufSize() > 5) && (strncmp((LPCSTR)RetData.GetData(), "<?xml", 5) == 0))
	{
		XML_S3_SERVICE_INFO_CONTEXT Context;
		Context.pServiceInfo = &ServiceInfo;
		HRESULT hr = ScanXml(&RetData, &Context, XmlS3ServiceInfoCB);
		if (FAILED(hr))
			return hr;
	}
	else
	{
		// XML doesn't look valid. maybe we are connected to the wrong server?
		// maybe there is a man-in-middle attack?
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_MalformedXML;
		Error.sS3Code = _T("MalformedXML");
		Error.sS3RequestID = _T("GET");
		Error.sS3Resource = _T("/");
	}
	return Error;
}

// CECSConnection::GetHost
CString CECSConnection::GetHost(void) const
{
	return sHost;
}

CECSConnection::S3_ERROR CECSConnection::GetTags(LPCTSTR pszPathIn, list<CString>& TagList)
{
	S3_ERROR Error;
	try
	{
		CBuffer RetData;
		InitHeader();
		// get the complete list of metadata for this object
		list<HEADER_REQ> Req;
		Error = SendRequest(_T("HEAD"), UriEncode(pszPathIn), NULL, 0, RetData, &Req);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
		for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
		{
			if (!it->ContentList.empty())
			{
				if (it->sLabel.Find(sAmzMetaPrefix) == 0)
				{
					CString sTag(it->sLabel.Mid(sAmzMetaPrefix.GetLength()));				// strip off x-amz-meta-
					TagList.push_back(sTag);
				}
			}
		}
		TagList.sort();
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return ERROR_SUCCESS;
}

// WriteMetadata
// set specified metadata to path
// metadata is encoded using base64 and there is a maximum of 1k characters for each field
// and a maximum of 8k for the header
// it will encode it. if it is bigger than 7k, it is rejected
// if not, it splits it into 1k segments. The first has the name specified (pszTag)
// the overflow adds "_n", where n is 2 to 7
CECSConnection::S3_ERROR CECSConnection::WriteMetadataEntry(list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszTag, const CBuffer& Data)
{
	// convert to ANSI string
	return WriteMetadataEntry(MDList, pszTag, Data.EncodeBase64());
}

CECSConnection::S3_ERROR CECSConnection::WriteMetadataEntry(list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszTag, const CString& sStr)
{
	// if the specified tag already exists, erase it
	CString sTag(pszTag);
	for (list<S3_METADATA_ENTRY>::iterator itList = MDList.begin(); itList != MDList.end(); )
	{
		if (sTag == itList->sTag)
			itList = MDList.erase(itList);
		else
			++itList;
	}
	S3_METADATA_ENTRY Rec;
	Rec.sTag = pszTag;
	Rec.sData = sStr;
	MDList.push_back(Rec);
	return ERROR_SUCCESS;
}

CECSConnection::S3_ERROR CECSConnection::GetObjectSizeS3(
	LPCTSTR pszSrcPath,			// S3 path (translated) of source object
	LPCTSTR pszVersionId,		// (in) version ID or nullptr
	ULONGLONG *pullObjSize)		// (out) object size
{
	CString sSrcPath(pszSrcPath);
	if (sSrcPath.IsEmpty())
		return ERROR_INVALID_DATA;
	int iSlash = sSrcPath.Find(L'/', 1);
	if (iSlash < 0)
		return ERROR_INVALID_DATA;
	CString sResource(sSrcPath.Left(iSlash + 1));	// get terminating slash on bucket
	CString sPrefix(sSrcPath.Mid(iSlash + 1));
	sResource += _T("?versions&delimiter=/");
	if (!sPrefix.IsEmpty())
	{
		sPrefix = UriEncode(sPrefix, true);
		sResource += _T("&prefix=") + sPrefix;
	}
	CBuffer RetData;
	S3_ERROR Error = SendRequest(_T("GET"), (LPCTSTR)sResource, NULL, 0, RetData);
	if (Error.IfError())
		return Error;
	if (RetData.IsEmpty())
	{
		// it returns SUCCESS but there is no data!
		// this seems to be an invalid condition, return Internal Error
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_UNKNOWN;
		return Error;
	}
	if (strncmp((LPCSTR)RetData.GetData(), "<?xml", 5) != 0)
	{
		// XML doesn't look valid. maybe we are connected to the wrong server?
		// maybe there is a man-in-middle attack?
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_MalformedXML;
		Error.sS3Code = _T("MalformedXML");
		Error.sS3RequestID = _T("GET");
		Error.sS3Resource = sResource;
		return Error;
	}
	XML_DIR_LISTING_CONTEXT Context;
	DirEntryList_t DirList;
	Context.sPathIn = pszSrcPath;
	Context.pDirList = &DirList;
	Context.bValid = false;
	{
		CSingleLock lockDir(&Context.csDirList, true);
		Context.bGotSysMetadata = true;						// with S3, it always returns metadata
		HRESULT hr = ScanXml(&RetData, &Context, XmlDirListingS3VersionsCB);
		if (FAILED(hr))
			return hr;
		for (DirEntryList_t::iterator itList = DirList.begin(); itList != DirList.end(); ++itList)
		{
			// the name must be empty. an empty name means an exact match to the prefix
			if (itList->sName.IsEmpty() && !itList->Properties.bDeleted)
			{
				if (((pszVersionId == nullptr) && itList->Properties.bIsLatest)
					|| ((pszVersionId != nullptr) && (itList->Properties.sVersionId == pszVersionId)))
				{
					// found it!
					if (pullObjSize != nullptr)
						*pullObjSize = itList->Properties.llSize;
					return ERROR_SUCCESS;
				}
			}
		}
	}
	return ERROR_NOT_FOUND;
}

CECSConnection::S3_ERROR CECSConnection::CopyS3(
	LPCTSTR pszSrcPath,			// S3 path of source object
	LPCTSTR pszTargetPath,		// S3 path of target object
	LPCTSTR pszVersionId,		// nonNULL: version ID to copy
	bool bCopyMD,				// true - copy metadata, false - replace
	ULONGLONG *pullObjSize,		// optional - if object size supplied it doesn't have to query it
	const list<S3_METADATA_ENTRY> *pMDList)	// list of metadata to apply to object
{
	const ULONGLONG MULTIPART_SIZE = GIGABYTES(1ULL);		// each part will be 1GB

	S3_ERROR Error;
	CBuffer RetData;
	list<HEADER_REQ> Req;
	ULONGLONG ullObjSize = 0;
	bool bMultiPartInitiated = false;
	list<shared_ptr<CECSConnection::S3_UPLOAD_PART_ENTRY>> PartList;
	S3_UPLOAD_PART_INFO MultiPartInfo;

	try
	{
		InitHeader();
		if (pullObjSize != nullptr)
			ullObjSize = *pullObjSize;
		else
		{
			Error = GetObjectSizeS3(pszSrcPath, pszVersionId, &ullObjSize);
			if (Error.IfError())
				return Error;
		}
		InitHeader();
		// if object is < 5GB, a single copy will do it.
		// now construct the new metadata list
		if (ullObjSize < GIGABYTES(5ULL))
		{
			CString sSrcPath(UriEncode(pszSrcPath));
			if (pszVersionId != nullptr)
				sSrcPath += CString(_T("?versionId=")) + pszVersionId;
			AddHeader(_T("x-amz-copy-source"), sSrcPath);							// specify source of copy
			AddHeader(_T("x-amz-metadata-directive"), bCopyMD ? _T("COPY") : _T("REPLACE"));
			if ((pMDList != nullptr) && !pMDList->empty())
			{
				// add in all metadata
				for (list<S3_METADATA_ENTRY>::const_iterator itList = pMDList->begin(); itList != pMDList->end(); ++itList)
					AddHeader(sAmzMetaPrefix + itList->sTag, itList->sData);
			}
			Error = SendRequest(_T("PUT"), UriEncode(pszTargetPath), NULL, 0, RetData, &Req);
			return Error;
		}
		Error = S3MultiPartInitiate(pszTargetPath, MultiPartInfo, pMDList);
		if (Error.IfError())
			return Error;
		bMultiPartInitiated = true;
		// copy all parts
		ULONGLONG ullOffset = 0ULL;
		for (UINT uPartNum = 1; ; ++uPartNum)
		{
			shared_ptr<S3_UPLOAD_PART_ENTRY> pPartEntry = make_shared<S3_UPLOAD_PART_ENTRY>();
			pPartEntry->uPartNum = uPartNum;
			pPartEntry->ullBaseOffset = ullOffset;
			pPartEntry->Checksum.Empty();
			pPartEntry->sETag.Empty();
			pPartEntry->ullPartSize = ((ullObjSize - ullOffset) < (ULONGLONG)MULTIPART_SIZE) ? (ullObjSize - ullOffset) : MULTIPART_SIZE;
			if ((pPartEntry->ullPartSize == 0) || (ullObjSize <= ullOffset))

				break;
			Error = S3MultiPartUpload(MultiPartInfo, *pPartEntry, nullptr, pPartEntry->ullPartSize, pszSrcPath, ullOffset, pszVersionId);
			if (Error.IfError())
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
			PartList.push_back(pPartEntry);
			ullOffset += pPartEntry->ullPartSize;
		}
		S3_MPU_COMPLETE_INFO MPUCompleteInfo;
		Error = S3MultiPartComplete(MultiPartInfo, PartList, MPUCompleteInfo);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
	}
	catch (const CS3ErrorInfo& E)
	{
		if (bMultiPartInitiated)
		{
			(void)S3MultiPartAbort(MultiPartInfo);

		}
		return E.Error;
	}

	return Error;
}

CECSConnection::S3_ERROR CECSConnection::UpdateMetadataS3(LPCTSTR pszPath, const list<S3_METADATA_ENTRY>& MDListParam, const list<CString> *pDeleteTagParam)
{
	S3_ERROR Error;
	try
	{
		CECSConnectionState& State(GetStateBuf());
		list<S3_METADATA_ENTRY> MDList = MDListParam;
		CBuffer RetData;
		list<HEADER_REQ> Req;
		CString sPath(pszPath);
		InitHeader();
		Error = SendRequest(_T("HEAD"), UriEncode(sPath), NULL, 0, RetData, &Req);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);

		for (list<HEADER_REQ>::iterator it = Req.begin(); it != Req.end(); ++it)
		{
			if (!it->ContentList.empty() && (it->sLabel.Find(sAmzMetaPrefix) == 0))
			{
				CString sTag(it->sLabel.Mid(sAmzMetaPrefix.GetLength()));				// strip off x-amz-meta-
				for (list<S3_METADATA_ENTRY>::const_iterator itList = MDList.begin(); itList != MDList.end();)
				{
					if ((itList->sTag.CompareNoCase(sTag) == 0) && (it->ContentList.size() > 0))
					{
						it->ContentList.front() = itList->sData;
						itList = MDList.erase(itList);
					}
					else
						++itList;
				}
			}
		}
		// now construct the new metadata list
		for (list<HEADER_REQ>::iterator it = Req.begin(); it != Req.end(); ++it)
			if (!it->ContentList.empty() && (it->sLabel.Find(sAmzMetaPrefix) == 0))
				AddHeader(it->sLabel, it->ContentList.front());
		// add in any new tags
		for (list<S3_METADATA_ENTRY>::const_iterator itList = MDList.begin(); itList != MDList.end(); ++itList)
			AddHeader(sAmzMetaPrefix + itList->sTag, itList->sData);
		// delete tags
		if (pDeleteTagParam != NULL)
		{
			for (list<CString>::const_iterator itDel = pDeleteTagParam->begin(); itDel != pDeleteTagParam->end(); ++itDel)
			{
				CString sTag(sAmzMetaPrefix + *itDel);
				sTag.MakeLower();
				(void)State.Headers.erase(sTag);
			}
		}
		AddHeader(_T("x-amz-copy-source"), UriEncode(sPath));							// copy it to itself
		AddHeader(_T("x-amz-metadata-directive"), _T("REPLACE"));
		Error = SendRequest(_T("PUT"), UriEncode(sPath), NULL, 0, RetData, &Req);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return ERROR_SUCCESS;
}

CECSConnection::S3_ERROR CECSConnection::WriteMetadata(LPCTSTR pszPath, const list<S3_METADATA_ENTRY>& MDList)
{
	return UpdateMetadataS3(pszPath, MDList);
}

// ReadMetadata
// get specified metadata from path
// metadata is encoded using base64 and there is a maximum of 1k characters for each field
// and a maximum of 8k for the header
// this will first get the list of all metadata on the object
// then parse out only the specified tag (with extensions)
// then read all of the tags, reassemble the data and convert it to binary
CECSConnection::S3_ERROR CECSConnection::ReadMetadata(LPCTSTR pszPath, LPCTSTR pszTag, CBuffer& Data, LPCTSTR pszVersionId)
{
	S3_ERROR Error;
	CString sData;

	Error = ReadMetadata(pszPath, pszTag, sData, pszVersionId);
	if (Error.IfError())
		return Error;

	// now decode it using base64 into a binary blob
	Data.LoadBase64(sData);
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::ReadMetadata(LPCTSTR pszPath, LPCTSTR pszTag, CString& sData, LPCTSTR pszVersionId)
{
	S3_ERROR Error;

	list<CString> TagRequestList;
	list<S3_METADATA_ENTRY> MDList;
	TagRequestList.push_back(pszTag);
	Error = ReadMetadataBulkInternalS3(pszPath, TagRequestList, MDList, pszVersionId);
	if (Error.IfError())
		return Error;
	for (list<S3_METADATA_ENTRY>::const_iterator itList = MDList.begin(); itList != MDList.end(); ++itList)
	{
		if (itList->sTag == pszTag)
		{
			sData = itList->sData;
			return Error;
		}
	}
	Error.S3Error = S3_ERROR_NoSuchKey;
	Error.dwHttpError = HTTP_STATUS_BAD_REQUEST;
	return Error;				// no metadata for this object
}

//
// ReadMetadataBulk
// read multiple metadata tags
// attempt to combine as many as possible
// the problem here is that each tag can be at most 1k and the total header can be 8k
// this means that without knowing the size of each tag, we can only ask for 7 at a shot
// even though it may be possible to ask for more if each tag was not the full 1k
// a complete tag list is passed in. If it is not supplied, it is done internally
//
CECSConnection::S3_ERROR CECSConnection::ReadMetadataBulk(LPCTSTR pszPath, const list<CString>& TagRequestList, list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszVersionId)
{
	return ReadMetadataBulkInternalS3(pszPath, TagRequestList, MDList, pszVersionId);
}

CECSConnection::S3_ERROR CECSConnection::ReadMetadataBulk(LPCTSTR pszPath, list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszVersionId)
{
	list<CString> TagRequestList;
	return ReadMetadataBulkInternalS3(pszPath, TagRequestList, MDList, pszVersionId);
}

CECSConnection::S3_ERROR CECSConnection::ReadMetadataBulkInternalS3(LPCTSTR pszPath, const list<CString>& TagRequestList, list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszVersionId)
{
	S3_ERROR Error;
	CString sPath(pszPath);
	CBuffer RetData;
	list<HEADER_REQ> Req;

	try
	{
		InitHeader();
		if ((pszVersionId != nullptr) && (*pszVersionId != NUL))
			sPath += CString(_T("?versionId=")) + pszVersionId;
		Error = SendRequest(_T("HEAD"), UriEncode(sPath), NULL, 0, RetData, &Req);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);

		for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
		{
			if (!it->ContentList.empty())
			{
				if (it->sLabel.Find(sAmzMetaPrefix) == 0)
				{
					CString sTag(it->sLabel.Mid(sAmzMetaPrefix.GetLength()));				// strip off x-amz-meta-
					bool bMatch = TagRequestList.empty();
					if (!bMatch)
					{
						for (list<CString>::const_iterator itTagReq = TagRequestList.begin(); itTagReq != TagRequestList.end(); ++itTagReq)
						{
							if (*itTagReq == sTag)
								bMatch = true;
							break;
						}
					}
					if (bMatch)
					{
						for (list<CString>::const_iterator itContents = it->ContentList.begin(); itContents != it->ContentList.end(); ++itContents)
						{
							S3_METADATA_ENTRY Rec;
							Rec.sTag = sTag;
							Rec.sData = *itContents;
							MDList.push_back(Rec);
						}
					}
				}
			}
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return ERROR_SUCCESS;
}

// ReadMetadataBulkEntry
// get the specific metadata entry from the bulk buffer
void CECSConnection::ReadMetadataBulkEntry(list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszTag, CString& sValue)
{
	sValue.Empty();
	list<S3_METADATA_ENTRY>::iterator itList;

	for (itList = MDList.begin() ; itList != MDList.end() ; ++itList)
	{
		if (itList->sTag == pszTag)
		{
			sValue = itList->sData;
			break;
		}
	}
}

void CECSConnection::ReadMetadataBulkEntry(list<S3_METADATA_ENTRY>& MDList, LPCTSTR pszTag, CBuffer& Value)
{
	Value.Empty();
	CString sValue;
	ReadMetadataBulkEntry(MDList, pszTag, sValue);
	if (sValue.IsEmpty())
		return;
	// now decode it using base64 into a binary blob
	Value.LoadBase64(sValue);
}

// ParseS3Timestamp
// convert server time field to FILETIME
CECSConnection::S3_ERROR CECSConnection::ParseS3Timestamp(const CString& sS3Time, FILETIME& ftTime)
{
	S3_ERROR Error;
	SYSTEMTIME stTime;
	int iYear, iMonth, iDay, iHour, iMin, iSec;
	int iMilliSec = 0;
	ZeroMemory(&stTime, sizeof(stTime));
	// the format of this field: 2010-07-05T19:51:19Z
	int iConv = _stscanf_s(sS3Time, _T("%d-%d-%dT%d:%d:%dZ"), &iYear, &iMonth, &iDay, &iHour, &iMin, &iSec);
	if (iConv != 6)
	{
		iConv = _stscanf_s(sS3Time, _T("%d-%d-%dT%d:%d:%d.%dZ"), &iYear, &iMonth, &iDay, &iHour, &iMin, &iSec, &iMilliSec);
		if (iConv != 7)
		{
			Error.sDetails = sS3Time;
			Error.dwError = ERROR_INVALID_DATA;
			return Error;
		}
	}
	stTime.wYear = (WORD)iYear;
	stTime.wMonth = (WORD)iMonth;
	stTime.wDay = (WORD)iDay;
	stTime.wHour = (WORD)iHour;
	stTime.wMinute = (WORD)iMin;
	stTime.wSecond = (WORD)iSec;
	stTime.wMilliseconds = (WORD)iMilliSec;
	if (!SystemTimeToFileTime(&stTime, &ftTime))
	{
		Error.sDetails = sS3Time;
		Error.dwError = GetLastError();
		return Error;
	}
	return ERROR_SUCCESS;
}

// DeleteMetadata
// delete specified user metadata
// pszTag can contain multiple, comma-separated tags
CECSConnection::S3_ERROR CECSConnection::DeleteMetadata(LPCTSTR pszPath, LPCTSTR pszTag)
{
	S3_ERROR Error;
	try
	{
		list<S3_METADATA_ENTRY> MDList;
		list<CString> DeleteList;
		DeleteList.push_back(pszTag);
		return UpdateMetadataS3(pszPath, MDList, &DeleteList);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

// take text permission and translate it to E_S3_ACL_VALUES enum
CECSConnection::E_S3_ACL_VALUES TranslateACLText(LPCTSTR pszEntry)
{
	CString sValue(pszEntry);
	sValue.TrimLeft();
	sValue.TrimRight();
	if (sValue.CompareNoCase(_T("NONE")) == 0)
		return CECSConnection::AAV_NONE;
	if (sValue.CompareNoCase(_T("READ")) == 0)
		return CECSConnection::AAV_READ;
	if (sValue.CompareNoCase(_T("WRITE")) == 0)
		return CECSConnection::AAV_WRITE;
	if (sValue.CompareNoCase(_T("FULL_CONTROL")) == 0)
		return CECSConnection::AAV_FULL_CONTROL;
	if (sValue.CompareNoCase(_T("READ_ACP")) == 0)
		return CECSConnection::AAV_READ_ACP;
	if (sValue.CompareNoCase(_T("WRITE_ACP")) == 0)
		return CECSConnection::AAV_WRITE_ACP;
	return CECSConnection::AAV_INVALID;
}

struct XML_S3_ACL_CONTEXT
{
	deque<CECSConnection::ACL_ENTRY> *pAcls;
	CECSConnection::ACL_ENTRY Rec;
	CString sOwner;
	CString sDisplayName;
	bool bCanonicalUser;
	bool bGroup;

	XML_S3_ACL_CONTEXT()
		: pAcls(NULL)
		, bCanonicalUser(false)
		, bGroup(false)
	{}
};

const WCHAR * const XML_S3_ACL_OWNER_ID =							L"//AccessControlPolicy/Owner/ID";
const WCHAR * const XML_S3_ACL_OWNER_DISPLAYNAME =					L"//AccessControlPolicy/Owner/DisplayName";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_GRANTEE_ID =				L"//AccessControlPolicy/AccessControlList/Grant/Grantee/ID";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_GRANTEE_URI =			L"//AccessControlPolicy/AccessControlList/Grant/Grantee/URI";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_GRANTEE_DISPLAYNAME =	L"//AccessControlPolicy/AccessControlList/Grant/Grantee/DisplayName";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_PERMISSION =				L"//AccessControlPolicy/AccessControlList/Grant/Permission";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_ELEMENT =				L"//AccessControlPolicy/AccessControlList/Grant";
const WCHAR * const XML_S3_ACL_OWNER_GRANT_GRANTEE_ELEMENT =		L"//AccessControlPolicy/AccessControlList/Grant/Grantee";

HRESULT XmlS3AclContext_CB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_S3_ACL_CONTEXT *pInfo = (XML_S3_ACL_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_ID) == 0)
				pInfo->sOwner = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_DISPLAYNAME) == 0)
				pInfo->sDisplayName = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_GRANTEE_ID) == 0)
				pInfo->Rec.sID = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_GRANTEE_URI) == 0)
				pInfo->Rec.sID = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_GRANTEE_DISPLAYNAME) == 0)
				pInfo->Rec.sDisplayName = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_PERMISSION) == 0)
				pInfo->Rec.Acl = TranslateACLText(FROM_UNICODE(*psValue));
		}
		break;
	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_ELEMENT) == 0)
		{
			if (!pInfo->Rec.sID.IsEmpty())
			{
				pInfo->Rec.bGroup = pInfo->bGroup;
				pInfo->pAcls->push_back(pInfo->Rec);
			}
			pInfo->Rec.Clear();
		}
		break;
	case XmlNodeType_Element:
		if (sXmlPath.CompareNoCase(XML_S3_ACL_OWNER_GRANT_GRANTEE_ELEMENT) == 0)
		{
			pInfo->bCanonicalUser = false;
			pInfo->bGroup = false;

			if (pAttrList != nullptr)
			{
				for (list<XML_LITE_ATTRIB>::const_iterator itList = pAttrList->begin(); itList != pAttrList->end(); ++itList)
				{
					if (itList->sAttrName.Find(L"type") >= 0)
					{
						if (itList->sValue.CompareNoCase(L"CanonicalUser") == 0)
							pInfo->bCanonicalUser = true;
						else if (itList->sValue.CompareNoCase(L"Group") == 0)
							pInfo->bGroup = true;
					}
				}
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

// CECSConnection::ReadACL
// return the list of acl entries for the file
// it is returned in 2 lists: user acls and group acls
CECSConnection::S3_ERROR CECSConnection::ReadACL(
	LPCTSTR pszPath,
	deque<CECSConnection::ACL_ENTRY>& Acls,
	LPCTSTR pszVersion)
{
	CBuffer RetData;
	S3_ERROR Error;
	try
	{
		InitHeader();
		list<HEADER_REQ> Req;
		CString sResource(UriEncode(CString(pszPath) + _T("?acl")));
		CString sVersion(pszVersion);
		if (!sVersion.IsEmpty())
			sResource += _T("&versionId=") + sVersion;
		Error = SendRequest(_T("GET"), sResource, NULL, 0, RetData, &Req);
		if (Error.IfError())
			return Error;
		// parse the output and fill the output lists
		Acls.clear();

		// parse the returned XML
		XML_S3_ACL_CONTEXT Context;
		HRESULT hr;
		Context.pAcls = &Acls;
		hr = ScanXml(&RetData, &Context, XmlS3AclContext_CB);
		if (FAILED(hr))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, hr);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

// CECSConnection::WriteACL
// replace the list of acl entries for the file/directory
CECSConnection::S3_ERROR CECSConnection::WriteACL(
	LPCTSTR pszPath,
	const deque<CECSConnection::ACL_ENTRY>& UserAcls,
	LPCTSTR pszVersion)
{
	CBuffer RetData;
	S3_ERROR Error;
	S3_SERVICE_INFO S3Info;
	try
	{
		InitHeader();

		// create XML request
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"AccessControlPolicy", L"http://s3.amazonaws.com/doc/2006-03-01/")))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"Owner", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"ID", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(S3Info.sOwnerID))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

//			if (FAILED(pWriter->WriteStartElement(NULL, L"DisplayName", NULL)))
//				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
//			if (FAILED(pWriter->WriteString(S3Info.sOwnerDisplayName)))
//				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
//			if (FAILED(pWriter->WriteEndElement()))
//				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteEndElement()))						// end Owner
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"AccessControlList", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		for (deque<CECSConnection::ACL_ENTRY>::const_iterator it = UserAcls.begin(); it != UserAcls.end(); ++it)
		{
			if (FAILED(pWriter->WriteStartElement(NULL, L"Grant", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteStartElement(NULL, L"Grantee", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteAttributeString(L"xmlns", L"xsi", NULL, L"http://www.w3.org/2001/XMLSchema-instance")))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteAttributeString(L"xsi", L"type", NULL, L"CanonicalUser")))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"ID", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(it->sID))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteEndElement()))						// end Grantee
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"Permission", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(it->GetAclString()))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndElement()))						// end Grant
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		}

		if (FAILED(pWriter->WriteEndElement()))						// end AccessControlList
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndElement()))						// end AccessControlPolicy
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		CString sResource(UriEncode(CString(pszPath) + _T("?acl")));
		CString sVersion(pszVersion);
		if (!sVersion.IsEmpty())
			sResource += _T("&versionId=") + sVersion;
		Error = SendRequest(_T("PUT"), sResource, XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

bool CECSConnection::TestAbort(void)
{
	CECSConnectionState& State(GetStateBuf());
	if (!bCheckShutdown)
		return false;
	CSingleLock lock(&State.csAbortList, true);
	for (list<ABORT_ENTRY>::iterator itList = AbortList.begin() ; itList!= AbortList.end() ; ++itList)
	{
		if ((itList->ShutdownCB != NULL) && (itList->ShutdownCB)(itList->pShutdownContext))
			return true;
		if (itList->pbAbort != NULL)
		{
			bool bAbort = *(itList->pbAbort);
			if (itList->bAbortIfTrue)
			{
				if (bAbort)
					return true;
			}
			else
			{
				if (!bAbort)
					return true;
			}
		}
	}
	return false;
}

void CECSConnection::RegisterShutdownCB(TEST_SHUTDOWN_CB ShutdownParamCB, void *pContext)
{
	CECSConnectionState& State(GetStateBuf());
	ABORT_ENTRY Rec;
	Rec.ShutdownCB = ShutdownParamCB;
	Rec.pShutdownContext = pContext;
	CSingleLock lock(&State.csAbortList, true);
	AbortList.push_back(Rec);
}

void CECSConnection::UnregisterShutdownCB(TEST_SHUTDOWN_CB ShutdownParamCB)
{
	CECSConnectionState& State(GetStateBuf());
	CSingleLock lock(&State.csAbortList, true);
	for (list<ABORT_ENTRY>::iterator itList = AbortList.begin() ; itList!= AbortList.end() ; )
	{
		if (itList->ShutdownCB == ShutdownParamCB)
			itList = AbortList.erase(itList);
		else
			++itList;
	}
}

void CECSConnection::RegisterAbortPtr(const bool *pbAbort, bool bAbortTrue)
{
	CECSConnectionState& State(GetStateBuf());
	ABORT_ENTRY Rec;
	Rec.pbAbort = pbAbort;
	Rec.bAbortIfTrue = bAbortTrue;
	CSingleLock lock(&State.csAbortList, true);
	AbortList.push_back(Rec);
}

void CECSConnection::UnregisterAbortPtr(const bool *pbAbort)
{
	CECSConnectionState& State(GetStateBuf());
	CSingleLock lock(&State.csAbortList, true);
	for (list<ABORT_ENTRY>::iterator itList = AbortList.begin() ; itList!= AbortList.end() ; )
	{
		if (itList->pbAbort == pbAbort)
			itList = AbortList.erase(itList);
		else
			++itList;
	}
}

void CECSConnection::RegisterDisconnectCB(ECS_DISCONNECT_CB DisconnectParamCB)
{
	DisconnectCB = DisconnectParamCB;
}

void CECSConnection::SetTimeouts(
	DWORD dwWinHttpOptionConnectRetriesParam,
	DWORD dwWinHttpOptionConnectTimeoutParam,
	DWORD dwWinHttpOptionReceiveResponseTimeoutParam,
	DWORD dwWinHttpOptionReceiveTimeoutParam,
	DWORD dwWinHttpOptionSendTimeoutParam,
	DWORD dwBadIPAddrAgeParam)
{
	dwWinHttpOptionConnectRetries = dwWinHttpOptionConnectRetriesParam;
	dwWinHttpOptionConnectTimeout = dwWinHttpOptionConnectTimeoutParam;
	dwWinHttpOptionReceiveResponseTimeout = dwWinHttpOptionReceiveResponseTimeoutParam;
	dwWinHttpOptionReceiveTimeout = dwWinHttpOptionReceiveTimeoutParam;
	dwWinHttpOptionSendTimeout = dwWinHttpOptionSendTimeoutParam;
	dwBadIPAddrAge = dwBadIPAddrAgeParam;
}

void CECSConnection::SetRetries(DWORD dwMaxRetryCountParam)
{
	if (dwMaxRetryCountParam == 0)
		dwMaxRetryCount = MaxRetryCount;
	else
		dwMaxRetryCount = dwMaxRetryCountParam;
}

void CECSConnection::SetMaxWriteRequest(DWORD dwMaxWriteRequestParam)
{
	CSingleLock lock(&csThrottleMap, true);
	list<CECSConnection *>::iterator itList;
	for (itList = ECSConnectionList.begin() ; itList != ECSConnectionList.end() ; ++itList)
	{
		if ((*itList)->sHost == sHost)
		{
			(*itList)->dwMaxWriteRequest = dwMaxWriteRequestParam;
		}
	}
}

void CECSConnection::SetMaxWriteRequestAll(DWORD dwMaxWriteRequestParam)
{
	CSingleLock lock(&csThrottleMap, true);
	list<CECSConnection *>::iterator itList;
	for (itList = ECSConnectionList.begin() ; itList != ECSConnectionList.end() ; ++itList)
	{
		(*itList)->dwMaxWriteRequest = dwMaxWriteRequestParam;
	}
}

// CheckShutdown
// set this to false if you need to do ops during a shutdown
void CECSConnection::CheckShutdown(bool bCheckShutdownParam)
{
	bCheckShutdown = bCheckShutdownParam;
}

void CECSConnection::SetDisableSecureLog(bool bDisable)
{
	CECSConnectionState& State(GetStateBuf());
	State.bDisableSecureLog = bDisable;
}

void CECSConnection::SetDisableUserSupport(bool bDisable)
{
	CECSConnectionState& State(GetStateBuf());
	State.bDisableUserSupport = bDisable;
}

void CECSConnection::GetCertInfo(ECS_CERT_INFO& Rec)
{
	CECSConnectionState& State(GetStateBuf());
	Rec = State.CertInfo;
}

DWORD CECSConnection::ChooseAuthScheme(DWORD dwSupportedSchemes)
{
//	DWORD dwMask;
//	if (RegistryGet(HKEY_LOCAL_MACHINE, LgtoavlibData.msgfmt_data.sORK_OCTOPUS, _T("ProxyAuthMask"), &dwMask) == ERROR_SUCCESS)
//		dwSupportedSchemes &= ~dwMask;

	//  It is the server's responsibility only to accept 
	//  authentication schemes that provide a sufficient level
	//  of security to protect the server's resources.
	//
	//  The client is also obligated only to use an authentication
	//  scheme that adequately protects its username and password.
	//
	//  Thus, this sample code does not use Basic authentication  
	//  because Basic authentication exposes the client's username 
	//  and password to anyone monitoring the connection.

	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_NEGOTIATE)
		return WINHTTP_AUTH_SCHEME_NEGOTIATE;
	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_NTLM)
		return WINHTTP_AUTH_SCHEME_NTLM;
	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_PASSPORT)
		return WINHTTP_AUTH_SCHEME_PASSPORT;
	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_DIGEST)
		return WINHTTP_AUTH_SCHEME_DIGEST;
	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_BASIC)
		return WINHTTP_AUTH_SCHEME_BASIC;
	return 0;
}

// FormatAuthScheme
// output non-localized authorization scheme name
CString CECSConnection::FormatAuthScheme()
{
	CECSConnectionState& State(GetStateBuf());
	//  It is the server's responsibility only to accept 
	//  authentication schemes that provide a sufficient level
	//  of security to protect the server's resources.
	//
	//  The client is also obligated only to use an authentication
	//  scheme that adequately protects its username and password.
	//
	//  Thus, this sample code does not use Basic authentication  
	//  because Basic authentication exposes the client's username 
	//  and password to anyone monitoring the connection.
  
	if (TST_BIT(State.dwProxyAuthScheme, WINHTTP_AUTH_SCHEME_NEGOTIATE))
		return _T("Negotiate");
	if (TST_BIT(State.dwProxyAuthScheme, WINHTTP_AUTH_SCHEME_NTLM))
		return _T("NTLM");
	if (TST_BIT(State.dwProxyAuthScheme, WINHTTP_AUTH_SCHEME_PASSPORT))
		return _T("Passport");
	if (TST_BIT(State.dwProxyAuthScheme, WINHTTP_AUTH_SCHEME_DIGEST))
		return _T("Digest");
	if (TST_BIT(State.dwProxyAuthScheme, WINHTTP_AUTH_SCHEME_BASIC))
		return _T("Basic");
	return _T("");
}

DWORD CECSConnection::GetSecureError(void)
{
	CECSConnectionState& State(GetStateBuf());
	return State.dwSecureError;
}

CString CECSConnection::signS3ShareableURL(const CString& sResource, const CString& sExpire)
{
	CECSConnectionState& State(GetStateBuf());
	CString sSignature(signRequestS3v2(sSecret, _T("GET"), sResource, State.Headers, sExpire));
	return sSignature;
}

CString CECSConnection::GenerateShareableURL(
	LPCTSTR pszPath,									// translated path to object
	SYSTEMTIME *pstExpire)								// must be LOCAL time
{
	CString sPath(pszPath), sURL;
	try
	{
		CString sIP;
		{
			CSimpleRWLockAcquire lock(&rwlIPListHost, false);			// read lock
			if (IPListHost.empty())
				return _T("");
			sIP = IPListHost[0];
		}
		CTime Time(*pstExpire);
		CString sExpire(FmtNum(Time.GetTime()));
		CString sSignature;
		if (bSSL)
			sURL = _T("https:");
		else
			sURL = _T("http:");
		CString sPort;
		if ((bSSL && (Port != INTERNET_DEFAULT_HTTPS_PORT))
				|| (!bSSL && (Port != INTERNET_DEFAULT_HTTP_PORT)))
			sPort = _T(":") + FmtNum(Port);
		sSignature = UriEncodeS3(signS3ShareableURL(UriEncode(sPath), sExpire), true);
		sURL += _T("//") + sIP + sPort + sPath + _T("?AWSAccessKeyId=") + sS3KeyID + _T("&Expires=") + sExpire + _T("&Signature");
		sURL = EncodeSpecialChars(UriEncode(sURL)) + _T("=") + sSignature;
	}
	catch (const CS3ErrorInfo& E)
	{
		(void)E;
		return _T("");
	}
	return sURL;
}

void CECSConnection::RemoveACLDups(deque<CECSConnection::ACL_ENTRY>& AclList)
{
	bool bRetry;
	do
	{
		bRetry = false;
		for (deque<CECSConnection::ACL_ENTRY>::iterator itOuter=AclList.begin() ; !bRetry && (itOuter!=AclList.end()) ; ++itOuter)
		{
			for (deque<CECSConnection::ACL_ENTRY>::iterator itInner=itOuter + 1 ; itInner!=AclList.end() ; ++itInner)
			{
				if (itOuter->sID == itInner->sID)
				{
					// if both READ and WRITE exist, keep both
					// otherwise, keep only the one with the most permissions
					if (((itOuter->Acl == AAV_READ) && (itInner->Acl == AAV_WRITE))
						|| ((itOuter->Acl == AAV_WRITE) && (itInner->Acl == AAV_READ)))
					{
					}
					else
					{
						if ((int)itOuter->Acl < (int)itInner->Acl)
							itOuter->Acl = itInner->Acl;
						// delete the later one (inner)
						(void)AclList.erase(itInner);
						bRetry = true;
						break;
					}
				}
			}
			if (bRetry)
				break;
		}
	} while (bRetry);
}

bool CECSConnection::IfValidMetadataTag(LPCTSTR pszMDString)
{
	CString sStr(pszMDString);
	if (sStr.FindOneOf(_T("=,")) >= 0)
		return false;
	for (int i=0 ; i<sStr.GetLength() ; i++)
	{
		// if UNICODE, don't allow
		if (sStr[i] > 0x100)
			return false;
		if (iscntrl(sStr[i]))
			return false;
	}
	return true;
}

CECSConnection::CECSConnectionSession::CECSConnectionSession()
	: pValue(NULL)
{}

void CECSConnection::CECSConnectionSession::AllocSession(LPCTSTR pszHost, LPCTSTR pszIP)
{
	CSingleLock lock(&csSessionMap, true);
	ReleaseSession();
	Key.sHostEntry = pszHost;
	Key.sIP = pszIP;
	Key.lKey = 0;
	map<SESSION_MAP_KEY, SESSION_MAP_VALUE>::iterator itMap;
	for (itMap = SessionMap.lower_bound(Key)
		; itMap != SessionMap.end()
		; ++itMap)
	{
		if ((Key.sHostEntry != itMap->first.sHostEntry)
			|| (Key.sIP != itMap->first.sIP))
			break;
		if (!itMap->second.bInUse)
		{
			// claim it for us!
			itMap->second.bInUse = true;
			Key = itMap->first;					// save the key so we can release it
			pValue = &itMap->second;
			return;
		}
	}
	// there wasn't any entry. create one
	Key.lKey = InterlockedIncrement(&lSessionKeyValue);
	if (Key.lKey == 0)
		Key.lKey = InterlockedIncrement(&lSessionKeyValue);
	pair<map<SESSION_MAP_KEY, SESSION_MAP_VALUE>::iterator, bool> ret = SessionMap.insert(make_pair(Key, SESSION_MAP_VALUE()));
	ASSERT(ret.second);
	ret.first->second.bInUse = true;
	Key = ret.first->first;					// save the key so we can release it
	pValue = &ret.first->second;
}

CECSConnection::CECSConnectionSession::~CECSConnectionSession()
{
	ReleaseSession();
	pValue = NULL;
}

void CECSConnection::CECSConnectionSession::ReleaseSession(void) throw()
{
	CSingleLock lock(&csSessionMap, true);
	if (pValue != NULL)
	{
		map<SESSION_MAP_KEY, SESSION_MAP_VALUE>::iterator itMap = SessionMap.find(Key);
		if (itMap != SessionMap.end())
		{
			if (!itMap->second.bInUse)
				ASSERT(itMap->second.bInUse);
			if (itMap->second.bKillWhenDone)
				(void)SessionMap.erase(itMap);
			else
			{
				itMap->second.bInUse = false;
				GetSystemTimeAsFileTime(&itMap->second.ftIdleTime);
			}
		}
	}
	Key.sHostEntry.Empty();
	Key.sIP.Empty();
	Key.lKey = 0;
	pValue = NULL;
}

CString CECSConnection::CECSConnectionSession::Format(void) const
{
	return Key.Format() + _T(": ") + ((pValue != NULL) ? (LPCTSTR)pValue->Format() : _T(""));
}

// invalidate all entries for this host
void CECSConnection::KillHostSessions()
{
	CSingleLock lock(&csSessionMap, true);
	map<SESSION_MAP_KEY, SESSION_MAP_VALUE>::iterator itMap;
	for (itMap = SessionMap.begin()
		; itMap != SessionMap.end()
		;)
	{
		if (!itMap->second.bInUse)
		{
			// not currently in use - delete it
			itMap = SessionMap.erase(itMap);
		}
		else
		{
			itMap->second.bKillWhenDone = true;
			++itMap;
		}
	}
}

void CECSConnection::GarbageCollect()
{
	CSingleLock lock(&csSessionMap, true);
	FILETIME ftNow;
	GetSystemTimeAsFileTime(&ftNow);
	map<SESSION_MAP_KEY, SESSION_MAP_VALUE>::iterator itMap;
	for (itMap = SessionMap.begin()
		; itMap != SessionMap.end()
		;)
	{
		if (!itMap->second.bInUse)
		{
			if (ftNow > (itMap->second.ftIdleTime + FT_MINUTES(2)))
			{
				itMap = SessionMap.erase(itMap);
				continue;
			}
		}
		++itMap;
	}
}

// CECSConnection::ValidateS3BucketName
// returns false if the name does not conform to all the rules for a bucket name
bool CECSConnection::ValidateS3BucketName(LPCTSTR pszBucketName)
{
	CString sBucket(pszBucketName);
	// verify that the bucket name is valid
	//		Bucket names must be at least 3 and no more than 63 characters long.
	//		Bucket names must be a series of one or more labels.Adjacent labels are separated by a single period(.).Bucket names can contain lowercase letters, numbers, and hyphens.Each label must start and end with a lowercase letter or a number.
	//		Bucket names must not be formatted as an IP address(e.g., 192.168.5.4).
	//		Bucket name cannot start with a period(.).
	//		Bucket name cannot end with a period(.).
	//		There can be only one period between labels.
	if ((sBucket.GetLength() < 3)
		|| (sBucket.GetLength() > 63)
		|| (sBucket[0] == L'.')
		|| (sBucket[sBucket.GetLength() - 1] == L'.')
		|| (sBucket.Find(_T("..")) >= 0))
		return false;
	for (int i = 0; i < sBucket.GetLength(); i++)
	{
		TCHAR ch = sBucket[i];
		if ((ch != L'.')
			&& (ch != L'-')
			&& !iswdigit(ch)
			&& !iswlower(ch)
			&& !iswupper(ch)			// relax rules - allow upper case
			&& (ch != L'_'))			// relax rules - allow underscore
			return false;
	}
	// verify that the name doesn't look like an IP address
	// since we already know that there is no '.' at the beginning or end, and there are not 2 dots in a row
	// all we need to do is see if there are 3 total dots
	// and the rest are all numeric
	int iDots = 0;
	bool bAllNum = true;
	for (int i = 0; i < sBucket.GetLength(); i++)
	{
		TCHAR ch = sBucket[i];
		if (ch == L'.')
			iDots++;
		if (!iswdigit(ch))
			bAllNum = true;
	}
	if (bAllNum && (iDots == 3))
		return false;					// looks like nnn.nnn.nnn.nnn, where 'nnn' is any number
	return true;
}

CECSConnection::S3_ERROR CECSConnection::CreateS3Bucket(LPCTSTR pszBucketName)
{
	CBuffer RetData;
	S3_ERROR Error;

	try
	{
		if (!ValidateS3BucketName(pszBucketName))
		{
			Error.S3Error = S3_ERROR_InvalidBucketName;
			Error.dwHttpError = HTTP_STATUS_NOT_FOUND;
			return Error;
		}
		// get the list of buckets. maybe it is already created
		S3_SERVICE_INFO ServiceInfo;
		Error = S3ServiceInformation(ServiceInfo);
		if (Error.IfError())
			return Error;
		for (list<S3_BUCKET_INFO>::const_iterator itList = ServiceInfo.BucketList.begin(); itList != ServiceInfo.BucketList.end(); ++itList)
		{
			if (itList->sName == pszBucketName)
			{
				Error.S3Error = S3_ERROR_BucketAlreadyExists;
				Error.dwHttpError = HTTP_STATUS_NOT_FOUND;
				return Error;
			}
		}
		InitHeader();
		AddHeader(_T("Content-Length"), _T("0"));

		// set location constraint if any other region except for us-east-1
		CAnsiString XmlUTF8;
		if (!sS3Region.IsEmpty() && (sS3Region != _T("us-east-1")))
		{
			// create XML request
			CBufferStream *pBufStream = new CBufferStream;
			CComPtr<IStream> pOutFileStream = pBufStream;
			CComPtr<IXmlWriter> pWriter;

			if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->SetOutput(pOutFileStream)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteStartElement(NULL, L"CreateBucketConfiguration", L"http://s3.amazonaws.com/doc/2006-03-01/")))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"LocationConstraint", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(sS3Region))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndDocument()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->Flush()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
			CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
			CAnsiString XmlUTF8(sXmlOut);
#endif
			XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		}

		Error = SendRequest(_T("PUT"), CString(_T("/")) + pszBucketName + _T("/"), XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}


CECSConnection::S3_ERROR CECSConnection::DeleteS3Bucket(LPCTSTR pszBucketName)
{
	CBuffer RetData;
	S3_ERROR Error;

	try
	{
		InitHeader();
		Error = SendRequest(_T("DELETE"), CString(_T("/")) + pszBucketName + _T("/"), nullptr, 0, RetData);
		if (Error.IfError())
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, Error);
	}
	catch (const CS3ErrorInfo& E)
	{
		return E.Error;
	}
	return Error;
}

// parse response from multipart initiate:
//<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
//  <Bucket>example-bucket</Bucket>
//  <Key>example-object</Key>
//  <UploadId>VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA</UploadId>
//</InitiateMultipartUploadResult>
const WCHAR * const XML_MULTI_PART_BUCKET = L"//InitiateMultipartUploadResult/Bucket";
const WCHAR * const XML_MULTI_PART_KEY = L"//InitiateMultipartUploadResult/Key";
const WCHAR * const XML_MULTI_PART_UPLOAD_ID = L"//InitiateMultipartUploadResult/UploadId";

struct XML_MULTI_PART_CONTEXT
{
	CECSConnection::S3_UPLOAD_PART_INFO *pMultiPartInfo;
};

HRESULT XmlMultiPartCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_MULTI_PART_CONTEXT *pInfo = (XML_MULTI_PART_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (psValue != NULL)
		{
			if (sXmlPath.CompareNoCase(XML_MULTI_PART_BUCKET) == 0)
			{
				pInfo->pMultiPartInfo->sBucket = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_MULTI_PART_KEY) == 0)
			{
				pInfo->pMultiPartInfo->sKey = FROM_UNICODE(*psValue);
			}
			else if (sXmlPath.CompareNoCase(XML_MULTI_PART_UPLOAD_ID) == 0)
			{
				pInfo->pMultiPartInfo->sUploadId = FROM_UNICODE(*psValue);
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

// S3 multipart upload support
CECSConnection::S3_ERROR CECSConnection::S3MultiPartInitiate(LPCTSTR pszPath, S3_UPLOAD_PART_INFO& MultiPartInfo, const list<S3_METADATA_ENTRY> *pMDList)
{
	CECSConnection::S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	MultiPartInfo.sResource = pszPath;
	if (MultiPartInfo.sResource.IsEmpty())
		return ERROR_INVALID_NAME;
	// add in all metadata
	if (pMDList != NULL)
	{
		for (list<S3_METADATA_ENTRY>::const_iterator itList = pMDList->begin(); itList != pMDList->end(); ++itList)
			AddHeader(sAmzMetaPrefix + itList->sTag, itList->sData);
	}
	AddHeader(_T("content-type"), _T("application/octet-stream"));
	Error = SendRequest(_T("POST"), UriEncode(MultiPartInfo.sResource) + _T("?uploads"), NULL, 0, RetData);
	if (Error.IfError())
		return Error;
	// parse XML
	XML_MULTI_PART_CONTEXT Context;
	Context.pMultiPartInfo = &MultiPartInfo;
	HRESULT hr;
	hr = ScanXml(&RetData, &Context, XmlMultiPartCB);
	if (FAILED(hr))
		return hr;
	return ERROR_SUCCESS;
}

struct XML_MPU_COMPLETE_CONTEXT
{
	CECSConnection::S3_MPU_COMPLETE_INFO *pMPUComplete;
	XML_MPU_COMPLETE_CONTEXT()
		: pMPUComplete(nullptr)
	{}
};

const WCHAR * const XML_MPU_COMPLETE_LOCATION = L"//CompleteMultipartUploadResult/Location";
const WCHAR * const XML_MPU_COMPLETE_BUCKET = L"//CompleteMultipartUploadResult/Bucket";
const WCHAR * const XML_MPU_COMPLETE_KEY = L"//CompleteMultipartUploadResult/Key";
const WCHAR * const XML_MPU_COMPLETE_ETAG = L"//CompleteMultipartUploadResult/ETag";

HRESULT XmlMPUCompleteCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_MPU_COMPLETE_CONTEXT *pInfo = (XML_MPU_COMPLETE_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_MPU_COMPLETE_LOCATION) == 0)
				pInfo->pMPUComplete->sLocation = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_MPU_COMPLETE_BUCKET) == 0)
				pInfo->pMPUComplete->sBucket = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_MPU_COMPLETE_KEY) == 0)
				pInfo->pMPUComplete->sKey = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_MPU_COMPLETE_ETAG) == 0)
				pInfo->pMPUComplete->sETag = FROM_UNICODE(*psValue);
		}
		break;

	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::S3MultiPartComplete(
	const S3_UPLOAD_PART_INFO& MultiPartInfo,
	const list<shared_ptr<CECSConnection::S3_UPLOAD_PART_ENTRY>>& PartList,
	S3_MPU_COMPLETE_INFO& MPUCompleteInfo)
{
	CECSConnection::S3_ERROR Error;
	CBuffer RetData;
	try
	{
		// create XML request
		//<CompleteMultipartUpload>
		//  <Part>
		//    <PartNumber>PartNumber</PartNumber>
		//    <ETag>ETag</ETag>
		//  </Part>
		//  ...
		//</CompleteMultipartUpload>
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"CompleteMultipartUpload", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		for (list<shared_ptr<CECSConnection::S3_UPLOAD_PART_ENTRY>>::const_iterator itList = PartList.begin(); itList != PartList.end(); ++itList)
		{
			if (FAILED(pWriter->WriteStartElement(NULL, L"Part", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"PartNumber", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(FmtNum((*itList)->uPartNum)))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"ETag", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE((*itList)->sETag))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		}
		if (FAILED(pWriter->WriteEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));

		InitHeader();
		Error = SendRequest(_T("POST"), UriEncode(MultiPartInfo.sResource) + _T("?uploadId=") + MultiPartInfo.sUploadId, XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData);
		if (!Error.IfError() && !RetData.IsEmpty())
		{
			{
				// even though no error was returned, this is a very special call that may still fail
				XML_ECS_ERROR_CONTEXT Context;
				Context.pAtError = &Error;
				(void)ScanXml(&RetData, &Context, XmlS3ErrorCB);
			}
			if (!Error.sS3Code.IsEmpty())
			{
				Error.dwHttpError = HTTP_STATUS_BAD_REQUEST;			// make sure this is recognized as a failure
			}
			else
			{
				// parse successful XML response
				XML_MPU_COMPLETE_CONTEXT Context;
				Context.pMPUComplete = &MPUCompleteInfo;
				(void)ScanXml(&RetData, &Context, XmlMPUCompleteCB);
			}
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::S3MultiPartAbort(const S3_UPLOAD_PART_INFO& MultiPartInfo)
{
	CECSConnection::S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	Error = SendRequest(_T("DELETE"), UriEncode(MultiPartInfo.sResource) + _T("?uploadId=") + MultiPartInfo.sUploadId, NULL, 0, RetData);
	return Error;
}

const WCHAR * const XML_S3_MULTIPART_LIST_BUCKET =							L"//ListMultipartUploadsResult/Bucket";
const WCHAR * const XML_S3_MULTIPART_LIST_NEXTKEYMARKER=					L"//ListMultipartUploadsResult/NextKeyMarker";
const WCHAR * const XML_S3_MULTIPART_LIST_NEXTUPLOADIDMARKER =				L"//ListMultipartUploadsResult/NextUploadIdMarker";
const WCHAR * const XML_S3_MULTIPART_LIST_MAXUPLOADS =						L"//ListMultipartUploadsResult/MaxUploads";
const WCHAR * const XML_S3_MULTIPART_LIST_ISTRUNCATED =						L"//ListMultipartUploadsResult/IsTruncated";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_ELEMENT =					L"//ListMultipartUploadsResult/Upload";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_KEY =						L"//ListMultipartUploadsResult/Upload/Key";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_UPLOADID =					L"//ListMultipartUploadsResult/Upload/UploadId";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_INITIATOR_ID =				L"//ListMultipartUploadsResult/Upload/Initiator/ID";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_INITIATOR_DISPLAYNAME =	L"//ListMultipartUploadsResult/Upload/Initiator/DisplayName";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_OWNER_ID =					L"//ListMultipartUploadsResult/Upload/Owner/ID";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_OWNER_DISPLAYNAME =		L"//ListMultipartUploadsResult/Upload/Owner/DisplayName";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_STORAGECLASS =				L"//ListMultipartUploadsResult/Upload/StorageClass";
const WCHAR * const XML_S3_MULTIPART_LIST_UPLOAD_INITIATED =				L"//ListMultipartUploadsResult/Upload/Initiated";

struct S3_MULTIPART_LIST_CONTEXT
{
	CECSConnection::S3_LIST_MULTIPART_UPLOADS *pMultiPartList;
	CECSConnection::S3_LIST_MULTIPART_UPLOADS_ENTRY Rec;
	CString sNextKeyMarker;
	CString sNextUploadIdMarker;
	bool bIsTruncated;

	S3_MULTIPART_LIST_CONTEXT()
		: pMultiPartList(NULL)
		, bIsTruncated(false)
	{}
};

HRESULT XmlS3MultiPartListCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	S3_MULTIPART_LIST_CONTEXT *pInfo = (S3_MULTIPART_LIST_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_BUCKET) == 0)
				pInfo->pMultiPartList->sBucket = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_NEXTKEYMARKER) == 0)
				pInfo->sNextKeyMarker = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_NEXTUPLOADIDMARKER) == 0)
				pInfo->sNextUploadIdMarker = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_MAXUPLOADS) == 0)
				pInfo->pMultiPartList->uMaxUploads = _ttoi(FROM_UNICODE(*psValue));
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_ISTRUNCATED) == 0)
				pInfo->bIsTruncated = FROM_UNICODE(*psValue) == _T("true");
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_KEY) == 0)
				pInfo->Rec.sKey = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_UPLOADID) == 0)
				pInfo->Rec.sUploadId = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_INITIATOR_ID) == 0)
				pInfo->Rec.sInitiatorId = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_INITIATOR_DISPLAYNAME) == 0)
				pInfo->Rec.sInitiatorDisplayName = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_OWNER_ID) == 0)
				pInfo->Rec.sOwnerId = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_OWNER_DISPLAYNAME) == 0)
				pInfo->Rec.sOwnerDisplayName = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_STORAGECLASS) == 0)
				pInfo->Rec.sStorageClass = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_INITIATED) == 0)
			{
				CECSConnection::S3_ERROR Error = CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Rec.ftInitiated);
				if (Error.IfError())
					return Error.dwError;
			}
		}
		break;

	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_S3_MULTIPART_LIST_UPLOAD_ELEMENT) == 0)
		{
			if (!pInfo->Rec.sKey.IsEmpty())
			{
				pInfo->pMultiPartList->ObjectList.push_back(pInfo->Rec);
				pInfo->Rec.EmptyRec();
			}
		}
		break;
	default:
		break;
	}
	return 0;
}

// get a list of all multipart uploads currently active
CECSConnection::S3_ERROR CECSConnection::S3MultiPartList(LPCTSTR pszBucketName, S3_LIST_MULTIPART_UPLOADS& MultiPartList)
{
	CECSConnection::S3_ERROR Error;
	S3_MULTIPART_LIST_CONTEXT Context;
	CBuffer RetData;

	Context.pMultiPartList = &MultiPartList;
	InitHeader();
	CString sResource(CString(_T("/")) + pszBucketName + _T("/?uploads&delimiter=/"));
	for (;;)
	{
		CString sTempResource(sResource);
		if (!Context.sNextKeyMarker.IsEmpty())
			sTempResource += _T("&") + UriEncode(Context.sNextKeyMarker, true);
		if (!Context.sNextUploadIdMarker.IsEmpty())
			sTempResource += _T("&") + Context.sNextUploadIdMarker;
		Error = SendRequest(_T("GET"), sTempResource, NULL, 0, RetData);
		if (Error.IfError())
			return Error;
		// parse XML
		HRESULT hr;
		hr = ScanXml(&RetData, &Context, XmlS3MultiPartListCB);
		if (FAILED(hr))
			return hr;
		if (!Context.bIsTruncated)
			break;
	}
	return Error;
}

struct XML_MULTIPART_COPY_CONTEXT
{
	CString sETag;
};

const WCHAR * const XML_MULTIPART_COPY_ETAG = L"//CopyPartResult/ETag";

HRESULT XmlMultipartCopyCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_MULTIPART_COPY_CONTEXT *pInfo = (XML_MULTIPART_COPY_CONTEXT *)pContext;
	if (pInfo == NULL)
		return ERROR_INVALID_DATA;

	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_MULTIPART_COPY_ETAG) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->sETag = FROM_UNICODE(*psValue);
		}
		break;
	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::S3MultiPartUpload(
	const S3_UPLOAD_PART_INFO& MultiPartInfo,		// (in) info from multipart initiate
	S3_UPLOAD_PART_ENTRY& PartEntry,				// (in, out) get part number, set ETag if successful
	STREAM_CONTEXT *pStreamSend,					// (in) context for data (null if copy)
	ULONGLONG ullTotalLen,							// (in) total length of data
	LPCTSTR pszCopySource,							// (in) optional - source object to copy from
	ULONGLONG ullStartRange,						// (in) if pszCopySource, this is the start of the range to copy
	LPCTSTR pszVersionId)							// (in) if pszCopySource, nonNULL: version ID to copy
{
	PartEntry.sETag.Empty();
	CECSConnection::S3_ERROR Error;
	list<HEADER_REQ> Req;
	CBuffer RetData;
	InitHeader();
	Req.emplace_back(_T("ETag"));
	if (!PartEntry.Checksum.IsEmpty())
		AddHeader(_T("Content-MD5"), PartEntry.Checksum.EncodeBase64());
	
	// extract the bucket
	CString sBucket;
	int iSlash = MultiPartInfo.sResource.Find(L'/', 1);
	sBucket = MultiPartInfo.sResource.Mid(1, iSlash - 1);
	CString sResource;
	sResource = L'/' + sBucket + UriEncode(MultiPartInfo.sResource.Mid(iSlash), true);
	sResource += _T("?partNumber=") + FmtNum(PartEntry.uPartNum) + _T("&uploadId=") + MultiPartInfo.sUploadId;
	if (pszCopySource != nullptr)
	{
		CString sSource(UriEncode(pszCopySource));
		if ((pszVersionId != nullptr) && (*pszVersionId != NUL))
			sSource += CString(_T("?versionId=")) + pszVersionId;
		AddHeader(_T("x-amz-copy-source"), sSource);
		AddHeader(_T("x-amz-copy-source-range"), _T("bytes=") + FmtNum(ullStartRange) + _T("-") + FmtNum(ullStartRange + ullTotalLen - 1));
		Error = SendRequest(_T("PUT"), sResource, NULL, 0, RetData, &Req, 0, 0, nullptr, nullptr, 0ULL);
		if (!Error.IfError())
		{
			XML_MULTIPART_COPY_CONTEXT Context;
			HRESULT hr = ScanXml(&RetData, &Context, XmlMultipartCopyCB);
			if (FAILED(hr))
				return hr;
			if (Context.sETag.IsEmpty())
				return ERROR_INVALID_DATA;
			PartEntry.sETag = Context.sETag;
		}
	}
	else
	{
		Error = SendRequest(_T("PUT"), sResource, NULL, 0, RetData, &Req, 0, 0, pStreamSend, NULL, ullTotalLen);
		if (!Error.IfError())
		{
			for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
			{
				if (it->sLabel.CompareNoCase(_T("ETag")) == 0)
				{
					if (it->ContentList.size() != 1)
						return ERROR_INVALID_DATA;
					PartEntry.sETag = it->ContentList.front();
					break;
				}
			}
			if (PartEntry.sETag.IsEmpty())
				return ERROR_INVALID_DATA;
		}
	}
	return Error;
}

struct XML_S3_VERSIONING_CONTEXT
{
	CECSConnection::E_S3_VERSIONING Versioning;
	XML_S3_VERSIONING_CONTEXT()
		: Versioning(CECSConnection::E_S3_VERSIONING::Off)
	{}
};

const WCHAR * const XML_S3_VERSIONING_STATUS = L"//VersioningConfiguration/Status";

HRESULT XmlS3VersioningCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_S3_VERSIONING_CONTEXT *pInfo = (XML_S3_VERSIONING_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_S3_VERSIONING_STATUS) == 0)
			{
				if (psValue->CompareNoCase(L"Suspended") == 0)
					pInfo->Versioning = CECSConnection::E_S3_VERSIONING::Suspended;
				else if (psValue->CompareNoCase(L"Enabled") == 0)
					pInfo->Versioning = CECSConnection::E_S3_VERSIONING::On;
				else
					return ERROR_INVALID_DATA;
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

// S3GetBucketVersioning
// return bucket versioning information
CECSConnection::S3_ERROR CECSConnection::S3GetBucketVersioning(LPCTSTR pszBucket, E_S3_VERSIONING& Versioning)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	Error = SendRequest(_T("GET"), CString(_T("/")) + pszBucket + _T("?versioning"), NULL, 0, RetData, &Req);
	if (Error.IfError())
		return Error;
	if ((RetData.GetBufSize() > 5) && (strncmp((LPCSTR)RetData.GetData(), "<?xml", 5) == 0))
	{
		XML_S3_VERSIONING_CONTEXT Context;
		HRESULT hr;
		hr = ScanXml(&RetData, &Context, XmlS3VersioningCB);
		if (hr != ERROR_SUCCESS)
			return hr;
		Versioning = Context.Versioning;
	}
	else
	{
		// XML doesn't look valid. maybe we are connected to the wrong server?
		// maybe there is a man-in-middle attack?
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_MalformedXML;
		Error.sS3Code = _T("MalformedXML");
		Error.sS3RequestID = _T("GET");
		Error.sS3Resource = _T("/");
	}
	return Error;
}

// S3PutBucketVersioning
// set version state for bucket
CECSConnection::S3_ERROR CECSConnection::S3PutBucketVersioning(LPCTSTR pszBucket, CECSConnection::E_S3_VERSIONING Versioning)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	try
	{
		// create XML request
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"VersioningConfiguration", L"http://s3.amazonaws.com/doc/2006-03-01/")))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"Status", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sStatus;
		if (Versioning == E_S3_VERSIONING::On)
			sStatus = _T("Enabled");
		else
			sStatus = _T("Suspended");
		if (FAILED(pWriter->WriteString(TO_UNICODE(sStatus))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		AddHeader(_T("Content-Type"), _T("application/xml"));
		Error = SendRequest(_T("PUT"), CString(_T("/")) + pszBucket + _T("?versioning"), XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData, &Req);
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}

void CECSConnection::SetHTTPSecurityFlags(DWORD dwHTTPSecurityFlagsParam)
{
	dwHttpSecurityFlags = dwHTTPSecurityFlagsParam;
}

CECSConnection::S3_ERROR CECSConnection::ECSAdminLogin(LPCTSTR pszUser, LPCTSTR pszPassword)
{
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.sHTTPUser = pszUser;
	State.sHTTPPassword = pszPassword;
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	Error = SendRequest(_T("GET"), _T("/login"), NULL, 0, RetData, &Req);
	if (!Error.IfError())
	{
		for (list<HEADER_REQ>::const_iterator it = Req.begin(); it != Req.end(); ++it)
		{
			if ((it->sLabel.CompareNoCase(_T("X-SDS-AUTH-TOKEN")) == 0) && (it->ContentList.size() == 1))
				State.sX_SDS_AUTH_TOKEN = it->ContentList.front();
		}
		if (State.sX_SDS_AUTH_TOKEN.IsEmpty())
			Error.dwError = ERROR_INVALID_DATA;				// didn't get the auth token
	}
	SetPort(wSavePort);
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::ECSAdminLogout()
{
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	AddHeader(_T("X-SDS-AUTH-TOKEN"), State.sX_SDS_AUTH_TOKEN);
	Error = SendRequest(_T("GET"), _T("/logout"), NULL, 0, RetData, &Req);
	if (!Error.IfError())
		State.sX_SDS_AUTH_TOKEN.Empty();
	SetPort(wSavePort);
	return Error;
}

struct XML_ECS_GET_USERS_CONTEXT
{
	CECSConnection::S3_ADMIN_USER_INFO User;
	list<CECSConnection::S3_ADMIN_USER_INFO> *pUserList;
	XML_ECS_GET_USERS_CONTEXT()
		: pUserList(nullptr)
	{}
};

const WCHAR * const XML_ECS_GET_USERS_USERID = L"//users/blobuser/userid";
const WCHAR * const XML_ECS_GET_USERS_NAMESPACE = L"//users/blobuser/namespace";
const WCHAR * const XML_ECS_GET_USERS_ELEMENT = L"//users/blobuser";

HRESULT XmlECSGetUsersCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_ECS_GET_USERS_CONTEXT *pInfo = (XML_ECS_GET_USERS_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_ECS_GET_USERS_USERID) == 0)
		{
			pInfo->User.sUser = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_USERS_NAMESPACE) == 0)
		{
			pInfo->User.sNamespace = FROM_UNICODE(*psValue);
		}
		break;
	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_ECS_GET_USERS_ELEMENT) == 0)
		{
			if (!pInfo->User.IsEmpty())
			{
				pInfo->pUserList->push_back(pInfo->User);
				pInfo->User.EmptyRec();
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::ECSAdminGetUserList(list<S3_ADMIN_USER_INFO>& UserList)
{
	UserList.clear();
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	AddHeader(_T("X-SDS-AUTH-TOKEN"), State.sX_SDS_AUTH_TOKEN);
	Error = SendRequest(_T("GET"), _T("/object/users"), NULL, 0, RetData, &Req);
	SetPort(wSavePort);
	if (!Error.IfError())
	{
		XML_ECS_GET_USERS_CONTEXT Context;
		Context.pUserList = &UserList;
		HRESULT hr;
		hr = ScanXml(&RetData, &Context, XmlECSGetUsersCB);
		if (hr != ERROR_SUCCESS)
			return hr;
	}
	return Error;
}

struct XML_ECS_CREATE_USER_CONTEXT
{
	CECSConnection::S3_ADMIN_USER_INFO User;
};

const WCHAR * const XML_ECS_CREATE_USER_SECRET = L"//user_secret_key/secret_key";
const WCHAR * const XML_ECS_CREATE_USER_KEY_TS = L"//user_secret_key/key_timestamp";
const WCHAR * const XML_ECS_CREATE_USER_KEY_EXPIRY_TS = L"//user_secret_key/key_expiry_timestamp";
const WCHAR * const XML_ECS_CREATE_USER_LINK = L"//user_secret_key/link";

HRESULT XmlECSCreateUserCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_ECS_CREATE_USER_CONTEXT *pInfo = (XML_ECS_CREATE_USER_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_ECS_CREATE_USER_SECRET) == 0)
		{
			pInfo->User.sSecret = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_CREATE_USER_KEY_TS) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->User.ftKeyCreate);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_CREATE_USER_KEY_EXPIRY_TS) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->User.ftKeyExpiry);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_CREATE_USER_LINK) == 0)
		{
			pInfo->User.sLink = FROM_UNICODE(*psValue);
		}
		break;

	default:
		break;
	}
	return 0;
}

// Create ECS User
// User.sUser and sNamespace must be filled in. sTags is optional
CECSConnection::S3_ERROR CECSConnection::ECSAdminCreateUser(S3_ADMIN_USER_INFO& User)
{
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	AddHeader(_T("X-SDS-AUTH-TOKEN"), State.sX_SDS_AUTH_TOKEN);
	AddHeader(_T("Content-Type"), _T("application/xml"));
	try
	{
		// create XML request
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"user_create_param", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"user", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(User.sUser))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"namespace", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(User.sNamespace))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (!User.sTags.IsEmpty())
		{
			if (FAILED(pWriter->WriteStartElement(NULL, L"tags", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(User.sTags))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		}
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		Error = SendRequest(_T("POST"), _T("/object/users"), XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData, &Req);
		SetPort(wSavePort);
		if (!Error.IfError())
		{
			XML_ECS_CREATE_USER_CONTEXT Context;
			Context.User = User;
			HRESULT hr;
			hr = ScanXml(&RetData, &Context, XmlECSCreateUserCB);
			if (hr != ERROR_SUCCESS)
				return hr;
			User = Context.User;
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}

struct XML_ECS_GET_KEYS_FOR_USER_CONTEXT
{
	CECSConnection::S3_ADMIN_USER_KEY_INFO Keys;
};

const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_KEY1 = L"//user_secret_key/secret_key_1";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_TS1 = L"//user_secret_key/key_timestamp_1";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_EXPIRY_TS1 = L"//user_secret_key/key_expiry_timestamp_1";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_KEY2 = L"//user_secret_key/secret_key_2";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_TS2 = L"//user_secret_key/key_timestamp_2";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER_EXPIRY_TS2 = L"//user_secret_key/key_expiry_timestamp_2";
const WCHAR * const XML_ECS_GET_KEYS_FOR_USER = L"//user_secret_key/link";

HRESULT XmlECSGetKeysForUserCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_ECS_GET_KEYS_FOR_USER_CONTEXT *pInfo = (XML_ECS_GET_KEYS_FOR_USER_CONTEXT *)pContext;
	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_KEY1) == 0)
		{
			pInfo->Keys.sSecret1 = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_KEY2) == 0)
		{
			pInfo->Keys.sSecret2 = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_TS1) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Keys.ftKeyCreate1);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_TS2) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Keys.ftKeyCreate2);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_EXPIRY_TS1) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Keys.ftKeyExpiry1);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_GET_KEYS_FOR_USER_EXPIRY_TS2) == 0)
		{
			(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->Keys.ftKeyExpiry2);
		}
		else if (sXmlPath.CompareNoCase(XML_ECS_CREATE_USER_LINK) == 0)
		{
			pInfo->Keys.sLink = FROM_UNICODE(*psValue);
		}
		break;

	default:
		break;
	}
	return 0;
}

// Create ECS User
// User.sUser and sNamespace must be filled in. sTags is optional
CECSConnection::S3_ERROR CECSConnection::ECSAdminGetKeysForUser(LPCTSTR pszUser, LPCTSTR pszNamespace, S3_ADMIN_USER_KEY_INFO& Keys)
{
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	AddHeader(_T("X-SDS-AUTH-TOKEN"), State.sX_SDS_AUTH_TOKEN);
	try
	{
		CString sMethod(_T("/object/user-secret-keys/"));
		sMethod += pszUser;
		if ((pszNamespace != nullptr) && (*pszNamespace != NUL))
			sMethod += CString(_T("/")) + pszNamespace;
		Error = SendRequest(_T("GET"), UriEncode(sMethod), nullptr, 0, RetData, &Req);
		SetPort(wSavePort);
		if (!Error.IfError())
		{
			XML_ECS_GET_KEYS_FOR_USER_CONTEXT Context;
			HRESULT hr;
			hr = ScanXml(&RetData, &Context, XmlECSGetKeysForUserCB);
			if (hr != ERROR_SUCCESS)
				return hr;
			Keys = Context.Keys;
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}

// ECSAdminCreateKeyForUser
// given the user, create a secret key
// User.sUser and sNamespace must be filled in
CECSConnection::S3_ERROR CECSConnection::ECSAdminCreateKeyForUser(S3_ADMIN_USER_INFO& User)
{
	INTERNET_PORT wSavePort = Port;
	SetPort(4443);
	CECSConnectionState& State(GetStateBuf());
	State.dwSecurityFlagsAdd = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	State.dwSecurityFlagsSub = 0;
	CBoolSet TurnOffSignature(&State.bS3Admin);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	State.Headers.clear();
	AddHeader(_T("accept"), _T("*/*"));
	CString sDate(GetCanonicalTime());
	AddHeader(_T("Date"), sDate);
	AddHeader(_T("host"), GetCurrentServerIP());
	AddHeader(_T("X-SDS-AUTH-TOKEN"), State.sX_SDS_AUTH_TOKEN);
	AddHeader(_T("Content-Type"), _T("application/xml"));
	try
	{
		// create XML request
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"user_secret_key_create", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"existing_key_expiry_time_mins", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
//		if (FAILED(pWriter->WriteAttributeString(NULL, _T("null"), NULL, _T("true"))))
//			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(L"2")))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"namespace", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(User.sNamespace))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteStartElement(NULL, L"secretkey", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteString(TO_UNICODE(User.sSecret))))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		Error = SendRequest(_T("POST"), UriEncode(_T("/object/user-secret-keys/") + User.sUser), XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData, &Req);
		SetPort(wSavePort);
		if (!Error.IfError())
		{
			XML_ECS_CREATE_USER_CONTEXT Context;
			Context.User = User;
			HRESULT hr;
			hr = ScanXml(&RetData, &Context, XmlECSCreateUserCB);
			if (hr != ERROR_SUCCESS)
				return hr;
			User = Context.User;
		}
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}

// global initialized flag. must be called to set regular timeouts
void CECSConnection::SetInitialized(void)
{
	bInitialized = true;
}

struct XML_S3_ENDPOINT_INFO_CONTEXT
{
	CECSConnection::S3_ENDPOINT_INFO *pEndpointInfo;
	CString sLastDataNode;
};

const WCHAR * const XML_S3_ENDPOINT_INFO_DATA_NODE = L"//ListDataNode/DataNodes";
const WCHAR * const XML_S3_ENDPOINT_INFO_VERSION = L"//ListDataNode/VersionInfo";

HRESULT XmlS3EndpointInfoCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_S3_ENDPOINT_INFO_CONTEXT *pInfo = (XML_S3_ENDPOINT_INFO_CONTEXT *)pContext;
	if ((pInfo == NULL) || (pInfo->pEndpointInfo == NULL))
		return ERROR_INVALID_DATA;

	switch (NodeType)
	{
	case XmlNodeType_Text:
		if (sXmlPath.CompareNoCase(XML_S3_ENDPOINT_INFO_DATA_NODE) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->sLastDataNode = FROM_UNICODE(*psValue);
		}
		else if (sXmlPath.CompareNoCase(XML_S3_ENDPOINT_INFO_VERSION) == 0)
		{
			if ((psValue != NULL) && !psValue->IsEmpty())
				pInfo->pEndpointInfo->sVersion = FROM_UNICODE(*psValue);
		}
		break;

	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_S3_ENDPOINT_INFO_DATA_NODE) == 0)
		{
			// finished receiving a BUCKET element
			if (!pInfo->sLastDataNode.IsEmpty())
			{
				pInfo->pEndpointInfo->EndpointList.push_back(pInfo->sLastDataNode);
				pInfo->sLastDataNode.Empty();
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::DataNodeEndpointS3(S3_ENDPOINT_INFO& Endpoint)
{
	CECSConnectionState& State(GetStateBuf());
	CBoolSet ECSExtension(&State.bECSExtension);				// set this flag, and reset it on exit
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	Error = SendRequest(_T("GET"), _T("/?endpoint"), NULL, 0, RetData, &Req);
	if (Error.IfError())
		return Error;
	if (!RetData.IsEmpty())
	{
		XML_S3_ENDPOINT_INFO_CONTEXT Context;
		Context.pEndpointInfo = &Endpoint;
		HRESULT hr = ScanXml(&RetData, &Context, XmlS3EndpointInfoCB);
		if (FAILED(hr))
			return hr;
	}
	else
	{
		// XML doesn't look valid. maybe we are connected to the wrong server?
		// maybe there is a man-in-middle attack?
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_MalformedXML;
		Error.sS3Code = _T("MalformedXML");
		Error.sS3RequestID = _T("GET");
		Error.sS3Resource = _T("/");
	}
	return Error;
}

struct XML_S3_LIFECYCLE_INFO_CONTEXT
{
	CECSConnection::S3_LIFECYCLE_INFO *pLifecycleInfo;
	CECSConnection::S3_LIFECYCLE_RULE LastRule;
};

const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE = L"//LifecycleConfiguration/Rule";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_ID = L"//LifecycleConfiguration/Rule/ID";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_PREFIX = L"//LifecycleConfiguration/Rule/Prefix";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_STATUS = L"//LifecycleConfiguration/Rule/Status";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION = L"//LifecycleConfiguration/Rule/Expiration";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION_DAYS = L"//LifecycleConfiguration/Rule/Expiration/Days";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION_DATE = L"//LifecycleConfiguration/Rule/Expiration/Date";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_NONCURRENT_EXPIRATION = L"//LifecycleConfiguration/Rule/NoncurrentVersionExpiration";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_NONCURRENT_EXPIRATION_DAYS = L"//LifecycleConfiguration/Rule/NoncurrentVersionExpiration/NoncurrentDays";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_ABORTUPLOAD = L"//LifecycleConfiguration/Rule/AbortIncompleteMultipartUpload";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_ABORTUPLOAD_DAYS = L"//LifecycleConfiguration/Rule/AbortIncompleteMultipartUpload/DaysAfterInitiation";
const WCHAR * const XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION_EXPIRE_DELETE_MARKER = L"//LifecycleConfiguration/Rule/Expiration/ExpiredObjectDeleteMarker";

HRESULT XmlS3LifecycleInfoCB(const CStringW& sXmlPath, void *pContext, IXmlReader *pReader, XmlNodeType NodeType, const list<XML_LITE_ATTRIB> *pAttrList, const CStringW *psValue)
{
	(void)pReader;
	(void)pAttrList;
	XML_S3_LIFECYCLE_INFO_CONTEXT *pInfo = (XML_S3_LIFECYCLE_INFO_CONTEXT *)pContext;
	if ((pInfo == NULL) || (pInfo->pLifecycleInfo == NULL))
		return ERROR_INVALID_DATA;

	switch (NodeType)
	{
	case XmlNodeType_Text:
		if ((psValue != NULL) && !psValue->IsEmpty())
		{
			if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_ID) == 0)
				pInfo->LastRule.sRuleID = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_PREFIX) == 0)
				pInfo->LastRule.sPath = FROM_UNICODE(*psValue);
			else if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_STATUS) == 0)
				pInfo->LastRule.bEnabled = psValue->CompareNoCase(L"enabled") == 0;
			else if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION_DAYS) == 0)
			{
				pInfo->LastRule.dwDays = _ttoi(FROM_UNICODE(*psValue));
				ZeroFT(pInfo->LastRule.ftDate);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_EXPIRATION_DATE) == 0)
			{
				pInfo->LastRule.dwDays = 0;
				(void)CECSConnection::ParseISO8601Date(FROM_UNICODE(*psValue), pInfo->LastRule.ftDate);
			}
			else if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE_NONCURRENT_EXPIRATION_DAYS) == 0)
			{
				pInfo->LastRule.dwNoncurrentDays = _ttoi(FROM_UNICODE(*psValue));
			}
		}
		break;

	case XmlNodeType_Element:
		if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE) == 0)
		{
			pInfo->LastRule.Empty();
		}
		break;
	case XmlNodeType_EndElement:
		if (sXmlPath.CompareNoCase(XML_S3_LIFECYCLE_INFO_RULE) == 0)
		{
			// finished receiving a lifecycle rule
			if (!pInfo->LastRule.IsEmpty())
			{
				pInfo->pLifecycleInfo->LifecycleRules.push_back(pInfo->LastRule);
				pInfo->LastRule.Empty();
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

CECSConnection::S3_ERROR CECSConnection::S3GetLifecycle(LPCTSTR pszBucket, S3_LIFECYCLE_INFO & Lifecycle)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	CString sResource(CString(_T("/")) + pszBucket + _T("?lifecycle"));
	Error = SendRequest(_T("GET"), sResource, NULL, 0, RetData, &Req);
	if (Error.IfError())
		return Error;
	if (!RetData.IsEmpty())
	{
		XML_S3_LIFECYCLE_INFO_CONTEXT Context;
		Context.pLifecycleInfo = &Lifecycle;
		HRESULT hr = ScanXml(&RetData, &Context, XmlS3LifecycleInfoCB);
		if (FAILED(hr))
			return hr;
	}
	else
	{
		// XML doesn't look valid. maybe we are connected to the wrong server?
		// maybe there is a man-in-middle attack?
		Error.dwHttpError = HTTP_STATUS_SERVER_ERROR;
		Error.S3Error = S3_ERROR_MalformedXML;
		Error.sS3Code = _T("MalformedXML");
		Error.sS3RequestID = _T("GET");
		Error.sS3Resource = _T("/");
	}
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::S3DeleteLifecycle(LPCTSTR pszBucket)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	CString sResource(CString(_T("/")) + pszBucket + _T("?lifecycle"));
	Error = SendRequest(_T("DELETE"), sResource, NULL, 0, RetData, &Req);
	if (Error.IfError())
		return Error;
	return Error;
}

CECSConnection::S3_ERROR CECSConnection::S3PutLifecycle(LPCTSTR pszBucket, const S3_LIFECYCLE_INFO & Lifecycle)
{
	list<HEADER_REQ> Req;
	S3_ERROR Error;
	CBuffer RetData;
	InitHeader();
	try
	{
		// create XML request
		CBufferStream *pBufStream = new CBufferStream;
		CComPtr<IStream> pOutFileStream = pBufStream;
		CComPtr<IXmlWriter> pWriter;

		if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->SetOutput(pOutFileStream)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartDocument(XmlStandalone_Omit)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteStartElement(NULL, L"LifecycleConfiguration", NULL)))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

		for (list<S3_LIFECYCLE_RULE>::const_iterator it = Lifecycle.LifecycleRules.begin(); it != Lifecycle.LifecycleRules.end(); ++it)
		{
			if (FAILED(pWriter->WriteStartElement(NULL, L"Rule", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"ID", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(TO_UNICODE(it->sRuleID))))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"Prefix", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			CString sPath(it->sPath);
			if (!sPath.IsEmpty() && (sPath[sPath.GetLength() - 1] != L'/'))
				sPath += L'/';
			if (FAILED(pWriter->WriteString(TO_UNICODE(sPath))))							// path, if not empty, should always terminate with /
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (FAILED(pWriter->WriteStartElement(NULL, L"Status", NULL)))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteString(it->bEnabled ? L"Enabled" : L"Disabled")))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

			if (!IfFTZero(it->ftDate) || (it->dwDays != 0))
			{
				if (FAILED(pWriter->WriteStartElement(NULL, L"Expiration", NULL)))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
				if (!IfFTZero(it->ftDate))
				{
					// specify a date
					if (FAILED(pWriter->WriteStartElement(NULL, L"Date", NULL)))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
					if (FAILED(pWriter->WriteString(TO_UNICODE(CECSConnection::FormatISO8601Date(it->ftDate, false)))))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
					if (FAILED(pWriter->WriteFullEndElement()))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
				}
				else
				{
					// specify # of days
					if (it->dwDays == 0)
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
					if (FAILED(pWriter->WriteStartElement(NULL, L"Days", NULL)))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
					if (FAILED(pWriter->WriteString(TO_UNICODE(FmtNum(it->dwDays)))))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
					if (FAILED(pWriter->WriteFullEndElement()))
						throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
				}
				if (FAILED(pWriter->WriteFullEndElement()))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			}
			if (it->dwNoncurrentDays != 0)
			{
				// non-current objects
				if (FAILED(pWriter->WriteStartElement(NULL, L"NoncurrentVersionExpiration", NULL)))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

				// specify # of days
				if (FAILED(pWriter->WriteStartElement(NULL, L"NoncurrentDays", NULL)))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
				if (FAILED(pWriter->WriteString(TO_UNICODE(FmtNum(it->dwNoncurrentDays)))))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
				if (FAILED(pWriter->WriteFullEndElement()))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);

				if (FAILED(pWriter->WriteFullEndElement()))
					throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
			}
			// End "Rule" clause
			if (FAILED(pWriter->WriteFullEndElement()))
				throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		}

		if (FAILED(pWriter->WriteFullEndElement()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->WriteEndDocument()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		if (FAILED(pWriter->Flush()))
			throw CS3ErrorInfo(_T(__FILE__), __LINE__, ERROR_XML_PARSE_ERROR);
		CString sXmlOut(pBufStream->GetXml());
#ifdef _UNICODE
		CAnsiString XmlUTF8(sXmlOut, CP_UTF8);
#else
		CAnsiString XmlUTF8(sXmlOut);
#endif
		XmlUTF8.SetBufSize((DWORD)strlen(XmlUTF8));
		CCngAES_GCM HashObj;
		HashObj.CreateHash(BCRYPT_MD5_ALGORITHM);
		HashObj.AddHashData(XmlUTF8);
		CBuffer MD5Hash;
		HashObj.GetHashData(MD5Hash);
		AddHeader(_T("Content-MD5"), MD5Hash.EncodeBase64());
		CString sResource(CString(_T("/")) + pszBucket + _T("/?lifecycle"));
		Error = SendRequest(_T("PUT"), sResource, XmlUTF8.GetData(), XmlUTF8.GetBufSize(), RetData, &Req);
		if (Error.IfError())
			return Error;
	}
	catch (const CS3ErrorInfo& E)
	{
		Error = E.Error;
	}
	return Error;
}
