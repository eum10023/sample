//===============================================================================
// Copyright (c) 2018, Optimal Alpha LLC
//===============================================================================

#include "CCircCharBuffer.h"


using namespace Ats::Core;


CCircCharBuffer::CCircCharBuffer(unsigned int iBufferSizeBytes)
: m_iBufferSizeBytes(iBufferSizeBytes) {

	initialize();
}


CCircCharBuffer::~CCircCharBuffer() {

	delete [] m_pszBufferStart;
}


void CCircCharBuffer::initialize() {

	m_pszBufferStart		= new char[m_iBufferSizeBytes];
	m_pszBufferEnd			= m_pszBufferStart + m_iBufferSizeBytes;
	m_pszHead						= m_pszBufferStart;
	m_pszTail						= m_pszBufferStart;
	m_iNumItems					= 0;
}
