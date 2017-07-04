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
#ifndef _DETBaseEX_H_
#define _DETBaseEX_H_

#include <Av/DETEx.h>

namespace Av
{
	namespace DETEx
	{
		enum ActionType {Push, Pull, Remove, Iterate, Move, QueryByID};
		class DETBaseEx
		{

		protected:
			DETBaseEx()
			{
			}

		public:
			virtual ~DETBaseEx()
			{
			}

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// VersionEx()- Used to obtain DLL version number.
			// Function returns a DWORD containing version info. Version 1.6 would be expressed in hex as 0x00010006
			// The version info can be extracted as follows.
			// DWORD dwVersion = obj->GetVersionDW();	// = 0x00010006
			// DWORD highVersion = dwVersion >> 16;		// = 0x00010006
			// DWORD lowVersion = dwVersion & 0x00FF;	// = 0x00010006
			virtual Av::DETEx::eError VersionEx(long *pwVersion) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// This function does the initialization
			virtual Av::DETEx::eError OpenEx(Av::DETEx::ActionType type) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// Replace DETPush, DETPull, DETIterate, DETREmove methods, with Action method.
			virtual Av::DETEx::eError ActionEx(const char* lpXML) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// This function shuts down this action and no further action can be sent
			virtual Av::DETEx::eError CloseEx() = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// GetResultEx() - Used to obtain result XML(cookie XML) string from DLL after Action has been performed
			// input parameters:
			// lpBuffer - character pointer to buffer which will be used to return the string.
			// nSize - Size in bytes of character buffer. If the request fails. This parameter will be set to the
			// number of bytes required to hold the string.
			// Results:
			// If function succeeds lpBuffer will contain NULL terminated string and nSize will contain number
			// of bytes in the buffer. If function fails because buffer size is too small, the function will return
			// an error, lpBuffer will not be set and nSize will contain required buffer size for string including
			// NULL termination char.
			//
			virtual Av::DETEx::eError GetResultEx(char* lpBuffer, unsigned long* nSize) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// GetLastErrorEx() - Used to obtain error string from DLL after error has occurred.
			// input parameters:
			// lpBuffer - character pointer to buffer which will be used to return error string.
			// nSize - Size in bytes of character buffer. If the request fails. This parameter will be set to the
			// number of bytes required to hold the error string.
			// Results:
			// If function succeeds lpBuffer will contain NULL terminated status message and nSize will contain number
			// of bytes in the buffer. If function fails because buffer size is too small, the function will return
			// an error, lpBuffer will not be set and nSize will contain required buffer size for string including
			// NULL termination char.
			//
			virtual Av::DETEx::eError GetLastErrorEx(char* lpBuffer, unsigned long* nSize) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// GetStatusEx() - Used to obtain status string from DLL.
			//
			virtual Av::DETEx::eError GetStatusEx(Av::DETEx::Status* stat) = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// PauseEx() - Not supported at this time
			virtual Av::DETEx::eError PauseEx() = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// ResumeEx() - Not supported at this time
			virtual Av::DETEx::eError ResumeEx() = 0;

			////////////////////////////////////////////////////////////////////////////////////////////////////
			// CancelEx() - This cancel the current request
			virtual Av::DETEx::eError CancelEx() = 0;
		};

		////////////////////////////////////////////////////////////////////////////////////////////////////
		// DETBaseExCreate() - This is the entry point for the function pointer to access
		// this class and its member functions
		typedef Av::DETEx::DETBaseEx *DETBaseExCreate();

	}
}
#endif // _DETBaseEX_H_