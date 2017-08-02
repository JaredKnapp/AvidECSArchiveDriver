#include "stdafx.h"
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/MemBufFormatTarget.hpp>
#include <xercesc/framework/LocalFileFormatTarget.hpp>
#include <iostream>
#include "XMLDomGenerator.h"

using namespace XERCES_CPP_NAMESPACE;

XMLDomGenerator::XMLDomGenerator(const DETActionData& data)
	:m_Data(data), m_Doc(NULL)
{
}

XMLDomGenerator::~XMLDomGenerator()
{

}

void XMLDomGenerator::writeXMLToString(DOMImplementation *impl, DOMNode* node, CString& xml)
{

	DOMLSSerializer*  pSerializer = ((DOMImplementationLS*)impl)->createLSSerializer();
	DOMLSOutput* theOutputDesc = ((DOMImplementationLS*)impl)->createLSOutput();

	if (NULL != pSerializer)
	{
		MemBufFormatTarget * pFormatTarget = new MemBufFormatTarget();
		if (NULL != pFormatTarget)
		{

			XMLCh* tempStr = XMLString::transcode("UTF-8");
			theOutputDesc->setEncoding(tempStr);
			theOutputDesc->setByteStream(pFormatTarget);

			pSerializer->write(node, theOutputDesc);

			char* pXmlString = (char*)((MemBufFormatTarget*)pFormatTarget)->getRawBuffer();
			xml = pXmlString;

			// (this may not be correct) XMLString::release(&pXmlString);
			delete pFormatTarget;

			//TODO WJK - why can't delete this???
//			delete tempStr;
			tempStr = NULL;
		}

		pSerializer->release();
		pSerializer = NULL;
	}
}

void XMLDomGenerator::writeXMLToFile(DOMImplementation *impl, DOMNode* node, const CString& filename)
{
	USES_CONVERSION;

	DOMLSSerializer*  pSerializer = ((DOMImplementationLS*)impl)->createLSSerializer();
	DOMLSOutput* theOutputDesc = ((DOMImplementationLS*)impl)->createLSOutput();

	if (NULL != pSerializer)
	{
		XMLCh tempStr[1024];
		XMLString::transcode(T2A((LPCTSTR)filename), tempStr, 1024);
		LocalFileFormatTarget * pFormatTarget = new LocalFileFormatTarget(tempStr);
		if (NULL != pFormatTarget)
		{

			XMLCh tempStr[1024];
			XMLString::transcode("UTF-8", tempStr, 1024);
			theOutputDesc->setEncoding(tempStr);
			theOutputDesc->setByteStream(pFormatTarget);

			pSerializer->write(node, theOutputDesc);
			pFormatTarget->flush();
		}

		pSerializer->release();
		pSerializer = NULL;
	}
}



DOMElement* XMLDomGenerator::createNode(DOMNode* root, CString name)
{
	USES_CONVERSION;

	if (m_Doc == NULL) return NULL;

	XMLCh tempStr[1024];
	XMLString::transcode(T2A((LPCTSTR)name), tempStr, 1024);
	DOMElement*   e = m_Doc->createElement(tempStr);
	root->appendChild(e);
	return e;
}


DOMElement* XMLDomGenerator::createNodeValue(DOMNode* root, CString name, CString value)
{
	USES_CONVERSION;

	if (m_Doc == NULL || root == NULL) return NULL;

	DOMElement* e = createNode(root, name);

	XMLCh tempStr[1024];
	XMLString::transcode(T2A((LPCTSTR)value), tempStr, 1024);

	e->setTextContent(tempStr);

	return e;
}

void XMLDomGenerator::createFileElement(DOMNode* fileListNode, const DETActionData::FileStruct& fStruct)
{
	CString buff;
	DOMElement* fileNode = createNode(fileListNode, _T(DET_XML_TAG_FILE));
	CString id = fStruct.MetadataID;
	CString fn = fStruct.FileName;

	createNodeValue(fileNode, _T(DET_XML_TAG_ID), id);
	createNodeValue(fileNode, _T(DET_XML_TAG_FILENAME), fn);

	buff.Format(_T("%d"), (int)fStruct.transferSuccess);
	createNodeValue(fileNode, _T(DET_XML_TAG_TRANSFERSTATUS), buff);

	if (fStruct.segments.size() > 0)
	{
		DOMElement* segmentsNode = createNode(fileNode, _T(DET_XML_TAG_SEGMENTS));
		for (size_t i = 0; i < fStruct.segments.size(); i++)
		{
			DOMElement* segNode = createNode(segmentsNode, _T(DET_XML_TAG_SEGMENT));
			const DETActionData::Segment& s = fStruct.segments[i];

			buff.Format(_T("%I64d"), s.StartOffset);
			createNodeValue(segNode, _T(DET_XML_TAG_STARTOFFSET), buff);

			buff.Format(_T("%I64d"), s.EndOffset);
			createNodeValue(segNode, _T(DET_XML_TAG_ENDOFFSET), buff);

			createNodeValue(segNode, _T(DET_XML_TAG_PARTIALFILENAME), s.partialFn);

			buff.Format(_T("%d"), (int)s.transferSuccess);
			createNodeValue(segNode, _T(DET_XML_TAG_TRANSFERSTATUS), buff);
		}
	}
}

void XMLDomGenerator::createFileList(DOMNode* root)
{
	DOMElement* fileListNode = createNode(root, _T(DET_XML_TAG_FILELIST));
	for (size_t i = 0; i < m_Data.m_FileStructList.size(); i++)
	{
		// create file
		createFileElement(fileListNode, m_Data.m_FileStructList[i]);
	}
}

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


bool XMLDomGenerator::generate(CString& xml)
{
	try
	{
		// get a serializer, an instance of DOMWriter
		XMLCh tempStr[1024];
		XMLString::transcode("LS", tempStr, 1024);
		DOMImplementation *impl = DOMImplementationRegistry::getDOMImplementation(tempStr);

		XMLString::transcode(DET_XML_TAG_DET, tempStr, 1024);
		m_Doc = impl->createDocument(0, tempStr, 0);
		DOMNode* detNode = selectSingleNode(m_Doc, DET_XML_TAG_DET);

		createFileList(detNode);

		writeXMLToString(impl, detNode, xml);
		m_Doc->release();
		return true;
	}
	catch (const XMLException& toCatch) {
		char* message = XMLString::transcode(toCatch.getMessage());
		std::cout << "Exception message is: \n" << message << "\n";
		XMLString::release(&message);
		return false;
	}
	catch (const DOMException& toCatch) {
		char* message = XMLString::transcode(toCatch.msg);
		std::cout << "Exception message is: \n" << message << "\n";
		XMLString::release(&message);
		return false;
	}
	catch (...) {
		std::cout << "Unexpected Exception \n";
		return false;
	}

}
