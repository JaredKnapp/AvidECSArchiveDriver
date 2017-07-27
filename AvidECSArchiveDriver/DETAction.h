#pragma once

#include <Av\DETEx.h>
#include "ECSConnection.h"
#include "CriticalSection.h"
#include "DETActionData.h"

class DETAction
{
public:
	~DETAction();
	virtual Av::DETEx::eError Action(const char* lpXML);
	Av::DETEx::eError GetError(char* lpBuffer, unsigned long *nSize);
	Av::DETEx::eError GetResult(char* lpBuffer, unsigned long *nSize);
	Av::DETEx::eError GetStatus(Av::DETEx::Status* stat);
	Av::DETEx::eError Pause();
	Av::DETEx::eError Resume();
	Av::DETEx::eError Cancel();

protected:
	DETAction();

	//dummy placeholder. Should be implemented by push/pull... Action classes
	virtual bool TransferFile(unsigned long index) { return false; }

	void SetStatus(Av::DETEx::eError Code, Av::Int64 FileSize = 0, Av::DETEx::eErrorType ErrType = Av::DETEx::ketSuccess);
	bool DETAction::CreateDirectories(CString sDirectoryPath);
	CString BuildArchiveDir(CString sMetadataId);
	CString BuildArchiveFullPath(CString sArchiveDir, CString sSourceFullPath);
	CString ParsePath(CString sFullPath);
	void StoreCookieXML();
	
	CECSConnection m_ECSConnection;
	DETActionData m_Data;
	DETState *m_pState;

	CString m_sLastError;

	static CriticalSection m_CriticalSection;

private:
	Av::Int64 DETAction::CalculateTransferSize();
	Av::DETEx::eError DETAction::RollbackState(bool& stateSet, CString msg);


	//thread routine called by Send to perform file move
	static unsigned int WINAPI TransferFiles(void *pDETAction);

	int m_CntFilesXfer;
	Av::Int64 m_iBytesToXfer;
	Av::Int64 m_iBytesXferred;

	HANDLE m_hndActionThread;

	Av::DETEx::Status m_Status;
	CString m_sResultXML;
};

