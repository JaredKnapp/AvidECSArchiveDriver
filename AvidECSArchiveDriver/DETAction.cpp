#include "stdafx.h"

#include "Messages.h"
#include "DETState.h"
#include "DETAction.h"

CriticalSection DETAction::m_CriticalSection;

DETAction::DETAction() :
	m_pState(NULL),
	m_hndActionThread(NULL),
	m_CntFilesXfer(0),
	m_BytesXferred(0),
	m_TotalBytesToXfer(0)
{
	m_CriticalSection.Enter();

	m_Status.Code = Av::DETEx::keNoError;
	m_Status.State = Av::DETEx::krsUnknown;

	m_Status.TotalKBytesToXfer = 0;
	m_Status.PercentXferComplete = 0;
	m_Status.NumFilesXfer = 0;
	m_Status.KBytesXferred = 0;

	m_CriticalSection.Leave();

	try
	{
		m_pState = new DETState();
		m_pState->SetState(vrsCreate);
	}
	catch (...)
	{
		FILE_LOG(logERROR) << sUnknownException;

		m_sLastError = sUnknownException;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}
}

DETAction::~DETAction() {
	Resume();
	Cancel();
	if (m_pState != NULL)
	{
		delete m_pState;
	}
}

Av::DETEx::eError DETAction::Action(const char * lpXML)
{
	bool StateSet = false;
	DWORD ErrorCode = 0;

	Av::DETEx::eError Error = Av::DETEx::keNoError;

	try
	{
		//first check that we are in the Idle
		//or krsCancel state before we perform
		//the Push
		if (StateSet = m_pState->SetState(vrskrsRun))
		{
//			DETXMLDomParser p(m_Data, DETXML);

		}
	}
	catch (...)
	{
		FILE_LOG(logERROR) << sUnknownException;
		Error = RollbackState(StateSet, sUnknownException);
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}

	return Error;
}

Av::DETEx::eError DETAction::GetDETError(char * lpBuffer, unsigned long * nSize)
{
	return Av::DETEx::eError();
}

Av::DETEx::eError DETAction::GetResult(char * lpBuffer, unsigned long * nSize)
{
	if (lpBuffer == NULL || *nSize <= (unsigned long)m_sResultXML.GetLength() + 1)
	{
		*nSize = (unsigned long)m_sResultXML.GetLength() + 1;
		return Av::DETEx::keBufferTooSmall;
	}
	else
	{
		USES_CONVERSION;
		strcpy(lpBuffer, T2A((LPCTSTR)m_sResultXML));
		return Av::DETEx::keNoError;
	}
}

Av::DETEx::eError DETAction::GetStatus(Av::DETEx::Status * stat)
{
	m_CriticalSection.Enter();
	m_Status.State = m_pState->GetState();
	*stat = m_Status;
	FILE_LOG(logDEBUG) << "GetStatus: m_Status, Code=" << m_Status.Code << ",ErrorType=" << m_Status.ErrorType << ",State=" << m_Status.State << ",TotalKBytesToXfer=" << m_Status.TotalKBytesToXfer << ",KBytesXferred=" << m_Status.KBytesXferred << ",PercentXferComplete=" << m_Status.PercentXferComplete << ")";
	m_CriticalSection.Leave();
	return Av::DETEx::keNoError;
}

Av::DETEx::eError DETAction::Pause()
{
	bool StateSet = false;
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	std::string strMessage;

	if (m_hndActionThread == NULL) return Av::DETEx::keNoTransaction;
	try
	{
		//first check that we are in krsRun
		//state in order to perform a Pause
		if (StateSet = m_pState->SetState(vrskrsPause))
		{
			//Okay to perform pause
			DWORD SuspendStatus = SuspendThread(m_hndActionThread);

			//check that the FM thread has suspended
			if (SuspendStatus == 0xFFFFFFFF)
			{
				Error = RollbackState(StateSet, sTransferNotSuspendedMsg);
			}
			else
			{
				//log that the FM thread has been Suspended
				FILE_LOG(logDEBUG) << sTransferSuspendedMsg << sStateChangeMsg;

			}

			SetStatus(Error);
		}
	}
	catch (...)
	{
		FILE_LOG(logERROR) << sUnknownException;
		Error = RollbackState(StateSet, sUnknownException);
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}

	return Error;
}

