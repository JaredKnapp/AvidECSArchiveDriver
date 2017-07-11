#include "stdafx.h"

#include "Messages.h"
#include "XMLDomParser.h"

#include "DETActionRemove.h"

Av::DETEx::eError DETActionRemove::Action(const char* lpXML)
{
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	FILE_LOG(logDEBUG) << "DETActionRemove::Action(xml)";

	try
	{
		XMLDomParser parser(m_Data, lpXML);

		if (parser.parse())
		{
			if (!m_pState->SetState(vrskrsRun))
			{
				FILE_LOG(logDEBUG) << "DETActionRemove::Action 1";

				int iNumElements = (int)m_Data.m_FileStructList.size();
				m_iTotalBytesToXfer = 0;
				DWORD remainder = 0;

				for (int index = 0; index < iNumElements; index++)
				{
					DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];

					CString sArchiveDir = BuildArchiveDir(fileElement.MetadataID);
					CString sArchiveFullPath = CreatePath(sArchiveDir, fileElement.FileName);

					CECSConnection::S3_ERROR Error = m_ECSConnection.DeleteS3(sArchiveFullPath);
					if (Error.IfError())
					{
						FILE_LOG(logERROR) << "DETActionRemove::Action(): " << "Failed to delete file - " << Error.Format();
						Error = Av::DETEx::keInternalError;
						fileElement.transferSuccess = FALSE;
					}
					else
					{
						fileElement.transferSuccess = TRUE;
					}
				}

				StoreCookieXML();

				if (!m_pState->SetState(vrsFinish))
				{
					Error = Av::DETEx::keIterationFinished;
				}
			}
			else
			{
				Error = Av::DETEx::keNotRunning;
			}
			SetStatus(Error);
		}
		else
			Error = Av::DETEx::keInvalidXML;
	}
	catch (...)
	{
		FILE_LOG(logERROR) << "DETActionRemove::Action() " << sUnknownException;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}
	return Error;
}