#pragma once

#include <string>
#include "DETState.h"
#include "CommonDef.h"

typedef struct DETActionData
{
	typedef struct {
		CString partialFn;
		Av::Int64 StartOffset;
		Av::Int64 EndOffset;
		bool transferSuccess;
	} Segment;
	typedef std::vector<Segment> SegmentVector;
	typedef std::vector<Segment>::iterator SegmentVector_iterator;

	typedef struct {
		CString MetadataID;
		CString FileName;
		Av::Int64 FileSize;
		bool transferSuccess;

		// wg4
		CString archiveID;
		CString tapename;
		CString type;

		SegmentVector segments;

	} FileStruct;
	typedef std::vector<FileStruct> FileStructVector;

	//XML FileSet
	FileStructVector m_FileStructList;


	//XML Vendor Settings
	CString m_sS3Url;
	WORD m_wS3Port;
	CString m_sS3User;
	CString m_sS3Secret;
	CString m_sS3Bucket;
	long m_lBlockSize;
} DETActionData;
