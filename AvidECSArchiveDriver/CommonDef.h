#pragma once

#include <string>
#include <Av\DETEx.h>

#define DETEXPORT extern "C" __declspec(dllexport)
#define LOGFILENAME "c:\\temp\\AvidECSArchiveDriver.log"

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

#define DET_XML_TAG_BLOCKMOVESIZE "BlockMoveSize"
#define DET_XML_TAG_DESTINATION "StoragePath"
#define DET_XML_TAG_FILELIST "Filelist"
#define DET_XML_TAG_METADATA "Metadata"
#define DET_XML_TAG_NAME "Name"
#define DET_XML_TAG_S3BUCKET "Bucket"
#define DET_XML_TAG_S3SECRET "Secret"
#define DET_XML_TAG_S3URL "Url"
#define DET_XML_TAG_S3USER "User"
#define DET_XML_TAG_SESSION "SessionID"
#define DET_XML_TAG_VENDOR "DETVendorParams"
