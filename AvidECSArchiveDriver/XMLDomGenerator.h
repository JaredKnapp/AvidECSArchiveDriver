#pragma once

#include <map>
#include "CommonDef.h"
#include "DETActionData.h"

namespace xercesc_3_1
{
	class DOMDocument;
	class DOMNode;
	class DOMElement;
	class DOMImplementation;
}

class XMLDomGenerator
{
public:
	XMLDomGenerator(const DETActionData& data);
	~XMLDomGenerator();

	bool generateFile(const CString& filename);
	bool generate(CString& xml);

private:
	void createFileList(xercesc_3_1::DOMNode* root);
	void createFileElement(xercesc_3_1::DOMNode* fileListNode, const DETActionData::FileStruct& fileStruct);
	void createCookieFilename(xercesc_3_1::DOMNode* root);
	xercesc_3_1::DOMElement* createNodeValue(xercesc_3_1::DOMNode* root, CString name, CString value);
	xercesc_3_1::DOMElement* createNode(xercesc_3_1::DOMNode* root, CString name);
	void writeXMLToFile(xercesc_3_1::DOMImplementation *impl, xercesc_3_1::DOMNode* node, const CString& filename);
	void writeXMLToString(xercesc_3_1::DOMImplementation *impl, xercesc_3_1::DOMNode* node, CString& xml);

private:
	const DETActionData& m_Data;
	xercesc_3_1::DOMDocument* m_Doc;

};