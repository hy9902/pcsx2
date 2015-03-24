/*  PCSX2 - PS2 Emulator for PCs
*  Copyright (C) 2002-2014  PCSX2 Dev Team
*
*  PCSX2 is free software: you can redistribute it and/or modify it under the terms
*  of the GNU Lesser General Public License as published by the Free Software Found-
*  ation, either version 3 of the License, or (at your option) any later version.
*
*  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
*  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
*  PURPOSE.  See the GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License along with PCSX2.
*  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PrecompiledHeader.h"
#include <wx/stdpaths.h>
#include "AppConfig.h"
#include "AsyncFileReader.h"

#include "zlib_indexed.h"

/////////// Some complementary utilities for zlib_indexed.c //////////

#include <fstream>

// This is ugly, but it's hard to find something which will work/compile for both
// windows and *nix and work with non-english file names.
// Maybe some day we'll convert all file related ops to wxWidgets, which means also the
// instances at zlib_indexed.h (which use plain stdio FILE*)
#ifdef WIN32
#   define PX_wfilename(name_wxstr) (WX_STR(name_wxstr))
#   define PX_fopen_rb(name_wxstr) (_wfopen(PX_wfilename(name_wxstr), L"rb"))
#else
#   define PX_wfilename(name_wxstr) (name_wxstr.mbc_str())
#   define PX_fopen_rb(name_wxstr) (fopen(PX_wfilename(name_wxstr), "rb"))
#endif


static s64 fsize(const wxString& filename) {
	if (!wxFileName::FileExists(filename))
		return -1;

	std::ifstream f(PX_wfilename(filename), std::ifstream::binary);
	f.seekg(0, f.end);
	s64 size = f.tellg();
	f.close();

	return size;
}

#define GZIP_ID "PCSX2.index.gzip.v1|"
#define GZIP_ID_LEN (sizeof(GZIP_ID) - 1)	/* sizeof includes the \0 terminator */

// File format is:
// - [GZIP_ID_LEN] GZIP_ID (no \0)
// - [sizeof(Access)] index (should be allocated, contains various sizes)
// - [rest] the indexed data points (should be allocated, index->list should then point to it)
static Access* ReadIndexFromFile(const wxString& filename) {
	s64 size = fsize(filename);
	if (size <= 0) {
		Console.Error(L"Error: Can't open index file: '%s'", WX_STR(filename));
		return 0;
	}
	std::ifstream infile(PX_wfilename(filename), std::ifstream::binary);

	char fileId[GZIP_ID_LEN + 1] = { 0 };
	infile.read(fileId, GZIP_ID_LEN);
	if (wxString::From8BitData(GZIP_ID) != wxString::From8BitData(fileId)) {
		Console.Error(L"Error: Incompatible gzip index, please delete it manually: '%s'", WX_STR(filename));
		infile.close();
		return 0;
	}

	Access* index = (Access*)malloc(sizeof(Access));
	infile.read((char*)index, sizeof(Access));

	s64 datasize = size - GZIP_ID_LEN - sizeof(Access);
	if (datasize != index->have * sizeof(Point)) {
		Console.Error(L"Error: unexpected size of gzip index, please delete it manually: '%s'.", WX_STR(filename));
		infile.close();
		free(index);
		return 0;
	}

	char* buffer = (char*)malloc(datasize);
	infile.read(buffer, datasize);
	infile.close();
	index->list = (Point*)buffer; // adjust list pointer
	return index;
}

static void WriteIndexToFile(Access* index, const wxString filename) {
	if (wxFileName::FileExists(filename)) {
		Console.Warning(L"WARNING: Won't write index - file name exists (please delete it manually): '%s'", WX_STR(filename));
		return;
	}

	std::ofstream outfile(PX_wfilename(filename), std::ofstream::binary);
	outfile.write(GZIP_ID, GZIP_ID_LEN);

	Point* tmp = index->list;
	index->list = 0; // current pointer is useless on disk, normalize it as 0.
	outfile.write((char*)index, sizeof(Access));
	index->list = tmp;

	outfile.write((char*)index->list, sizeof(Point) * index->have);
	outfile.close();

	// Verify
	if (fsize(filename) != (s64)GZIP_ID_LEN + sizeof(Access) + sizeof(Point) * index->have) {
		Console.Warning(L"Warning: Can't write index file to disk: '%s'", WX_STR(filename));
	} else {
		Console.WriteLn(Color_Green, L"OK: Gzip quick access index file saved to disk: '%s'", WX_STR(filename));
	}
}

