#include "stdafx.h"

#include "XMLDomParser.h"

#include <iostream>

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>

#include "DETActionData.h"

XERCES_CPP_NAMESPACE_USE

#define __MODULE__ "XMLDomParser"

static DOMNodeList* selectNodes(DOMNode* root, const char* tag)
{
	DOMNodeList* nodeList = NULL;

	if (root->getNodeType() == DOMNode::ELEMENT_NODE)
	{
		DOMElement* pmElement = (DOMElement*)root;
		XMLCh* tmp = XMLString::transcode(tag);
		nodeList = pmElement->getElementsByTagName(tmp);
		XMLString::release(&tmp);
	}
	else if (root->getNodeType() == DOMNode::DOCUMENT_NODE)
	{
		xercesc::DOMDocument* doc = (xercesc::DOMDocument*)root;
		XMLCh* tmp = XMLString::transcode(tag);
		nodeList = doc->getElementsByTagName(tmp);
		XMLString::release(&tmp);
	}
	return nodeList;
}

static DOMNode* selectSingleNode(DOMNode* root, const char* tag)
{
	DOMNodeList* nodeList = selectNodes(root, tag);
	if (nodeList != NULL && nodeList->getLength() > 0)
		return nodeList->item(0);
	else
		return NULL;
}

static CString getNodeValue(DOMNode* n)
{
	CString value;
	const XMLCh* name = n->getNodeName();
	const XMLCh* v;

	short type = n->getNodeType();
	if (DOMNode::TEXT_NODE != type)
	{
		DOMNodeList* list = n->getChildNodes();
		if (list && list->getLength() > 0)
		{
			DOMNode* textNode = list->item(0);
			v = textNode->getNodeValue();
		}
		else
			v = n->getTextContent();
	}
	else
		v = n->getNodeValue();
	char* tmp = XMLString::transcode(v);
	value = tmp;
	XMLString::release(&tmp);

	return value;
}

static CString getSingleNodeValue(DOMNode* root, const char* tag)
{
	CString value;

	DOMNode* n = selectSingleNode(root, tag);
	if (n != NULL)
	{
		value = getNodeValue(n);
	}
	return value;
}

XMLDomParser::XMLDomParser(DETActionData& data, const char* lpInputXML)
	:m_Data(data), m_lpInputXML(lpInputXML) {
}

XMLDomParser::~XMLDomParser() {
}

void XMLDomParser::Initialize() {
	try {
		XMLPlatformUtils::Initialize();
	}
	catch (const XMLException& toCatch) {
		char* pMessage = XMLString::transcode(toCatch.getMessage());
		std::cout << "Error during initialization! :\n" << pMessage << "\n";
		XMLString::release(&pMessage);
	}
}

void XMLDomParser::Terminate() {
	try {
		XMLPlatformUtils::Terminate();
	}
	catch (const XMLException& toCatch) {
		char* pMessage = XMLString::transcode(toCatch.getMessage());
		std::cout << "Error during terminate! :\n" << pMessage << "\n";
		XMLString::release(&pMessage);
	}
}

bool XMLDomParser::parse() {
	try
	{
		XercesDOMParser* parser = new XercesDOMParser();
		MemBufInputSource source((const XMLByte*)m_lpInputXML, (unsigned int)strlen(m_lpInputXML), "memory_buffer", false);

		parser->parse(source);
		xercesc::DOMDocument* root = parser->getDocument();
		const XMLCh* rootName = root->getNodeName();
		DOMNode* detNode = selectSingleNode(root, DET_XML_TAG_DET);

		DOMNode* vendorNode = selectSingleNode(root, DET_XML_TAG_VENDOR);
		DOMNode* metadataNode = selectSingleNode(root, DET_XML_TAG_METADATA);
		DOMNode* sessionNode = selectSingleNode(root, DET_XML_TAG_SESSION);
		DOMNode* fileListNode = selectSingleNode(root, DET_XML_TAG_FILELIST);

		if (metadataNode != NULL) parseMetadata(metadataNode);
		if (sessionNode != NULL) parseSession(sessionNode);
		if (vendorNode != NULL) parseVendor(vendorNode);
		if (fileListNode != NULL) parseFileList(fileListNode);

		delete parser;
		return true;
	}
	catch (const XMLException& toCatch) {
		char* message = XMLString::transcode(toCatch.getMessage());
		std::cout << "Exception message is: \n"
			<< message << "\n";
		XMLString::release(&message);
		return false;
	}
	catch (const DOMException& toCatch) {
		char* message = XMLString::transcode(toCatch.msg);
		std::cout << "Exception message is: \n"
			<< message << "\n";
		XMLString::release(&message);
		return false;
	}
	catch (...) {
		std::cout << "Unexpected Exception \n";
		return false;
	}

}

