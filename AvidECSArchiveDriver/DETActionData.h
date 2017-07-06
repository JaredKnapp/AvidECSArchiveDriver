#pragma once

#include <string>
#include "DETState.h"
#include "CommonDef.h"

typedef struct DETActionData
{

	//XML Vendor Settings
	CString m_sDestination;
	CString m_sS3Url;
	CString m_sS3User;
	CString m_sS3Secret;
	CString m_sS3Bucket;

	//XML settings
	bool m_bOverwrite;
	bool m_bIterateOnMob;
	bool m_bDestroy;
	bool m_bDuplicate;
	long m_lBlockSize;
} DETActionData;
