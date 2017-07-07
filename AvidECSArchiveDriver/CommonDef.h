#pragma once

#include <string>
#include <Av\DETEx.h>

#define DETEXPORT extern "C" __declspec(dllexport)
#define LOGFILENAME "c:\\temp\\AvidECSArchiveDriver.log"
#define IDS_UNKNOWN_ERROR "Unknown error code %08x (%d)"

//Internal state table for DETPush and DETPull.  The interal states are
//used to control what eRunState and eError values are returned to the
//caller on a state transition, as well as, controlling the operation
//DETPush and DETPull are to perform.
//
enum intRunState
{
	vrsCreate = 0,
	vrsIdle = 1,
	vrskrsRun = 2,
	vrskrsCancel = 3,
	vrskrsPause = 4,
	vrsFinish = 5,
	vrsDelete = 6,
	vrsNone = 7

};

const int MaxStates = 8;
const int MaxStateDepth = 4;
const int MappingDepth = 1;

const intRunState StateTbl[MaxStates][MaxStateDepth] = {
	{ vrsIdle, vrskrsRun, vrsNone, vrsNone },				//Create
	{ vrskrsRun, vrsFinish, vrsNone, vrsNone },			//Idle
	{ vrskrsPause, vrskrsCancel, vrsFinish, vrsNone },		//krsRun
	{ vrsFinish, vrsNone, vrsNone, vrsNone },				//krsCancel
	{ vrskrsRun, vrsFinish, vrskrsCancel, vrsNone },		//krsPause
	{ vrsDelete, vrsIdle, vrsNone, vrsNone },				//Finish
	{ vrsNone, vrsNone, vrsNone, vrsNone },				//Delete
	{ vrsCreate, vrsNone, vrsNone, vrsNone }				//None
};


//Internal vendor state mapping to TM eRunState
const Av::DETEx::eRunState StateMapTbl[MaxStates][MappingDepth] = {
	{ Av::DETEx::krsUnknown },	//Create
	{ Av::DETEx::krsIdle },		//Idle
	{ Av::DETEx::krsRun },		//krsRun
	{ Av::DETEx::krsCancel },	//krsCancel
	{ Av::DETEx::krsPause },	//krsPause
	{ Av::DETEx::krsFinish },	//Finish
	{ Av::DETEx::krsUnknown },	//Delete
	{ Av::DETEx::krsUnknown }	//None
};

const CString StateStrings[MaxStates] = {
	_T(" Create State "),		//Create
	_T(" Idle State "),		//Idle
	_T(" krsRun State "),		//krsRun
	_T(" krsCancel State "),	//krsCancel
	_T(" krsPause State "),	//krsPause
	_T(" Finish State "),		//Finish
	_T(" Delete State "),		//Delete
	_T(" No State ")			//None
};

#define DET_XML_TAG_DET "DET"

#define DET_XML_TAG_ARCHIVE_ID "ArchiveID"
#define DET_XML_TAG_BLOCKMOVESIZE "BlockMoveSize"
#define DET_XML_TAG_DESTINATION "StoragePath"
#define DET_XML_TAG_ENDOFFSET "EndOffset"
#define DET_XML_TAG_FILE "File"
#define DET_XML_TAG_FILELIST "Filelist"
#define DET_XML_TAG_FILENAME "Filename"
#define DET_XML_TAG_ID "ID"
#define DET_XML_TAG_METADATA "Metadata"
#define DET_XML_TAG_NAME "Name"
#define DET_XML_TAG_PARTIALFILENAME "PartialFilename"
#define DET_XML_TAG_RESOLUTION "Resolution"
#define DET_XML_TAG_S3BUCKET "Bucket"
#define DET_XML_TAG_S3PORT "Port"
#define DET_XML_TAG_S3SECRET "Secret"
#define DET_XML_TAG_S3URL "Url"
#define DET_XML_TAG_S3USER "User"
#define DET_XML_TAG_SEGMENT "Segment"
#define DET_XML_TAG_SEGMENTS "Segments"
#define DET_XML_TAG_SESSION "SessionID"
#define DET_XML_TAG_STARTOFFSET "StartOffset"
#define DET_XML_TAG_TAPE_NAME "TapeName"
#define DET_XML_TAG_TYPE "Type"
#define DET_XML_TAG_VENDOR "DETVendorParams"

//Common Static Methods

static __int64 myGetFileSize(HANDLE hf)
{
	LARGE_INTEGER li;
	DWORD high;

	li.LowPart = GetFileSize(hf, &high);
	li.HighPart = high;

	return li.QuadPart;
}

static void FormatW32ErrorMessage(DWORD err, CString& sError)
{
	LPTSTR lpMsgBuf;
	if (FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		err,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
		(LPTSTR)&lpMsgBuf,
		0,
		NULL))
	{
		LPTSTR p = _tcsrchr(lpMsgBuf, _T('\r'));
		if (p != NULL) *p = _T('\0');
		sError = lpMsgBuf;
		::LocalFree(lpMsgBuf);
	}
}