/////////// End of complementary utilities for zlib_indexed.c //////////
#define CLAMP(val, minval, maxval) (std::min(maxval, std::max(minval, val)))

class ChunksCache {
public:
	ChunksCache(uint initialLimitMb) : m_entries(0), m_size(0), m_limit(initialLimitMb * 1024 * 1024) {};
	~ChunksCache() { Clear(); };
	void SetLimit(uint megabytes);
	void Clear() { MatchLimit(true); };

	void Take(void* pMallocedSrc, PX_off_t offset, int length, int coverage);
	int  Read(void* pDest,        PX_off_t offset, int length);

	static int CopyAvailable(void* pSrc, PX_off_t srcOffset, int srcSize,
							 void* pDst, PX_off_t dstOffset, int maxCopySize) {
		int available = CLAMP(maxCopySize, 0, (int)(srcOffset + srcSize - dstOffset));
		memcpy(pDst, (char*)pSrc + (dstOffset - srcOffset), available);
		return available;
	};

private:
	class CacheEntry {
	public:
		CacheEntry(void* pMallocedSrc, PX_off_t offset, int length, int coverage) :
			data(pMallocedSrc),
			offset(offset),
			coverage(coverage),
			size(length)
		{};

		~CacheEntry() { if (data) free(data); };

		void* data;
		PX_off_t offset;
		int coverage;
		int size;
	};

	std::list<CacheEntry*> m_entries;
	void MatchLimit(bool removeAll = false);
	PX_off_t m_size;
	PX_off_t m_limit;
};

void ChunksCache::SetLimit(uint megabytes) {
	m_limit = (PX_off_t)megabytes * 1024 * 1024;
	MatchLimit();
}

void ChunksCache::MatchLimit(bool removeAll) {
	std::list<CacheEntry*>::reverse_iterator rit;
	while (m_entries.size() && (removeAll || m_size > m_limit)) {
		rit = m_entries.rbegin();
		m_size -= (*rit)->size;
		delete(*rit);
		m_entries.pop_back();
	}
}

void ChunksCache::Take(void* pMallocedSrc, PX_off_t offset, int length, int coverage) {
	m_entries.push_front(new CacheEntry(pMallocedSrc, offset, length, coverage));
	m_size += length;
	MatchLimit();
}

// By design, succeed only if the entire request is in a single cached chunk
int ChunksCache::Read(void* pDest, PX_off_t offset, int length) {
	for (auto it = m_entries.begin(); it != m_entries.end(); it++) {
		CacheEntry* e = *it;
		if (e && offset >= e->offset && (offset + length) <= (e->offset + e->coverage)) {
			if (it != m_entries.begin())
				m_entries.splice(m_entries.begin(), m_entries, it); // Move to top (MRU)
			return CopyAvailable(e->data, e->offset, e->size, pDest, offset, length);
		}
	}
	return -1;
}

static wxString INDEX_TEMPLATE_KEY(L"$(f)");
// template:
// must contain one and only one instance of '$(f)' (without the quotes)
// if if !canEndWithKey -> must not end with $(f)
// if starts with $(f) then it expands to the full path + file name.
// if doesn't start with $(f) then it's expanded to file name only (with extension)
// if doesn't start with $(f) and ends up relative,
//   then it's relative to base (not to cwd)
// No checks are performed if the result file name can be created.
// If this proves useful, we can move it into Path:: . Right now there's no need.
static wxString ApplyTemplate(const wxString &name, const wxDirName &base,
                              const wxString &fileTemplate, const wxString &filename,
                              bool canEndWithKey)
{
	wxString tem(fileTemplate);
	wxString key = INDEX_TEMPLATE_KEY;
	tem = tem.Trim(true).Trim(false); // both sides

	int first = tem.find(key);
	if (first < 0 // not found
	    || first != tem.rfind(key) // more than one instance
	    || !canEndWithKey && first == tem.length() - key.length())
	{
		Console.Error(L"Invalid %s template '%s'.\n"
		              L"Template must contain exactly one '%s' and must not end with it. Abotring.",
		              WX_STR(name), WX_STR(tem), WX_STR(key));
		return L"";
	}

	wxString fname(filename);
	if (first > 0)
		fname = Path::GetFilename(fname); // without path

	tem.Replace(key, fname);
	if (first > 0)
		tem = Path::Combine(base, tem); // ignores appRoot if tem is absolute

	return tem;
}

