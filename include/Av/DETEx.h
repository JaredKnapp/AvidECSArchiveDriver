//////////////////////////////////////////////////////////////////////
/*
** /------------------------------------------------------------------------\
** | The following programs are the sole property of Avid Technology, Inc.,	|
** | and contain its proprietary and confidential information.				|
** |					Copyright © 2001 Avid Technology, Inc.	       		|
** \------------------------------------------------------------------------/
**
*/
//////////////////////////////////////////////////////////////////////
#ifndef AvDETEx
#define AvDETEx

#include <TCHAR.h>
#include <Av\Int64.h>

namespace Av
{
	namespace DETEx
	{
		enum eError
		{
			keNoError,
			keUnimplemented,
			keCancel,
			keNetwork,
			keInternalError,
			keInvalidXML,
			keDiskFull,
			keNotSuspended,
			keNotRunning,
			keMissingFile,
			keNoTransaction,
			keIterationFinished,
			keSessionFailed,
			kePartialTransfer,
			keVendorFailedToStart,
			keVendorTerminated,
			keBufferTooSmall,
			keNotInitialized,
			keAccessDenied,
			keSharingViolation
		};

		enum eErrorType
		{
			ketSuccess, ketInfo, ketWarning, ketSevere, ketFatal
		};

		enum eRunState
		{
			krsIdle,
			krsRun,
			krsPause,
			krsUnknown,
			krsCancel,
			krsFinish,
			krsNone
		};

		struct Status
		{
			eError Code;
			eErrorType ErrorType;
			eRunState State;
			Av::Int64 TotalKBytesToXfer;
			Av::Int64 KBytesXferred;
			int PercentXferComplete;
			int NumFilesXfer;
		};

#define DET_VENDOR_SUBKEY "Software\\Avid Technology\\DETVendor"
#define VENDOR_DIRECTORY "VendorDirectory"
#define VENDOR_DLL_FILENAME "VendorDll"
#define VENDOR_XML_FILENAME "VendorXML"
#define VENDOR_ARCH "VendorArch"
#define VENDOR_XML_START_TAG "<DETVendorParams "
#define VENDOR_XMK_END_TAG "</DETVendorParams>"
#define VENDOR_XML_MAX_SIZE 10240

	}
}



#endif
