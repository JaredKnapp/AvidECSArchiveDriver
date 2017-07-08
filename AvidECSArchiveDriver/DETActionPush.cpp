#include "stdafx.h"

#include "ECSConnection.h"

#include "DETActionPush.h"

bool DETActionPush::TransferFile(unsigned long index)
{
	//Push file to ECS
	CECSConnection Conn;
	CECSConnection::S3_ERROR Error;

	bool isSSL = false;

	deque<CString> IPList;
	IPList.push_back(_T("10.246.27.201"));
	Conn.SetIPList(IPList);
	Conn.SetS3KeyID(_T("avid"));
	Conn.SetSecret(_T("Sb0+RFz1jXu0WR5pYmNV1uE88uCnaGoInn2+40yn"));
	Conn.SetSSL(FALSE);
	Conn.SetPort(9020);
	Conn.SetHost(_T("ECS Community Edition"));
	Conn.SetUserAgent(_T("AvidEcsDriver/1.0"));

	// get the list of buckets
	CECSConnection::S3_SERVICE_INFO ServiceInfo;
	Error = Conn.S3ServiceInformation(ServiceInfo);
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
