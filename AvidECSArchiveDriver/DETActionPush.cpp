#include "stdafx.h"

#include "ECSConnection.h"

#include "DETActionPush.h"




bool DETActionPush::TransferFile(unsigned long index)
{
	//Push file to ECS
	CECSConnection::S3_ERROR Error;

	DETActionData::FileStruct& fileElement = m_Data.m_FileStructList[index];
	CString DestPath;
	CString DestDir = LookupDirectoryByID(index);
	DestPath = CreatePath(fileElement.FileName, DestDir);
	CString srcFilePath = fileElement.FileName;

	bool tmp = true;

	//Push to ECS
	//DETAction::CreateDirectories(DestDir);
	//tmp = DETAction::TransferFile(srcFilePath, DestPath, fileElement.FileSize);

	if (tmp) fileElement.transferSuccess = true;
	return tmp;
}
