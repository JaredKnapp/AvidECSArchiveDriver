#include "stdafx.h"
#include "FileSupport.h"
#include "Messages.h"
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
	LOG_DEBUG << pProg->sTitle << L" ProgressCallback, " << pProg->ullOffset;
	//SetStatus(Av::DETEx::keNoError, iProgress);
}

bool DETActionPull::TransferFile(unsigned long index)
{
	DWORD createError;
	bool isOK = false;
	DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];

	if (fileElement.type != "WG4")
	{
		CString sRestoreDir = ParsePath(fileElement.FileName);
		DETAction::CreateDirectories(sRestoreDir);

		CString sArchiveDir = BuildArchiveDir(fileElement.MetadataID);
		CString sSourceFullPath = BuildArchiveFullPath(sArchiveDir, fileElement.FileName);

		if (fileElement.segments.size() > 0)
		{
			// partial restore
			// returns fails if one fails
			fileElement.transferSuccess = true;
			isOK = true;

			DETActionData::SegmentVector_iterator segVectorIter;
			for (segVectorIter = fileElement.segments.begin();
				segVectorIter != fileElement.segments.end();
				segVectorIter++)
			{
				DETActionData::Segment& fileSegment = *segVectorIter;

				CString sDestFullPath = fileSegment.partialFn;
				HANDLE hndDestFile = CreateFile(sDestFullPath, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH), nullptr);
				if (hndDestFile == INVALID_HANDLE_VALUE)
				{
					createError = GetLastError();
					if (createError == 0 || createError == ERROR_FILE_NOT_FOUND)
					{
						hndDestFile = CreateFile(sDestFullPath, GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH), nullptr);
					}
				}

				if (hndDestFile == INVALID_HANDLE_VALUE)
				{
					LOG_ERROR << "Failed to Open for Write on File " << sDestFullPath;

					createError = GetLastError();
					FormatW32ErrorMessage(createError, m_sLastError);
					SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);

					fileElement.transferSuccess = false;
					isOK = false;
				}
				else
				{
					Av::Int64 iFileSize = fileSegment.EndOffset - fileSegment.StartOffset;
					Av::Int64 iBytesRemaining = iFileSize;
					Av::Int64 iCurrentOffset = fileSegment.StartOffset;

					CBuffer RetData;
					RetData.SetBufSize(m_Data.m_lBlockSize);

					do
					{
						DWORD iBytesWritten = 0;
						Av::Int64 iReadSize = min(iBytesRemaining, m_Data.m_lBlockSize);

						CECSConnection::S3_ERROR s3Error = m_ECSConnection.Read(sSourceFullPath, iReadSize, iCurrentOffset, RetData);
						if (s3Error.IfError())
						{
							LOG_ERROR << L"Error from S3Read (" << s3Error.Format() << L")";
							m_sLastError = s3Error.Format();
							SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
							CloseHandle(hndDestFile);
							hndDestFile = INVALID_HANDLE_VALUE;

							fileElement.transferSuccess = false;
							isOK = false;
						}
						else
						{
							BOOL bWriteResult = WriteFile(hndDestFile, RetData.GetData(), iReadSize, &iBytesWritten, NULL);
							if ((bWriteResult == 0) || (iBytesWritten != iReadSize))
							{
								createError = GetLastError();
								FormatW32ErrorMessage(createError, m_sLastError);
								SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);

								CloseHandle(hndDestFile);
								hndDestFile = INVALID_HANDLE_VALUE;

								LOG_ERROR << "Segment Restore FAILED due to " << m_sLastError;
								fileElement.transferSuccess = false;
								isOK = false;
							}
							else
							{
								iCurrentOffset += iBytesWritten;
								iBytesRemaining -= iBytesWritten;
								SetStatus(Av::DETEx::keNoError, iBytesWritten);
							}
						}
					} while (iBytesRemaining > 0 && isOK);

					CloseHandle(hndDestFile);
					hndDestFile = INVALID_HANDLE_VALUE;
				}
			}
		}
		else
		{
			PROGRESS_CONTEXT Context;
			Context.sTitle = L"Pull";

			CString sDestFullPath = fileElement.FileName;
			CHandle hndDestFile(CreateFile(sDestFullPath, FILE_GENERIC_READ | FILE_GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (hndDestFile.m_h != INVALID_HANDLE_VALUE)
			{
				CECSConnection::S3_ERROR s3Error = S3Read(m_ECSConnection, sSourceFullPath, hndDestFile, ProgressCallBack, &Context);
				if (s3Error.IfError())
				{
					LOG_ERROR << L"Error from S3Read (" << s3Error.Format() << L")";
					m_sLastError = s3Error.Format();
					SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
					fileElement.transferSuccess = false;
					isOK = false;
				}
				else {
					SetStatus(Av::DETEx::keNoError, Context.ullOffset);
					fileElement.transferSuccess = true;
					isOK = true;
				}
			}
			else
			{
				//ERROR
				LOG_ERROR << L"Invalid File Handle (" << sDestFullPath << L")";
				m_sLastError = sInvalidFileHandle;
				SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
				fileElement.transferSuccess = false;
				isOK = false;
			}
		}
	}
	else
	{
		LOG_ERROR << L"Transferring WG4 is not supported!! tapename=" << fileElement.tapename << L",archiveid=" << fileElement.archiveID;
		m_sLastError = sUnsupportedFileOperation;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);
		fileElement.transferSuccess = false;
		isOK = false;
	}

	return isOK;
}