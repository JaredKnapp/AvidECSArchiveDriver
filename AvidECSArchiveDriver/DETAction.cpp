#include "stdafx.h"

#include "Messages.h"
#include "DETState.h"
//#include "NTERRTXT.H"
#include "XMLDomGenerator.h"
#include "XMLDomParser.h"
#include "DETAction.h"


CriticalSection DETAction::m_CriticalSection;
bool m_bShuttingDown = false;


int LastIndexOf(const CString& s1, const CString& s2)
{
	int found = -1;
	int next_pos = 0;
	for (;;)
	{
		next_pos = s1.Find(s2, next_pos);
		if (next_pos == -1)
			return found;

		found = next_pos;
	};
}

DETAction::DETAction() :
	m_pState(NULL),
	m_hndActionThread(NULL),
	m_CntFilesXfer(0),
	m_iBytesXferred(0),
	m_iTotalBytesToXfer(0)
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

unsigned int DETAction::TransferFiles(void * pDETAction)
{
	FILE_LOG(logDEBUG) << "DETAction:TransferFile - YOU ARE HERE!!!";

	CString sError;
	DWORD dwErrorCode = 0;
	DETAction *pAction = static_cast<DETAction*>(pDETAction);
	Av::DETEx::eError Error = Av::DETEx::keNoError;

	//Establish ECS Connection
	bool isSSL = (pAction->m_Data.m_wS3Port == 9021 || pAction->m_Data.m_wS3Port == 443);

	deque<CString> IPList;
	IPList.push_back(pAction->m_Data.m_sS3Url);
	pAction->m_ECSConnection.SetIPList(IPList);
	pAction->m_ECSConnection.SetS3KeyID(pAction->m_Data.m_sS3User);
	pAction->m_ECSConnection.SetSecret(pAction->m_Data.m_sS3Secret);
	pAction->m_ECSConnection.SetSSL(isSSL);
	pAction->m_ECSConnection.SetPort(pAction->m_Data.m_wS3Port);
	pAction->m_ECSConnection.SetHost(_T("ECS S3 API"));
	pAction->m_ECSConnection.SetUserAgent(_T("AvidEcsDriver/1.0"));

	//retrieve the number of files to be moved
	long NumElementsToMove = (long)pAction->m_Data.m_FileStructList.size();
	long i = 0;
	for (; i < NumElementsToMove; i++)
	{
		try
		{
			if (pAction->m_pState->IsValid(vrskrsRun))
			{
				if (!pAction->TransferFile(i))
				{
					pAction->m_pState->SetState(vrsNone);
					FILE_LOG(logERROR) << "DETAction:TransferFiles(...) - " << "Failed to transfer file and setting state to vrsNone";
					pAction->m_hndActionThread = NULL;
					ExitThread((DWORD)FM_EXITTHREAD);
					break;
				}
			}
		}
		catch (...)
		{
			//log unknown exception from the wait on events
			pAction->m_pState->SetState(vrsNone);
			FILE_LOG(logERROR) << sUnknownException;
			pAction->m_hndActionThread = NULL;
			ExitThread((DWORD)FM_EXITTHREAD);
		}
	}

	if (i >= NumElementsToMove)
	{
		pAction->m_sLastError = sTransferSuccessMsg;
		pAction->StoreCookieXML();

		//if we get here, it means we have finished the transfer.
		//Thus, we need to change state to close on push completion
		if (!pAction->m_pState->SetState(vrsFinish))
		{
			//we were not able to change to the finish state
			FILE_LOG(logERROR) << sStateNoChangeMsg;
			Error = Av::DETEx::keInternalError;
		}

		pAction->SetStatus(Error);
	}

	return 0;
}

