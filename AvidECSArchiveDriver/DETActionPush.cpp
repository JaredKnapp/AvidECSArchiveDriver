#include "stdafx.h"

//#include "ECSConnection.h"
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
	//_tprintf(L"%s: %-20I64d\r", (LPCTSTR)pProg->sTitle, pProg->ullOffset);
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

			CECSConnection::S3_ERROR s3Error;

			LOG_DEBUG << fileElement.FileName;
			LOG_DEBUG << sDestFullPath;

			isOK = DoS3MultiPartUpload(
				m_ECSConnection,		// established connection to ECS
				fileElement.FileName,	// path to source file
				sDestFullPath,			// path to object in format: /bucket/dir1/dir2/object
				hFile,					// open handle to file
				liFileSize.QuadPart,	// size of the file
				m_Data.m_lBlockSize,	// size of buffer to use
				10,						// part size (in MB)
				3,						// maxiumum number of threads to spawn
				true,					// if set, include content-MD5 header
				&MDList,				// optional metadata to send to object
				4,						// how big the queue can grow that feeds the upload thread
				5,						// how many times to retry a part before giving up
				ProgressCallBack,		// optional progress callback
				&Context,				// context for ShutdownParamCB and UpdateProgressCB
				s3Error);				// returned error

			if (!isOK) {
				LOG_ERROR << L"Error Pushing to ECS: " << s3Error.Format() << L"-" << s3Error.sDetails;
			}
		}
		catch (...) {
			LOG_ERROR << "Caught Exception";
		}
	}
	else
	{
		LOG_ERROR << L"Error Opening file to Push to ECS: (" << fileElement.FileName << L")";
	}

	fileElement.transferSuccess = isOK;
	return isOK;
}