void XMLDomParser::parseFileList(DOMNode * root) 
{
	DOMNodeList* nodes = selectNodes(root, DET_XML_TAG_FILE);

	// loop through all file nodes
	for (XMLSize_t i = 0; i < nodes->getLength(); i++)
	{
		DETActionData::FileStruct FileInfo;
		DOMNode* fileNode = nodes->item(i);

		CString type = getSingleNodeValue(fileNode, DET_XML_TAG_TYPE);
		if (type.Compare(_T("WG4"))==0)
		{
			CString archiveid = getSingleNodeValue(fileNode, DET_XML_TAG_ARCHIVE_ID);
			CString tapename = getSingleNodeValue(fileNode, DET_XML_TAG_TAPE_NAME);

			FILE_LOG(logDEBUG) << "XMLDomParser::parseFileList: " << "Files[" << i << "]=(" << archiveid << ", " << tapename << ")";

			FileInfo.archiveID = archiveid;
			FileInfo.tapename = tapename;
			FileInfo.type = "WG4";
		}
		else
		{
			CString id = getSingleNodeValue(fileNode, DET_XML_TAG_ID);
			CString fn = getSingleNodeValue(fileNode, DET_XML_TAG_FILENAME);
			CString resolution = getSingleNodeValue(fileNode, DET_XML_TAG_RESOLUTION);

			FILE_LOG(logDEBUG) << "XMLDomParser::parseFileList: " << "Files [" << i << "]=(" << id << ", " << fn << ", " << resolution << ")";

			FileInfo.FileName = fn;
			FileInfo.MetadataID = id;
			FileInfo.FileSize = 0;

			parsePartialRestoreSegments(fileNode, FileInfo.segments);
		}
		m_Data.m_FileStructList.push_back(FileInfo);
	}
}

void XMLDomParser::parseMetadata(DOMNode * root) {
	CString sName = getSingleNodeValue(root, DET_XML_TAG_NAME);
	FILE_LOG(logDEBUG) << "Name of the job=" << sName;
}

void XMLDomParser::parseSession(DOMNode * root) {
}

void XMLDomParser::parseVendor(DOMNode * root) {
	CString sBlockMoveSizeStr = getSingleNodeValue(root, DET_XML_TAG_BLOCKMOVESIZE);
	CString sDestination = getSingleNodeValue(root, DET_XML_TAG_DESTINATION);
	CString sS3Url = getSingleNodeValue(root, DET_XML_TAG_S3URL);
	CString sS3Port = getSingleNodeValue(root, DET_XML_TAG_S3PORT);
	CString sS3User = getSingleNodeValue(root, DET_XML_TAG_S3USER);
	CString sS3Secret = getSingleNodeValue(root, DET_XML_TAG_S3SECRET);
	CString sS3Bucket = getSingleNodeValue(root, DET_XML_TAG_S3BUCKET);

	m_Data.m_lBlockSize = _wtol(sBlockMoveSizeStr);
	m_Data.m_sDestination = sDestination;
	m_Data.m_sS3Url = sS3Url;
	m_Data.m_wS3Port = (WORD)_tcstoul(sS3Port, NULL, 10);
	m_Data.m_sS3User = sS3User;
	m_Data.m_sS3Secret = sS3Secret;
	m_Data.m_sS3Bucket = sS3Bucket;
}

void XMLDomParser::parsePartialRestoreSegments(xercesc_3_1::DOMNode * root, DETActionData::SegmentVector & segments)
{
	DOMNode* segmentsNode = selectSingleNode(root, DET_XML_TAG_SEGMENTS);
	if (segmentsNode != NULL)
	{
		DOMNodeList* segmentNodeList = selectNodes(segmentsNode, DET_XML_TAG_SEGMENT);

		for (XMLSize_t index = 0; index < segmentNodeList->getLength(); index++)
		{
			DOMNode* segmentNode = segmentNodeList->item(index);

			CString sStartOffset = getSingleNodeValue(segmentNode, DET_XML_TAG_STARTOFFSET);
			CString sEndOffset = getSingleNodeValue(segmentNode, DET_XML_TAG_ENDOFFSET);
			CString sPartialFileName = getSingleNodeValue(segmentNode, DET_XML_TAG_PARTIALFILENAME);

			DETActionData::Segment seg;

			seg.StartOffset = _tcstoi64(sStartOffset, NULL, 10);
			seg.EndOffset = _tcstoi64(sEndOffset, NULL, 10);
			seg.partialFn = sPartialFileName;

			segments.push_back(seg);
		}
	}
}