/*
static void TestTemplate(const wxDirName &base, const wxString &fname, bool canEndWithKey)
{
	const char *ins[] = {
		"$(f).pindex.tmp",                    // same folder as the original file
		"	$(f).pindex.tmp ",                // same folder as the original file (trimmed silently)
		"cache/$(f).pindex",                  // relative to base
		"../$(f).pindex",                     // relative to base
		"%appdata%/pcsx2/cache/$(f).pindex",  // c:/Users/<user>/AppData/Roaming/pcsx2/cache/ ...
		"c:\\pcsx2-cache/$(f).pindex",        // absolute
		"~/.cache/$(f).pindex",	              // TODO: check if this works on *nix. It should...
		                                      //       (on windows ~ isn't recognized as special)
		"cache/$(f)/$(f).index",              // invalid: appears twice
		"hello",                              // invalid: doesn't contain $(f)
		"hello$(f)",                          // invalid, can't end with $(f)
		NULL
	};

	for (int i = 0; ins[i]; i++) {
		wxString tem(wxString::From8BitData(ins[i]));
		Console.WriteLn(Color_Green, L"test: '%s' -> '%s'",
		                WX_STR(tem),
		                WX_STR(ApplyTemplate(L"test", base, tem, fname, canEndWithKey)));
	}
}
*/

static wxString iso2indexname(const wxString& isoname) {
	//testTemplate(isoname);
	wxDirName appRoot = // TODO: have only one of this in PCSX2. Right now have few...
	    (wxDirName)(wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath());
	//TestTemplate(appRoot, isoname, false);
	return ApplyTemplate(L"gzip index", appRoot, g_Conf->GzipIsoIndexTemplate, isoname, false);
}

#define SPAN_DEFAULT (1048576L * 4)   /* distance between direct access points when creating a new index */
#define READ_CHUNK_SIZE (256 * 1024)  /* zlib extraction chunks size (at 0-based boundaries) */
#define CACHE_SIZE_MB 200             /* cache size for extracted data. must be at least READ_CHUNK_SIZE (in MB)*/

class GzippedFileReader : public AsyncFileReader
{
	DeclareNoncopyableObject(GzippedFileReader);
public:
	GzippedFileReader(void) :
		m_pIndex(0),
		m_zstates(0),
		m_src(0),
		m_cache(CACHE_SIZE_MB) {
		m_blocksize = 2048;
		AsyncPrefetchReset();
	};

	virtual ~GzippedFileReader(void) { Close(); };

	static  bool CanHandle(const wxString& fileName);
	virtual bool Open(const wxString& fileName);

	virtual int ReadSync(void* pBuffer, uint sector, uint count);

	virtual void BeginRead(void* pBuffer, uint sector, uint count);
	virtual int FinishRead(void);
	virtual void CancelRead(void) {};

	virtual void Close(void);

	virtual uint GetBlockCount(void) const {
		// type and formula copied from FlatFileReader
		// FIXME? : Shouldn't it be uint and (size - m_dataoffset) / m_blocksize ?
		return (int)((m_pIndex ? m_pIndex->uncompressed_size : 0) / m_blocksize);
	};

	virtual void SetBlockSize(uint bytes) { m_blocksize = bytes; }
	virtual void SetDataOffset(int bytes) { m_dataoffset = bytes; }
private:
	class Czstate {
	public:
		Czstate() { state.isValid = 0; };
		~Czstate() { Kill(); };
		void Kill() {
			if (state.isValid)
				inflateEnd(&state.strm);
			state.isValid = 0;
		}
		Zstate state;
	};

