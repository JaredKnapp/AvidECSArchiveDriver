#include "stdafx.h"

#include "Messages.h"
#include "XMLDomParser.h"

#include "DETActionRemove.h"

Av::DETEx::eError DETActionRemove::Action(const char* lpXML)
{
	Av::DETEx::eError Error = Av::DETEx::keNoError;
	try
	{
		XMLDomParser parser(m_Data, lpXML);

		if (parser.parse())
		{
			if (m_pState->SetState(vrskrsRun))
			{
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

				int iNumElements = (int)m_Data.m_FileStructList.size();
				DWORD remainder = 0;

				for (int index = 0; index < iNumElements; index++)
				{
					DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];

					CString sArchiveDir = BuildArchiveDir(fileElement.MetadataID);
					CString sArchiveFullPath = BuildArchiveFullPath(sArchiveDir, fileElement.FileName);

					CECSConnection::S3_ERROR s3Error = m_ECSConnection.DeleteS3(sArchiveFullPath);
					if (s3Error.IfError())
					{
						LOG_ERROR << "Failed to delete file - " << s3Error .Format();

						m_sLastError = s3Error.Format();
						SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketWarning);

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
		LOG_ERROR << sUnknownException;
		m_sLastError = sUnknownException;
		SetStatus(Av::DETEx::keInternalError, 0, Av::DETEx::ketFatal);
	}
	return Error;
}