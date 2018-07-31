#pragma once

#include <7z.h>
#include <7zAlloc.h>
#include <7zBuf.h>
#include <7zFile.h>
#include "PsfArchive.h"

class CPsf7zArchive : public CPsfArchive
{
public:
	CPsf7zArchive();
	virtual ~CPsf7zArchive();

	void Open(const boost::filesystem::path&) override;
	void ReadFileContents(const char*, void*, unsigned int) override;

private:
	CFileInStream m_archiveStream;
	CLookToRead2 m_lookStream;
	CSzArEx m_archive;
	UInt32 m_blockIndex = 0xFFFFFFFF;
	Byte* m_outBuffer = nullptr;
	size_t m_outBufferSize = 0; 
};