	bool	OkIndex();  // Verifies that we have an index, or try to create one
	PX_off_t GetOptimalExtractionStart(PX_off_t offset);
	int     _ReadSync(void* pBuffer, PX_off_t offset, uint bytesToRead);
	void	InitZstates();

	int		mBytesRead; // Temp sync read result when simulating async read
	Access* m_pIndex;   // Quick access index
	Czstate* m_zstates;
	FILE*	m_src;

	ChunksCache m_cache;

#ifdef WIN32
	// Used by async prefetch
	HANDLE hOverlappedFile;
	OVERLAPPED asyncOperationContext;
	bool asyncInProgress;
	byte mDummyAsyncPrefetchTarget[READ_CHUNK_SIZE];
#endif

	void AsyncPrefetchReset();
	void AsyncPrefetchOpen();
	void AsyncPrefetchClose();
	void AsyncPrefetchChunk(PX_off_t dummy);
	void AsyncPrefetchCancel();
};

void GzippedFileReader::InitZstates() {
	if (m_zstates) {
		delete[] m_zstates;
		m_zstates = 0;
	}
	if (!m_pIndex)
		return;

	// having another extra element helps avoiding logic for last (so 2+ instead of 1+)
	int size = 2 + m_pIndex->uncompressed_size / m_pIndex->span;
	m_zstates = new Czstate[size]();
}

#ifndef WIN32
void GzippedFileReader::AsyncPrefetchReset() {};
void GzippedFileReader::AsyncPrefetchOpen() {};
void GzippedFileReader::AsyncPrefetchClose() {};
void GzippedFileReader::AsyncPrefetchChunk(PX_off_t dummy) {};
void GzippedFileReader::AsyncPrefetchCancel() {};
#else
// AsyncPrefetch works as follows:
// ater extracting a chunk from the compressed file, ask the OS to asynchronously
// read the next chunk from the file, and then completely ignore the result and
// cancel the async read before the next extract. the next extract then reads the
// data from the disk buf if it's overlapping/contained within the chunk we've
// asked the OS to prefetch, then the OS is likely to already have it cached.
// This procedure is frequently able to overcome seek time due to fragmentation of the
// compressed file on disk without any meaningful penalty.
// This system is only enabled for win32 where we have this async read request.
void GzippedFileReader::AsyncPrefetchReset() {
	hOverlappedFile = INVALID_HANDLE_VALUE;
	asyncInProgress = false;
}

void GzippedFileReader::AsyncPrefetchOpen() {
	hOverlappedFile = CreateFile(
		m_filename,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
		NULL);
};

void GzippedFileReader::AsyncPrefetchClose()
{
	AsyncPrefetchCancel();

	if (hOverlappedFile != INVALID_HANDLE_VALUE)
		CloseHandle(hOverlappedFile);

	AsyncPrefetchReset();
};

void GzippedFileReader::AsyncPrefetchChunk(PX_off_t start)
{
	if (hOverlappedFile == INVALID_HANDLE_VALUE || asyncInProgress) {
		Console.Warning(L"Unexpected file handle or progress state. Aborting prefetch.");
		return;
	}

	LARGE_INTEGER offset;
	offset.QuadPart = start;

	DWORD bytesToRead = READ_CHUNK_SIZE;

	ZeroMemory(&asyncOperationContext, sizeof(asyncOperationContext));
	asyncOperationContext.hEvent = 0;
	asyncOperationContext.Offset = offset.LowPart;
	asyncOperationContext.OffsetHigh = offset.HighPart;

	ReadFile(hOverlappedFile, mDummyAsyncPrefetchTarget, bytesToRead, NULL, &asyncOperationContext);
	asyncInProgress = true;
};

void GzippedFileReader::AsyncPrefetchCancel()
{
	if (!asyncInProgress)
		return;

	if (!CancelIo(hOverlappedFile)) {
		Console.Warning("canceling gz prefetch failed. following prefetching will not work.");
		return;
	}

	asyncInProgress = false;
};
#endif /* WIN32 */

