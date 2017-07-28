#include "stdafx.h"

#include "Messages.h"
#include "DETState.h"
#include "XMLDomGenerator.h"
#include "XMLDomParser.h"
#include "DETAction.h"


CriticalSection DETAction::m_CriticalSection;
bool m_bShuttingDown = false;

DETAction::DETAction() :
	m_pState(NULL),
	m_hndActionThread(NULL),
	m_CntFilesXfer(0),
	m_iBytesXferred(0),
	m_iBytesToXfer(0)
{
	LOG_DEBUG << "Constructor initialized";

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
		LOG_ERROR << sUnknownException;
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

//Called from ActionThread, launched in Action() method
unsigned int DETAction::TransferFiles(void * pDETAction)
{
	CString sError;
	DWORD dwErrorCode = 0;
	DETAction *pAction = static_cast<DETAction*>(pDETAction);
	Av::DETEx::eError Error = Av::DETEx::keNoError;

	//retrieve the number of files to be moved
	long NumElementsToMove = (long)pAction->m_Data.m_FileStructList.size();
	long index = 0;
	for (; index < NumElementsToMove; index++)
	{
		try
		{
			if (pAction->m_pState->IsValid(vrskrsRun))
			{
				//.TransferFile method should be implemented in object served up by the DETActionFactory
				if (!pAction->TransferFile(index))
				{
					pAction->m_pState->SetState(vrsNone);
					LOG_ERROR << "Failed to transfer file and setting state to vrsNone";
					pAction->m_hndActionThread = NULL;
					ExitThread((DWORD)FM_EXITTHREAD);
					break;
				}
				pAction->m_CriticalSection.Enter();
				pAction->m_Status.NumFilesXfer += 1;
				pAction->m_CriticalSection.Leave();
			}
		}
		catch (...)
		{
			//log unknown exception from the wait on events
			pAction->m_pState->SetState(vrsNone);
			LOG_ERROR << sUnknownException;
			pAction->m_hndActionThread = NULL;
			ExitThread((DWORD)FM_EXITTHREAD);
		}
	}

	if (index >= NumElementsToMove)
	{
		pAction->m_sLastError = sTransferSuccessMsg;
		pAction->StoreCookieXML();

		//if we get here, it means we have finished the transfer.
		//Thus, we need to change state to close on push completion
		if (!pAction->m_pState->SetState(vrsFinish))
		{
			//we were not able to change to the finish state
			LOG_ERROR << sStateNoChangeMsg;
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

				//Establish ECS Connection
				bool isSSL = (m_Data.m_wS3Port == 9021 || m_Data.m_wS3Port == 443);

				deque<CString> IPList;
				IPList.push_back(m_Data.m_sS3Url);
				m_ECSConnection.SetIPList(IPList);
				m_ECSConnection.SetS3KeyID(m_Data.m_sS3User);
				m_ECSConnection.SetSecret(m_Data.m_sS3Secret);
				m_ECSConnection.SetSSL(isSSL);
				m_ECSConnection.SetPort(m_Data.m_wS3Port);
				m_ECSConnection.SetHost(L"ECS S3 API");
				m_ECSConnection.SetUserAgent(L"AvidEcsDriver/1.0");

				//Set Global Variable
				m_CriticalSection.Enter();
				m_iBytesToXfer = CalculateTransferSize();
				SetStatus(Av::DETEx::keNoError, 0, Av::DETEx::ketInfo);
				m_CriticalSection.Leave();

				//Okay to perform send and create the thread
				if (m_hndActionThread == NULL)
				{
					m_hndActionThread = (HANDLE)_beginthreadex(NULL, 0, DETAction::TransferFiles, (void *)this, 0, NULL);
				}
				else
				{
					LOG_ERROR << sThreadExistsMsg;
					m_sLastError = sThreadExistsMsg;
					SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
				}
			}
			else
			{
				LOG_ERROR << sXMLParseError;
				m_sLastError = sXMLParseError;
				SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
			}
		}
	}
	catch (...)
	{
		LOG_ERROR << sUnknownException;
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
	LOG_DEBUG << "GetStatus: m_Status, Code=" << m_Status.Code << ",ErrorType=" << m_Status.ErrorType << ",State=" << m_Status.State << ",TotalKBytesToXfer=" << m_Status.TotalKBytesToXfer << ",KBytesXferred=" << m_Status.KBytesXferred << ",PercentXferComplete=" << m_Status.PercentXferComplete << ")";
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
				LOG_DEBUG << sTransferSuspendedMsg << sStateChangeMsg;

			}

			SetStatus(Error);
		}
	}
	catch (...)
	{
		LOG_ERROR << sUnknownException;
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
				LOG_DEBUG << sTransferNotSuspendedMsg << sStateChangeMsg;
			}

			SetStatus(Error);
		}
	}
	catch (...)
	{
		LOG_ERROR << sUnknownException;
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
				LOG_DEBUG << sStateChangeMsg;
			}
			else
			{
				Av::DETEx::eRunState CurDETState = m_pState->GetState();

				if (CurDETState == Av::DETEx::krsCancel)
				{
					Error = Av::DETEx::keCancel;
					LOG_DEBUG << sStateChangeMsg;
				}
				else
				{
					Error = Av::DETEx::keIterationFinished;
					LOG_DEBUG << sCurrentStateMsg << "Finished state";
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
				LOG_DEBUG << sTransferNotSuspendedMsg << sStateChangeMsg;
			}
		}
		SetStatus(Error);
	}
	catch (...)
	{
		LOG_DEBUG << sUnknownException;
		m_sLastError = sUnknownException;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}

	return Error;
}

