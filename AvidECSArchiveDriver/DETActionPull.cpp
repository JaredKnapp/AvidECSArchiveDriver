#include "stdafx.h"

#include "ECSConnection.h"
#include "FileSupport.h"

#include "DETActionPull.h"

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
	_tprintf(L"%s: %-20I64d\r", (LPCTSTR)pProg->sTitle, pProg->ullOffset);
}

bool DETActionPull::TransferFile(unsigned long index)
{
	bool isOK = false;
	DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];

	if (fileElement.type != "WG4")
	{
		CString sRestoreDir = ParsePath(fileElement.FileName);
		DETAction::CreateDirectories(sRestoreDir);

		if (fileElement.segments.size() > 0)
		{
			// partial restore
			// returns fails if one fails
			bool retval = true;
			DETActionData::SegmentVector_iterator segVectorIter;

			for (segVectorIter = fileElement.segments.begin();
				segVectorIter != fileElement.segments.end();
				segVectorIter++)
			{
				DETActionData::Segment& fileSegment = *segVectorIter;
				CString srcFilePath = fileElement.FileName;

				CString DestPath = fileSegment.partialFn;
				Av::Int64 fileSize = fileSegment.EndOffset - fileSegment.StartOffset;

				//if (DETAction::TransferFile(srcFilePath, DestPath, fileSize, s.StartOffset))
				//{
				//	s.transferSuccess = true;
				//}
				//else
				//{
				//	retval = false;
				//}
			}
			return retval;
		}
		else
		{
			CString sArchiveDir = BuildArchiveDir(fileElement.MetadataID);
			CString sSourceFullPath = BuildArchiveFullPath(sArchiveDir, fileElement.FileName);
			CString sDestFullPath = fileElement.FileName;

			//Transfer FROM ECS ============================================
//			bool tmp = DETAction::TransferFile(srcFilePath, DestPath, element.FileSize, 0);
//			if (tmp) element.transferSuccess = true;
//			return tmp;

			PROGRESS_CONTEXT Context;
			Context.sTitle = L"Pull";

			CHandle hFile(CreateFile(sDestFullPath, FILE_GENERIC_READ | FILE_GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (hFile.m_h != INVALID_HANDLE_VALUE)
			{
				CECSConnection::S3_ERROR s3Error = S3Read(m_ECSConnection, sSourceFullPath, hFile, ProgressCallBack, &Context);
				if (s3Error.IfError())
				{
					LOG_ERROR << L"DETActionPull::TransferFile(): " << L"Error from S3Read (" << s3Error.Format() << L")";
					fileElement.transferSuccess = false;
					isOK = false;
				}
				else {
					fileElement.transferSuccess = true;
					isOK = true;
				}
			}
			else
			{
				//ERROR
				LOG_ERROR << L"DETActionPull::TransferFile(): " << L"Invalid File Handle (" << sDestFullPath << L")";
				fileElement.transferSuccess = false;
				isOK = false;
			}
		}
	}
	else
	{
		LOG_ERROR << "Transferring WG4 is not supported!! tapename=" << fileElement.tapename << ",archiveid=" << fileElement.archiveID;
		fileElement.transferSuccess = false;
		isOK = false;
	}

	return isOK;
}