// TODO: do better than just checking existance and extension
bool GzippedFileReader::CanHandle(const wxString& fileName) {
	return wxFileName::FileExists(fileName) && fileName.Lower().EndsWith(L".gz");
}

bool GzippedFileReader::OkIndex() {
	if (m_pIndex)
		return true;

	// Try to read index from disk
	wxString indexfile = iso2indexname(m_filename);
	if (indexfile.length() == 0)
		return false; // iso2indexname(...) will print errors if it can't apply the template

	if (wxFileName::FileExists(indexfile) && (m_pIndex = ReadIndexFromFile(indexfile))) {
		Console.WriteLn(Color_Green, L"OK: Gzip quick access index read from disk: '%s'", WX_STR(indexfile));
		if (m_pIndex->span != SPAN_DEFAULT) {
			Console.Warning(L"Note: This index has %1.1f MB intervals, while the current default for new indexes is %1.1f MB.",
			                (float)m_pIndex->span / 1024 / 1024, (float)SPAN_DEFAULT / 1024 / 1024);
			Console.Warning(L"It will work fine, but if you want to generate a new index with default intervals, delete this index file.");
			Console.Warning(L"(smaller intervals mean bigger index file and quicker but more frequent decompressions)");
		}
		InitZstates();
		return true;
	}

	// No valid index file. Generate an index
	Console.Warning(L"This may take a while (but only once). Scanning compressed file to generate a quick access index...");

	Access *index;
	FILE* infile = PX_fopen_rb(m_filename);
	int len = build_index(infile, SPAN_DEFAULT, &index);
	printf("\n"); // build_index prints progress without \n's
	fclose(infile);

	if (len >= 0) {
		m_pIndex = index;
		WriteIndexToFile((Access*)m_pIndex, indexfile);
	} else {
		Console.Error(L"ERROR (%d): index could not be generated for file '%s'", len, WX_STR(m_filename));
		InitZstates();
		return false;
	}

	InitZstates();
	return true;
}

bool GzippedFileReader::Open(const wxString& fileName) {
	Close();
	m_filename = fileName;
	if (!(m_src = PX_fopen_rb(m_filename)) || !CanHandle(fileName) || !OkIndex()) {
		Close();
		return false;
	};

	AsyncPrefetchOpen();
	return true;
};

void GzippedFileReader::BeginRead(void* pBuffer, uint sector, uint count) {
	// No a-sync support yet, implement as sync
	mBytesRead = ReadSync(pBuffer, sector, count);
	return;
};

int GzippedFileReader::FinishRead(void) {
	int res = mBytesRead;
	mBytesRead = -1;
	return res;
};

#define PTT clock_t
#define NOW() (clock() / (CLOCKS_PER_SEC / 1000))

int GzippedFileReader::ReadSync(void* pBuffer, uint sector, uint count) {
	PX_off_t offset = (s64)sector * m_blocksize + m_dataoffset;
	int bytesToRead = count * m_blocksize;
	int res = _ReadSync(pBuffer, offset, bytesToRead);
	if (res < 0)
		Console.Error(L"Error: iso-gzip read unsuccessful.");
	return res;
}

// If we have a valid and adequate zstate for this span, use it, else, use the index
PX_off_t GzippedFileReader::GetOptimalExtractionStart(PX_off_t offset) {
	int span = m_pIndex->span;
	Czstate& cstate = m_zstates[offset / span];
	PX_off_t stateOffset = cstate.state.isValid ? cstate.state.out_offset : 0;
	if (stateOffset && stateOffset <= offset)
		return stateOffset; // state is faster than indexed

	// If span is not exact multiples of READ_CHUNK_SIZE (because it was configured badly),
	// we fallback to always READ_CHUNK_SIZE boundaries
	if (span % READ_CHUNK_SIZE)
		return offset / READ_CHUNK_SIZE * READ_CHUNK_SIZE;

	return span * (offset / span); // index direct access boundaries
}