Av::DETEx::eError DETAction::Resume()
{
	bool StateSet = false;
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	std::string strMessage;
	if (m_hndActionThread == NULL) return Av::DETEx::keNoTransaction;

	try
	{
		//first check that we are in krsRun
		//state in order to perform a Pause
		if (StateSet = m_pState->SetState(vrskrsRun))
		{
			//Okay to perform resume
			DWORD status = ResumeThread(m_hndActionThread);

			if (status == 0xFFFFFFFF)
			{
				Error = RollbackState(StateSet, sTransferNotResumedMsg + sStateNoChangeMsg);
			}
			else
			{
				FILE_LOG(logDEBUG) << sTransferNotSuspendedMsg << sStateChangeMsg;
			}

			SetStatus(Error);
		}
	}
	catch (...)
	{
		FILE_LOG(logERROR) << sUnknownException;
		Error = RollbackState(StateSet, sUnknownException);
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}

	return Error;
}

Av::DETEx::eError DETAction::Cancel()
{
	bool StateSet = false;
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	std::string strMessage;

	try
	{
		//determine what our actual state is
		intRunState CurrentState = m_pState->GetInternalState();

		if (CurrentState != vrskrsPause)
		{
			if (StateSet = m_pState->SetState(vrskrsCancel))
			{
				FILE_LOG(logDEBUG) << sStateChangeMsg;
			}
			else
			{
				Av::DETEx::eRunState CurDETState = m_pState->GetState();

				if (CurDETState == Av::DETEx::krsCancel)
				{
					Error = Av::DETEx::keCancel;
					FILE_LOG(logDEBUG) << sStateChangeMsg;
				}
				else
				{
					Error = Av::DETEx::keIterationFinished;
					FILE_LOG(logDEBUG) << sCurrentStateMsg << "Finished state";
				}
			}
		}
		else
		{
			//Handle case of suspended FM thread; we will just kill the
			//thread; first resume the thread, and then, send the cancel
			//event
			DWORD ResumeStatus = ResumeThread(m_hndActionThread);

			StateSet = m_pState->SetState(vrskrsCancel);

			if (ResumeStatus != 0xFFFFFFFF)
			{
				//log that the FM thread has resumed
				FILE_LOG(logDEBUG) << sTransferNotSuspendedMsg << sStateChangeMsg;
			}
		}
		SetStatus(Error);
	}
	catch (...)
	{
		FILE_LOG(logDEBUG) << sUnknownException;
		m_sLastError = sUnknownException;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}

	return Error;
}

/***************************************
PRIVATE METHODS
****************************************/
Av::DETEx::eError DETAction::RollbackState(bool& stateSet, CString msg)
{
	m_sLastError = msg;

	if (stateSet)
	{
		m_pState->RollBackState();
		stateSet = false;
	}
	return Av::DETEx::keInternalError;
}

void DETAction::SetStatus(Av::DETEx::eError Code, Av::Int64 FileSize, Av::DETEx::eErrorType ErrType)
{
	FILE_LOG(logDEBUG) << "SetStatue(" << Code << ", " << FileSize << ", " << ErrType << ")";

	m_CriticalSection.Enter();

	m_Status.Code = Code;
	m_Status.ErrorType = ErrType;
	m_Status.State = m_pState->GetState();
	m_Status.TotalKBytesToXfer = m_TotalBytesToXfer;

	if (FileSize != 0)
	{
		m_BytesXferred += FileSize;
		m_Status.KBytesXferred = m_BytesXferred;
	}

	//Infosys - If there is nothing to transfer, percentage is always 100. Added the else if part
	if (m_TotalBytesToXfer > 0)
	{
		m_Status.PercentXferComplete = (int)(m_Status.KBytesXferred * 100 / m_TotalBytesToXfer);
	}
	else if (m_TotalBytesToXfer == 0)
	{
		m_Status.PercentXferComplete = 100;
	}

	m_CriticalSection.Leave();

	FILE_LOG(logDEBUG) << "m_Status, Code=" << m_Status.Code << ",ErrorType=" << m_Status.ErrorType << ",State=" << m_Status.State << ",TotalKBytesToXfer=" << m_Status.TotalKBytesToXfer << ",KBytesXferred=" << m_Status.KBytesXferred << ",PercentXferComplete=" << m_Status.PercentXferComplete << ")";
}

