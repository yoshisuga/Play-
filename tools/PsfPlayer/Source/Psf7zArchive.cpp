#include "Psf7zArchive.h"
#include <7zCrc.h>

#define kInputBufSize ((size_t)1 << 18)

static const ISzAlloc g_allocImp = { &SzAlloc, &SzFree };

static std::string ConvertUtf16ToAnsi(const std::vector<UInt16>& input)
{
	std::string result;
	for(int i = 0; i < input.size(); i++)
	{
		UInt16 inputChar = input[i];
		if(inputChar == 0) break;
		result += static_cast<char>(inputChar);
	}
	return result;
}

static std::string GetSzEntryFileName(CSzArEx* db, int entryIndex)
{
	size_t len = SzArEx_GetFileNameUtf16(db, entryIndex, NULL);
	std::vector<UInt16> wideFileName;
	wideFileName.resize(len);
	SzArEx_GetFileNameUtf16(db, entryIndex, wideFileName.data());
	return ConvertUtf16ToAnsi(wideFileName);
}

CPsf7zArchive::CPsf7zArchive()
{
	CrcGenerateTable();

	//Initialize m_archiveStream
	FileInStream_CreateVTable(&m_archiveStream);
	File_Construct(&m_archiveStream.file);

	//Initialize m_lookStream
	LookToRead2_CreateVTable(&m_lookStream, False);
	m_lookStream.buf = nullptr;

	//Initialize m_archive
	SzArEx_Init(&m_archive);
}

CPsf7zArchive::~CPsf7zArchive()
{
	ISzAlloc_Free(&g_allocImp, m_outBuffer);
	SzArEx_Free(&m_archive, &g_allocImp);
	ISzAlloc_Free(&g_allocImp, m_lookStream.buf);
	File_Close(&m_archiveStream.file);
}

void CPsf7zArchive::Open(const boost::filesystem::path& filePath)
{
	if(InFile_Open(&m_archiveStream.file, filePath.string().c_str()))
	{
		throw std::runtime_error("Failed to open file.");
	}

	m_lookStream.buf = reinterpret_cast<Byte*>(ISzAlloc_Alloc(&g_allocImp, kInputBufSize));
	m_lookStream.bufSize = kInputBufSize;
	m_lookStream.realStream = &m_archiveStream.vt;
	LookToRead2_Init(&m_lookStream);

	SRes res = SzArEx_Open(&m_archive, &m_lookStream.vt, &g_allocImp, &g_allocImp);
	if(res != SZ_OK)
	{
		throw std::runtime_error("Failed to open file.");
	}

	for(int i = 0; i < m_archive.NumFiles; i++)
	{
		unsigned isDir = SzArEx_IsDir(&m_archive, i);
		if(isDir) continue;
		FILEINFO fileInfo;
		fileInfo.name = GetSzEntryFileName(&m_archive, i);
		fileInfo.length = SzArEx_GetFileSize(&m_archive, i);
		m_files.push_back(fileInfo);
	}
}

void CPsf7zArchive::ReadFileContents(const char* fileName, void* buffer, unsigned int bufferLength)
{
	int entryIndex =
		[&] ()
		{
			for(int i = 0; i < m_archive.NumFiles; i++)
			{
				auto entryFileName = GetSzEntryFileName(&m_archive, i);
				if(entryFileName.compare(fileName) == 0) return i;
			}
			return -1;
		}();
	if(entryIndex == -1)
	{
		throw std::runtime_error("Entry not found.");
	}
	size_t offset = 0;
	size_t outSizeProcessed = 0;
	SRes res = SzArEx_Extract(&m_archive, &m_lookStream.vt, entryIndex, &m_blockIndex, &m_outBuffer, &m_outBufferSize, &offset, &outSizeProcessed, &g_allocImp, &g_allocImp);
	if(bufferLength != outSizeProcessed)
	{
		throw std::runtime_error("Entry size mismatch.");
	}
	memcpy(buffer, m_outBuffer + offset, outSizeProcessed);
}