Av::DETEx::eError DETAction::Action(const char * lpXML)
{
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	bool StateSet = false;
	DWORD ErrorCode = 0;

	try
	{
		//first check that we are in the Idle or krsCancel state before we perform the Push
		if (StateSet = m_pState->SetState(vrskrsRun))
		{
			XMLDomParser xmlParser(m_Data, lpXML);
			if (xmlParser.parse()) {
				m_iTotalBytesToXfer = CalculateTransferSize();

				//Okay to perform send and create the thread
				if (m_hndActionThread == NULL)
				{
					m_hndActionThread = (HANDLE)_beginthreadex(NULL, 0, DETAction::TransferFiles, (void *)this, 0, NULL);
				}
				else
				{
					FILE_LOG(logERROR) << sThreadExistsMsg;
					m_sLastError = sThreadExistsMsg;
					SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
				}
			}
			else
			{
				FILE_LOG(logERROR) << sXMLParseError;
				m_sLastError = sXMLParseError;
				SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
			}
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

Av::DETEx::eError DETAction::GetError(char * lpBuffer, unsigned long * nSize)
{
	if (lpBuffer == NULL || *nSize <= m_sLastError.GetLength() + 1)
	{
		*nSize = (unsigned long)m_sLastError.GetLength() + 1;
		return Av::DETEx::keBufferTooSmall;
	}
	else
	{
		USES_CONVERSION;
		strcpy(lpBuffer, T2A((LPCTSTR)m_sLastError));
		return Av::DETEx::keNoError;
	}
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

Av::Int64 DETAction::CalculateTransferSize() {
	//determine the sizes for the individual files to moved and record the values in the FileStruct for each file
	Av::Int64 iBytesToXFer = 0;

	int iNumElements = (int)m_Data.m_FileStructList.size();
	FILE_LOG(logDEBUG) << "DETAction::Action: " << "Found " << iNumElements << " files to process";
	for (int index = 0; index < iNumElements; index++)
	{
		DETActionData::FileStruct& FileElement = m_Data.m_FileStructList[index];
		CString sType = m_Data.m_FileStructList[index].type;
		FILE_LOG(logDEBUG) << "DETAction::Action: " << "Type=" << sType;
		if (sType.Compare(_T("WG4")) != 0)
		{
			CString sSourceFileName = m_Data.m_FileStructList[index].FileName;
			FILE_LOG(logDEBUG) << "DETAction::Action: " << "SourceFilename=" << sSourceFileName;

			//open file to get its handle, and then, get file size
			HANDLE hFile = INVALID_HANDLE_VALUE;
			try
			{
				hFile = CreateFile(sSourceFileName,
					GENERIC_READ, FILE_SHARE_READ, NULL,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
					NULL);

				if (hFile != INVALID_HANDLE_VALUE)
				{
					if (FileElement.segments.size() > 0)
					{
						// partial restore
						DETActionData::SegmentVector_iterator segVectorIter;
						FileElement.FileSize = 0;

						for (segVectorIter = FileElement.segments.begin();
							segVectorIter != FileElement.segments.end();
							segVectorIter++)
						{
							DETActionData::Segment s = *segVectorIter;
							FileElement.FileSize += s.EndOffset - s.StartOffset;
						}
					}
					else
					{
						// full archive / restore
						LARGE_INTEGER liFileSize;
						if (GetFileSizeEx(hFile, &liFileSize))
						{
							FileElement.FileSize = liFileSize.QuadPart;
						}
					}
					FILE_LOG(logDEBUG) << "DETAction::Action: SourceFile size=" << FileElement.FileSize;
					iBytesToXFer += FileElement.FileSize;
					CloseHandle(hFile);
					hFile = INVALID_HANDLE_VALUE;
				}
				else
				{
					FormatW32ErrorMessage(GetLastError(), m_sLastError);
					SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
				}
			}
			catch (...)
			{
				if (hFile != INVALID_HANDLE_VALUE)
				{
					try { CloseHandle(hFile); }
					catch (...) {}
				}
			}
		}
	}
	return iBytesToXFer;
}

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
	m_Status.TotalKBytesToXfer = m_iTotalBytesToXfer;

	if (FileSize != 0)
	{
		m_iBytesXferred += FileSize;
		m_Status.KBytesXferred = m_iBytesXferred;
	}

	//Infosys - If there is nothing to transfer, percentage is always 100. Added the else if part
	if (m_iTotalBytesToXfer > 0)
	{
		m_Status.PercentXferComplete = (int)(m_Status.KBytesXferred * 100 / m_iTotalBytesToXfer);
	}
	else if (m_iTotalBytesToXfer == 0)
	{
		m_Status.PercentXferComplete = 100;
	}

	m_CriticalSection.Leave();

	FILE_LOG(logDEBUG) << "m_Status, Code=" << m_Status.Code << ",ErrorType=" << m_Status.ErrorType << ",State=" << m_Status.State << ",TotalKBytesToXfer=" << m_Status.TotalKBytesToXfer << ",KBytesXferred=" << m_Status.KBytesXferred << ",PercentXferComplete=" << m_Status.PercentXferComplete << ")";
}

void DETAction::StoreCookieXML()
{
	XMLDomGenerator generator(m_Data);
	generator.generate(m_sResultXML);
}

bool DETAction::CreateDirectories(CString sDirectoryPath)
{
	FILE_LOG(logDEBUG) << "DETAction::CreateDirectories(" << sDirectoryPath << ")";
	CString sDirectoryToCreate = sDirectoryPath;
	std::string::size_type pos = 0, slash_pos = 0, tmp_pos = 0;
	BOOL bResult = TRUE;
	bool isFinished = false;
	const int iEndUNCHeader = 4;
	DWORD ulErrCode;

	//first determine if we are drive letter or UNC based
	if ((slash_pos = sDirectoryPath.Find(L":", tmp_pos)) != std::string::npos)
	{
		tmp_pos = slash_pos;

		//adjust slash_pos beyond the first back slash
		slash_pos = sDirectoryPath.Find(L"\\/", tmp_pos);
		tmp_pos = slash_pos + 1;
	}
	else
	{
		//get by volume name
		for (int idx = 0; idx < iEndUNCHeader; idx++)
		{
			slash_pos = sDirectoryPath.Find(L"\\/", tmp_pos);
			tmp_pos = slash_pos + 1;
		}
	}

	do
	{
		//parse input dir path to create each directory in sucession
		if ((slash_pos = sDirectoryPath.Find(L"\\/", tmp_pos)) != std::string::npos)
		{
			//form directory name
			sDirectoryToCreate = sDirectoryPath.Mid(pos, (slash_pos - pos));
			if ((DWORD)-1 == GetFileAttributes(sDirectoryToCreate))
			{
				bResult = CreateDirectory(sDirectoryToCreate, NULL);
				FILE_LOG(logDEBUG) << "DETAction::CreateDirectory(" << sDirectoryToCreate << "), Result=" << bResult;
			}
			else
			{
				bResult = ERROR_ALREADY_EXISTS;
			}

			tmp_pos = slash_pos + 1;
		}
		else
		{
			bResult = CreateDirectory(sDirectoryPath, NULL); // todo: rework this function not to attempt to create directories that already exist.
			FILE_LOG(logDEBUG) << "DETAction::CreateDirectory(" << sDirectoryPath << "), Result=" << bResult;
			isFinished = true;
		}
	} while (!isFinished && (bResult || ((ulErrCode = GetLastError()) == ERROR_ALREADY_EXISTS)));

	return (bResult ? true : false);
}

CString DETAction::BuildArchiveDir(CString sMetadataId)
{
	CString sArchiveDir;
	CString sHashedDir = sMetadataId.Mid(42, 2);

	sArchiveDir.Format(L"/%s/%s/%s/", m_Data.m_sS3Bucket, sHashedDir, sMetadataId);
	return sArchiveDir;
}

CString DETAction::ParsePath(CString sFullPath)
{
	CString sPath = sFullPath.Left(sFullPath.ReverseFind('\\'));
	return sPath;
}

CString DETAction::CreatePath(CString sArchiveDir, CString sSourceFullPath)
{
	CString sFullPathFormatted;
	CString sDestFullPath;

	wchar_t lpSep = (sSourceFullPath.ReverseFind('\\') > 0) ? '\\' : '/';

	//Extract the filename from the source FullPath
	sFullPathFormatted.Format(L"%s%s", lpSep, sSourceFullPath);
	CString sFileName = sFullPathFormatted.Mid(sFullPathFormatted.ReverseFind(lpSep) + 1);

	//Combine the DestPath, with the filename
	sDestFullPath.Format(L"%s/%s", sArchiveDir, sFileName);

	return sDestFullPath;
}
