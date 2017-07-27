#include "stdafx.h"
#include "FileSupport.h"
#include "DETActionPush.h"

struct PROGRESS_CONTEXT
{
	CString sTitle;
	ULONGLONG ullOffset;
	PROGRESS_CONTEXT() : ullOffset(0ULL)
	{}
};

static void ProgressCallBack(int iProgress, void *pContext)
{
	PROGRESS_CONTEXT *pProg = (PROGRESS_CONTEXT *)pContext;
	pProg->ullOffset += iProgress;
	LOG_DEBUG << pProg->sTitle << L" ProgressCallback, " << pProg->ullOffset;
}

bool DETActionPush::TransferFile(unsigned long index)
{
	bool isOK = false;

	DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];
	CString sArchiveDir = BuildArchiveDir(fileElement.MetadataID);
	CString sDestFullPath = BuildArchiveFullPath(sArchiveDir, fileElement.FileName);

	//Push to ECS
	CHandle hFile(CreateFile(fileElement.FileName, FILE_GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	LARGE_INTEGER liFileSize;
	Av::Int64 x;
	if (GetFileSizeEx(hFile, &liFileSize))
	{
		PROGRESS_CONTEXT Context;
		Context.sTitle = L"Push";

		list<CECSConnection::S3_METADATA_ENTRY> MDList;

		CECSConnection::S3_METADATA_ENTRY MD_Rec;
		MD_Rec.sTag = L"avid-sourcefilepath";
		MD_Rec.sData = fileElement.FileName;
		MDList.push_back(MD_Rec);

		try {

			CECSConnection::S3_ERROR Error = S3Write(m_ECSConnection, sDestFullPath, hFile, m_Data.m_lBlockSize, true, 20, ProgressCallBack, &Context);
			if (Error.IfError())
			{
				LOG_ERROR << L"Failed to write to Archive: " << (LPCTSTR)Error.Format();
				SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
			}
			else {
				SetStatus(Av::DETEx::keNoError, liFileSize.QuadPart);
				isOK = true;
			}

		}
		catch (...) {
			LOG_ERROR << "Caught Unknown Exception";
			SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
		}
	}
	else
	{
		LOG_ERROR << L"Error Opening file: (" << fileElement.FileName << L")";
		DWORD createError = GetLastError();
		FormatW32ErrorMessage(createError, m_sLastError);
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
	}

	fileElement.transferSuccess = isOK;
	return isOK;
}