int GzippedFileReader::_ReadSync(void* pBuffer, PX_off_t offset, uint bytesToRead) {
	if (!OkIndex())
		return -1;

	// Without all the caching, chunking and states, this would be enough:
	// return extract(m_src, m_pIndex, offset, (unsigned char*)pBuffer, bytesToRead);

	// Split request to READ_CHUNK_SIZE chunks at READ_CHUNK_SIZE boundaries
	uint maxInChunk = READ_CHUNK_SIZE - offset % READ_CHUNK_SIZE;
	if (bytesToRead > maxInChunk) {
		int first = _ReadSync(pBuffer, offset, maxInChunk);
		if (first != maxInChunk)
			return first; // EOF or failure

		int rest = _ReadSync((char*)pBuffer + maxInChunk, offset + maxInChunk, bytesToRead - maxInChunk);
		if (rest < 0)
			return rest;

		return first + rest;
	}

	// From here onwards it's guarenteed that the request is inside a single READ_CHUNK_SIZE boundaries

	int res = m_cache.Read(pBuffer, offset, bytesToRead);
	if (res >= 0)
		return res;

	// Not available from cache. Decompress from optimal starting
	// point in READ_CHUNK_SIZE chunks and cache each chunk.
	PTT s = NOW();
	PX_off_t extractOffset = GetOptimalExtractionStart(offset); // guaranteed in READ_CHUNK_SIZE boundaries
	int size = offset + maxInChunk - extractOffset;
	unsigned char* extracted = (unsigned char*)malloc(size);

	int span = m_pIndex->span;
	int spanix = extractOffset / span;
	AsyncPrefetchCancel();
	res = extract(m_src, m_pIndex, extractOffset, extracted, size, &(m_zstates[spanix].state));
	if (res < 0) {
		free(extracted);
		return res;
	}
	AsyncPrefetchChunk(getInOffset(&(m_zstates[spanix].state)));

	int copied = ChunksCache::CopyAvailable(extracted, extractOffset, res, pBuffer, offset, bytesToRead);

	if (m_zstates[spanix].state.isValid && (extractOffset + res) / span != offset / span) {
		// The state no longer matches this span.
		// move the state to the appropriate span because it will be faster than using the index
		int targetix = (extractOffset + res) / span;
		m_zstates[targetix].Kill();
		m_zstates[targetix] = m_zstates[spanix]; // We have elements for the entire file, and another one.
		m_zstates[spanix].state.isValid = 0; // Not killing because we need the state.
	}

	if (size <= READ_CHUNK_SIZE)
		m_cache.Take(extracted, extractOffset, res, size);
	else { // split into cacheable chunks
		for (int i = 0; i < size; i += READ_CHUNK_SIZE) {
			int available = CLAMP(res - i, 0, READ_CHUNK_SIZE);
			void* chunk = available ? malloc(available) : 0;
			if (available)
				memcpy(chunk, extracted + i, available);
			m_cache.Take(chunk, extractOffset + i, available, std::min(size - i, READ_CHUNK_SIZE));
		}
		free(extracted);
	}

	int duration = NOW() - s;
	if (duration > 10)
		Console.WriteLn(Color_Gray, L"gunzip: chunk #%5d-%2d : %1.2f MB - %d ms",
		                (int)(offset / 4 / 1024 / 1024),
		                (int)(offset % (4 * 1024 * 1024) / READ_CHUNK_SIZE),
		                (float)size / 1024 / 1024,
		                duration);

	return copied;
}

void GzippedFileReader::Close() {
	m_filename.Empty();
	if (m_pIndex) {
		free_index((Access*)m_pIndex);
		m_pIndex = 0;
	}

	InitZstates(); // results in delete because no index
	m_cache.Clear();

	if (m_src) {
		fclose(m_src);
		m_src = 0;
	}

	AsyncPrefetchClose();
}


// CompressedFileReader factory - currently there's only GzippedFileReader

// Go through available compressed readers
bool CompressedFileReader::DetectCompressed(AsyncFileReader* pReader) {
	return GzippedFileReader::CanHandle(pReader->GetFilename());
}

// Return a new reader which can handle, or any reader otherwise (which will fail on open)
AsyncFileReader* CompressedFileReader::GetNewReader(const wxString& fileName) {
	//if (GzippedFileReader::CanHandle(pReader))
	return new GzippedFileReader();
}
