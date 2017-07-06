#pragma once

#include <map>
#include "DETActionData.h"


namespace xercesc_3_1
{
	class DOMNode;
	class DOMNodeList;
}

class XMLDomParser {

public:

	XMLDomParser(DETActionData& data, const char* lpInputXML);
	virtual ~XMLDomParser();

	static void Initialize();
	static void Terminate();

	bool parse();

private:

	void parseFileList(xercesc_3_1::DOMNode* root);
	void parseMetadata(xercesc_3_1::DOMNode* root);
	void parseSession(xercesc_3_1::DOMNode* root);
	void parseVendor(xercesc_3_1::DOMNode* root);

	const char* m_lpInputXML;
	DETActionData& m_Data;

};

