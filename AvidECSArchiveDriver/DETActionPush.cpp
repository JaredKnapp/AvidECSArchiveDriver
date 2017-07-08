#include "stdafx.h"

#include "ECSConnection.h"

#include "DETActionPush.h"

bool DETActionPush::TransferFile(unsigned long index)
{
	//Push file to ECS
	CECSConnection::S3_ERROR Error;

	// get the list of buckets
	CECSConnection::S3_SERVICE_INFO ServiceInfo;
	Error = m_ECSConnection.S3ServiceInformation(ServiceInfo);
	if (Error.IfError())
	{
		_tprintf(_T("S3ServiceInformation error: %s\n"), (LPCTSTR)Error.Format());
	}
	else
	{
		// dump service info
		_tprintf(_T("OwnerID: %s, Name: %s\n"), (LPCTSTR)ServiceInfo.sOwnerID, (LPCTSTR)ServiceInfo.sOwnerDisplayName);
		for (list<CECSConnection::S3_BUCKET_INFO>::const_iterator itList = ServiceInfo.BucketList.begin();
			itList != ServiceInfo.BucketList.end();
			++itList)
		{
			_tprintf(_T("  Bucket: %s: %s\n"), (LPCTSTR)itList->sName, (LPCTSTR)DateTimeStr(&itList->ftCreationDate, true, true, true, false, true));
		}
	}

	return true;
}