Av::Int64 DETAction::CalculateTransferSize() {
	//determine the sizes for the individual files to moved and record the values in the FileStruct for each file
	Av::Int64 iBytesToXFer = 0;

	int iNumElements = (int)m_Data.m_FileStructList.size();
	LOG_DEBUG << L"Found " << iNumElements << L" files to process";
	for (int index = 0; index < iNumElements; index++)
	{
		DETActionData::FileStruct& FileElement = m_Data.m_FileStructList[index];
		CString sType = m_Data.m_FileStructList[index].type;
		LOG_DEBUG << L"Type=" << sType;
		if (sType.Compare(L"WG4") != 0)
		{
			CString sSourceFileName = m_Data.m_FileStructList[index].FileName;
			LOG_DEBUG << L"SourceFilename=" << sSourceFileName;

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
					LOG_DEBUG << "SourceFile size=" << FileElement.FileSize;
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
	LOG_DEBUG << "SetStatus(" << Code << ", " << FileSize << ", " << ErrType << ")";

	m_CriticalSection.Enter();

	m_Status.Code = Code;
	m_Status.ErrorType = ErrType;
	m_Status.State = m_pState->GetState();
	m_Status.TotalKBytesToXfer = m_iBytesToXfer;

	if (FileSize != 0)
	{
		m_iBytesXferred += FileSize;
		m_Status.KBytesXferred = m_iBytesXferred;
	}

	//Infosys - If there is nothing to transfer, percentage is always 100. Added the else if part
	if (m_iBytesToXfer > 0)
	{
		m_Status.PercentXferComplete = (int)(m_Status.KBytesXferred * 100 / m_iBytesToXfer);
	}
	else if (m_iBytesToXfer == 0)
	{
		m_Status.PercentXferComplete = 100;
	}

	m_CriticalSection.Leave();

	LOG_DEBUG << "m_Status, Code=" << m_Status.Code << ",ErrorType=" << m_Status.ErrorType << ",State=" << m_Status.State << ",TotalKBytesToXfer=" << m_Status.TotalKBytesToXfer << ",KBytesXferred=" << m_Status.KBytesXferred << ",PercentXferComplete=" << m_Status.PercentXferComplete << ")";
}

void DETAction::StoreCookieXML()
{
	XMLDomGenerator generator(m_Data);
	generator.generate(m_sResultXML);
}

bool DETAction::CreateDirectories(CString sDirectoryPath)
{
	LOG_DEBUG << "CreateDirectories(" << sDirectoryPath << ")";
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
				LOG_DEBUG << "CreateDirectory(" << sDirectoryToCreate << "), Result=" << bResult;
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
			LOG_DEBUG << "CreateDirectory(" << sDirectoryPath << "), Result=" << bResult;
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

CString DETAction::BuildArchiveFullPath(CString sArchiveDir, CString sSourceFullPath)
{
	CString sFullPathFormatted = L"";
	CString sDestFullPath = L"";

	char sSep = (sSourceFullPath.ReverseFind('\\') > 0) ? '\\' : '/';

	//Extract the filename from the source FullPath
	sFullPathFormatted.Format(L"/%s", sSourceFullPath);
	CString sFileName = sFullPathFormatted.Mid(sFullPathFormatted.ReverseFind(sSep) + 1);

	//Combine the DestPath, with the filename
	sDestFullPath.Format(L"%s%s", sArchiveDir, sFileName);

	return sDestFullPath;
}