#define NO_UEFI

#include "gtest/gtest.h"


#include <iostream>
#include <thread>
#include <mutex>
#include <fstream>
#include <chrono>

#include "../src/platform/file_io.h"

static constexpr unsigned long long THREAD_COUNT = 4;
static constexpr unsigned long long MEM_BUFFER_SIZE = 52ULL * 1024ULL * 1024ULL;
std::mutex gMessageLock;

// For testing the scheduler save file
struct FragmentData
{
    // Reserve memories
    unsigned int memBuffer[MEM_BUFFER_SIZE];

    // Randomly parition of data for writing
    unsigned long long dataPos[4][2];
};

static std::vector<FragmentData> threadData;
static FragmentData buffer;
static unsigned char threadFinish[THREAD_COUNT];

inline static unsigned int random(const unsigned int range)
{
    unsigned int value;
    _rdrand32_step(&value);

    return value % range;
}

inline static unsigned long long random64(const unsigned long long range)
{
    unsigned long long value;
    _rdrand64_step(&value);

    return (value % range);
}

#ifdef __linux__
#include <sys/stat.h>
static unsigned long long allocatedBytes(const char* path)
{
    struct stat fileStat;
    return stat(path, &fileStat) == 0 ? (unsigned long long)fileStat.st_blocks * 512 : ~0ULL;
}
#endif

class FileSystemWrapper
{
public:
    FileSystemWrapper()
    {
        initFilesystem();
        registerAsynFileIO(NULL);
    }
    ~FileSystemWrapper()
    {
        deInitFileSystem();
    }

    bool initTestData()
    {
        memset(threadFinish, 1, THREAD_COUNT);

        threadData.resize(THREAD_COUNT);
        for (int id = 0; id < THREAD_COUNT; id++)
        {
            // randomly generate a chunk of data
            for (unsigned long long i = 0; i < MEM_BUFFER_SIZE; i++)
            {
                threadData[id].memBuffer[i] = random(4096) * ((int)i + 1);
            }
            // Randomly pick some part of data for writing out
            unsigned long long remainedData = MEM_BUFFER_SIZE;
            for (int i = 0; i < sizeof(threadData[id].dataPos) / sizeof(threadData[id].dataPos[0]); i++)
            {
                // Start of the data
                threadData[id].dataPos[i][0] = random64(MEM_BUFFER_SIZE - 1);

                // Size of the data. Make sure we limit all small files in size of total MEM_BUFFER_SIZE
                unsigned long long dataSize = random64(MEM_BUFFER_SIZE - threadData[id].dataPos[i][0]);
                dataSize = dataSize > remainedData ? remainedData : dataSize;
                remainedData = remainedData - dataSize;

                if (dataSize == 0)
                {
                    dataSize = 1;
                }

                threadData[id].dataPos[i][1] = dataSize;
            }
        }
        return true;
    }
};

static FileSystemWrapper fileSystem;

class TestNoUefiFileIO : public ::testing::Test
{
protected:
    void SetUp() override
    {
        cleanTestPaths();
    }

    void TearDown() override
    {
        cleanTestPaths();
    }

private:
    static void cleanTestPaths()
    {
        static const char* paths[] = {
            "tmp_file_no_uefi_root.bin",
            "tmp_file_no_uefi_empty.bin",
            "tmp_file_no_uefi_async.bin",
            "tmp_file_no_uefi_missing.bin",
            "tmp_file_no_uefi_short.bin",
            "tmp_file_no_uefi_dir",
            "tmp_file_no_uefi_explicit.bin",
            "tmp_file_no_uefi_existing_dir",
            "tmp_file_no_uefi_overwrite_dir",
            "tmp_file_no_uefi_overwrite.bin",
            "tmp_file_no_uefi_collision",
            "tmp_file_no_uefi_collision_payload.bin",
            "tmp_file_no_uefi_remove_dir",
            "tmp_file_no_uefi_async_dir",
            "tmp_file_no_uefi_async_explicit.bin",
            "tmp_file_no_uefi_missing_dir",
            "tmp_file_no_uefi_file_size_dir",
            "tmp_file_no_uefi_missing_size_dir",
            "tmp_file_no_uefi_missing_size.bin",
            "tmp_file_no_uefi_sparse.bin",
            "tmp_file_no_uefi_zero.bin",
            "tmp_file_no_uefi_writer_whole",
            "tmp_file_no_uefi_writer_pieces",
        };

        for (const char* path : paths)
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

#ifdef _WIN32
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(L"tmp_file_no_uefi_\u0E44\u0E17\u0E22"), error);
#endif
    }
};

TEST_F(TestNoUefiFileIO, HostFileOpenRejectsInvalidArguments)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_missing.bin";
    const std::filesystem::path filePath = getHostFilePath(fileName, nullptr);
    FILE* file = nullptr;

    EXPECT_EQ(openHostFile(nullptr, filePath, HostFileMode::ReadBinary), EINVAL);
    EXPECT_EQ(openHostFile(&file, {}, HostFileMode::ReadBinary), EINVAL);
    EXPECT_EQ(openHostFile(&file, filePath, static_cast<HostFileMode>(255)), EINVAL);
    EXPECT_EQ(file, nullptr);
}

TEST_F(TestNoUefiFileIO, HostFileOpenReturnsMissingFileError)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_missing.bin";
    const std::filesystem::path filePath = getHostFilePath(fileName, nullptr);
    FILE* file = nullptr;

    EXPECT_EQ(openHostFile(&file, filePath, HostFileMode::ReadBinary), ENOENT);
    EXPECT_EQ(file, nullptr);
}

TEST_F(TestNoUefiFileIO, HostFileModesRoundTrip)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_missing.bin";
    const std::filesystem::path filePath = getHostFilePath(fileName, nullptr);
    FILE* file = nullptr;
    const unsigned char expected = 0x5A;
    unsigned char actual = 0;

    EXPECT_EQ(openHostFile(&file, filePath, HostFileMode::WriteBinary), 0);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(fwrite(&expected, 1, sizeof(expected), file), sizeof(expected));
    EXPECT_EQ(fclose(file), 0);

    file = nullptr;
    EXPECT_EQ(openHostFile(&file, filePath, HostFileMode::ReadBinary), 0);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(fread(&actual, 1, sizeof(actual), file), sizeof(actual));
    EXPECT_EQ(fclose(file), 0);
    EXPECT_EQ(actual, expected);
}

TEST_F(TestNoUefiFileIO, HostFileWriteBinaryTruncatesExistingFile)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_missing.bin";
    const std::filesystem::path filePath = getHostFilePath(fileName, nullptr);
    const unsigned char initial[] = { 1, 2, 3 };
    const unsigned char replacement = 4;
    FILE* file = nullptr;

    ASSERT_EQ(openHostFile(&file, filePath, HostFileMode::WriteBinary), 0);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(fwrite(initial, 1, sizeof(initial), file), sizeof(initial));
    ASSERT_EQ(fclose(file), 0);

    file = nullptr;
    ASSERT_EQ(openHostFile(&file, filePath, HostFileMode::WriteBinary), 0);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(fwrite(&replacement, 1, sizeof(replacement), file), sizeof(replacement));
    ASSERT_EQ(fclose(file), 0);

    std::error_code error;
    const std::uintmax_t fileSize = std::filesystem::file_size(filePath, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(fileSize, sizeof(replacement));
}

TEST_F(TestNoUefiFileIO, CreateDirIsIdempotent)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_existing_dir";

    EXPECT_FALSE(checkDir(directory));
    ASSERT_TRUE(createDir(directory));
    EXPECT_TRUE(checkDir(directory));
    EXPECT_TRUE(createDir(directory));
    EXPECT_TRUE(checkDir(directory));
}

TEST_F(TestNoUefiFileIO, SaveOverwritesFileInExistingDirectory)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_overwrite_dir";
    CHAR16 fileName[] = L"tmp_file_no_uefi_overwrite.bin";
    const unsigned char initial[] = { 1, 2, 3, 4 };
    const unsigned char replacement[] = { 5, 6 };
    unsigned char actual[sizeof(replacement)] = {};
    const std::filesystem::path expectedPath =
        std::filesystem::path("tmp_file_no_uefi_overwrite_dir") / "tmp_file_no_uefi_overwrite.bin";

    ASSERT_TRUE(createDir(directory));
    ASSERT_EQ(save(fileName, sizeof(initial), initial, directory), sizeof(initial));
    ASSERT_TRUE(std::filesystem::is_regular_file(expectedPath));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_overwrite.bin"));

    ASSERT_EQ(save(fileName, sizeof(replacement), replacement, directory), sizeof(replacement));
    EXPECT_EQ(getFileSize(fileName, directory), sizeof(replacement));
    ASSERT_EQ(load(fileName, sizeof(actual), actual, directory), sizeof(actual));
    EXPECT_EQ(memcmp(actual, replacement, sizeof(replacement)), 0);
}

TEST_F(TestNoUefiFileIO, DirectoryCollisionFailsWithoutFallback)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_collision";
    CHAR16 fileName[] = L"tmp_file_no_uefi_collision_payload.bin";
    const unsigned char sentinel = 0x4A;
    const unsigned char payload = 0x7B;

    ASSERT_EQ(save(directory, sizeof(sentinel), &sentinel, nullptr), sizeof(sentinel));
    ASSERT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_collision"));
    EXPECT_FALSE(checkDir(directory));
    EXPECT_FALSE(createDir(directory));
    EXPECT_EQ(save(fileName, sizeof(payload), &payload, directory), -1);
    EXPECT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_collision"));
    EXPECT_EQ(std::filesystem::file_size("tmp_file_no_uefi_collision"), sizeof(sentinel));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_collision_payload.bin"));
}

TEST_F(TestNoUefiFileIO, RemoveDirIsRecursiveAndIdempotent)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_remove_dir";
    const std::filesystem::path nestedDirectory =
        std::filesystem::path("tmp_file_no_uefi_remove_dir") / "nested";
    const std::filesystem::path nestedFile = nestedDirectory / "state.bin";
    std::error_code error;

    EXPECT_TRUE(removeDir(directory));
    ASSERT_TRUE(std::filesystem::create_directories(nestedDirectory, error));
    ASSERT_FALSE(error);
    {
        std::ofstream output(nestedFile, std::ios::binary);
        ASSERT_TRUE(output);
        output.put(static_cast<char>(0x5A));
        ASSERT_TRUE(output);
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(nestedFile));

    EXPECT_TRUE(removeDir(directory));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_remove_dir"));
    EXPECT_TRUE(removeDir(directory));
}

TEST_F(TestNoUefiFileIO, AsyncRemoveUsesExplicitDirectoryOnly)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_async_dir";
    CHAR16 fileName[] = L"tmp_file_no_uefi_async_explicit.bin";
    const unsigned char directoryData = 0x31;
    const unsigned char workingDirectoryData = 0x62;
    const std::filesystem::path directoryFile =
        std::filesystem::path("tmp_file_no_uefi_async_dir") / "tmp_file_no_uefi_async_explicit.bin";

    ASSERT_EQ(save(fileName, sizeof(directoryData), &directoryData, directory), sizeof(directoryData));
    ASSERT_EQ(save(fileName, sizeof(workingDirectoryData), &workingDirectoryData, nullptr),
              sizeof(workingDirectoryData));
    ASSERT_TRUE(std::filesystem::is_regular_file(directoryFile));
    ASSERT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_async_explicit.bin"));

    EXPECT_EQ(asyncRemoveFile(fileName, directory), 0);
    EXPECT_FALSE(std::filesystem::exists(directoryFile));
    EXPECT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_async_explicit.bin"));
    EXPECT_EQ(getFileSize(fileName, nullptr), sizeof(workingDirectoryData));

    EXPECT_EQ(asyncRemoveFile(fileName, directory), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_async_explicit.bin"));
}

TEST_F(TestNoUefiFileIO, MissingFileSizeReturnsMinusOne)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_missing_size_dir";
    CHAR16 fileName[] = L"tmp_file_no_uefi_missing_size.bin";

    ASSERT_TRUE(createDir(directory));
    EXPECT_EQ(getFileSize(fileName, nullptr), -1);
    EXPECT_EQ(getFileSize(fileName, directory), -1);
}

TEST_F(TestNoUefiFileIO, NullDirectoryUsesWorkingDirectory)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_root.bin";
    const unsigned char expected[] = { 1, 2, 3, 4 };
    unsigned char actual[sizeof(expected)] = {};

    ASSERT_EQ(save(fileName, sizeof(expected), expected, nullptr), sizeof(expected));
    ASSERT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_root.bin"));
    EXPECT_EQ(getFileSize(fileName, nullptr), sizeof(expected));
    ASSERT_EQ(load(fileName, sizeof(actual), actual, nullptr), sizeof(actual));
    EXPECT_EQ(memcmp(actual, expected, sizeof(expected)), 0);
    EXPECT_TRUE(removeFile(nullptr, fileName));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_root.bin"));
    EXPECT_TRUE(removeFile(nullptr, fileName));
}

TEST_F(TestNoUefiFileIO, EmptyDirectoryUsesWorkingDirectory)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_empty.bin";
    CHAR16 directory[] = L"";
    const unsigned char expected[] = { 5, 6, 7 };

    ASSERT_EQ(save(fileName, sizeof(expected), expected, directory), sizeof(expected));
    ASSERT_TRUE(std::filesystem::is_regular_file("tmp_file_no_uefi_empty.bin"));
    EXPECT_EQ(getFileSize(fileName, directory), sizeof(expected));
    EXPECT_TRUE(removeFile(directory, fileName));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_empty.bin"));
}

TEST_F(TestNoUefiFileIO, ExplicitDirectoryIsUsedConsistently)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_explicit.bin";
    CHAR16 directory[] = L"tmp_file_no_uefi_dir";
    const unsigned char expected[] = { 8, 9, 10 };
    unsigned char actual[sizeof(expected)] = {};
    const std::filesystem::path expectedPath =
        std::filesystem::path("tmp_file_no_uefi_dir") / "tmp_file_no_uefi_explicit.bin";

    ASSERT_EQ(save(fileName, sizeof(expected), expected, directory), sizeof(expected));
    EXPECT_TRUE(checkDir(directory));
    EXPECT_TRUE(std::filesystem::is_regular_file(expectedPath));
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_explicit.bin"));
    EXPECT_EQ(getFileSize(fileName, directory), sizeof(expected));
    ASSERT_EQ(load(fileName, sizeof(actual), actual, directory), sizeof(actual));
    EXPECT_EQ(memcmp(actual, expected, sizeof(expected)), 0);
    EXPECT_TRUE(removeFile(directory, fileName));
    EXPECT_TRUE(removeDir(directory));
}

TEST_F(TestNoUefiFileIO, AsyncRemoveSupportsNullDirectory)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_async.bin";
    const unsigned char data[] = { 11 };

    ASSERT_EQ(save(fileName, sizeof(data), data, nullptr), sizeof(data));
    ASSERT_TRUE(std::filesystem::exists("tmp_file_no_uefi_async.bin"));
    EXPECT_EQ(asyncRemoveFile(fileName, nullptr), 0);
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_async.bin"));
}

TEST_F(TestNoUefiFileIO, LoadDoesNotCreateMissingDirectory)
{
    CHAR16 fileName[] = L"missing.bin";
    CHAR16 directory[] = L"tmp_file_no_uefi_missing_dir";
    unsigned char data = 0;

    EXPECT_EQ(load(fileName, sizeof(data), &data, directory), -1);
    EXPECT_FALSE(std::filesystem::exists("tmp_file_no_uefi_missing_dir"));
}

TEST_F(TestNoUefiFileIO, FileSizeRejectsDirectory)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_file_size_dir";

    ASSERT_TRUE(createDir(directory));
    EXPECT_EQ(getFileSize(directory, nullptr), -1);
}

TEST_F(TestNoUefiFileIO, ShortReadReturnsFailureAndClosesFile)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_short.bin";
    const unsigned char saved = 12;
    unsigned char loaded[2] = {};

    ASSERT_EQ(save(fileName, sizeof(saved), &saved, nullptr), sizeof(saved));
    EXPECT_EQ(load(fileName, sizeof(loaded), loaded, nullptr), -1);
    EXPECT_TRUE(removeFile(nullptr, fileName));
}

// Zero runs at the start, in the middle and at the end read back as written, and the length is kept.
TEST_F(TestNoUefiFileIO, SaveKeepsZeroRunsAndLength)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_sparse.bin";
    const unsigned long long size = 3ULL * 1024 * 1024 + 123;
    std::vector<unsigned char> expected(size, 0);
    expected[20000] = 1;
    for (unsigned long long i = 1024 * 1024 + 100; i < 1024 * 1024 + 9000; i++)
    {
        expected[i] = (unsigned char)(i % 251 + 1);
    }

    ASSERT_EQ(save(fileName, size, expected.data(), nullptr), (long long)size);
    EXPECT_EQ(getFileSize(fileName, nullptr), (long long)size);
    std::vector<unsigned char> actual(size, 0xFF);
    ASSERT_EQ(load(fileName, size, actual.data(), nullptr), (long long)size);
    EXPECT_EQ(memcmp(actual.data(), expected.data(), size), 0);
#ifdef __linux__
    EXPECT_LT(allocatedBytes("tmp_file_no_uefi_sparse.bin"), size / 2);
#endif
}

TEST_F(TestNoUefiFileIO, SaveAllZeroBufferKeepsLength)
{
    CHAR16 fileName[] = L"tmp_file_no_uefi_zero.bin";
    const unsigned long long size = 1024 * 1024 + 7;
    std::vector<unsigned char> zeros(size, 0);

    ASSERT_EQ(save(fileName, size, zeros.data(), nullptr), (long long)size);
    EXPECT_EQ(getFileSize(fileName, nullptr), (long long)size);
    std::vector<unsigned char> actual(size, 0xFF);
    ASSERT_EQ(load(fileName, size, actual.data(), nullptr), (long long)size);
    EXPECT_EQ(memcmp(actual.data(), zeros.data(), size), 0);
}

// Pieces of any size must end up in the same files, byte for byte, as one saveLargeFile() call.
TEST_F(TestNoUefiFileIO, LargeFileWriterMatchesSaveLargeFile)
{
    CHAR16 wholeDirectory[] = L"tmp_file_no_uefi_writer_whole";
    CHAR16 piecesDirectory[] = L"tmp_file_no_uefi_writer_pieces";
    CHAR16 chunkedName[] = L"chunked.bin";
    CHAR16 singleName[] = L"single.bin";
    ASSERT_TRUE(createDir(wholeDirectory));
    ASSERT_TRUE(createDir(piecesDirectory));

    // Two chunk files, with data at the start, across the chunk boundary and at the very end.
    const unsigned long long chunkedSize = FILE_CHUNK_SIZE + 5ULL * 1024 * 1024 + 77;
    std::vector<unsigned char> data(chunkedSize, 0);
    for (unsigned long long i = 0; i < 5000; i++)
    {
        data[i] = (unsigned char)(i % 253 + 1);
        data[FILE_CHUNK_SIZE - 2500 + i] = (unsigned char)(i % 241 + 1);
        data[chunkedSize - 5000 + i] = (unsigned char)(i % 239 + 1);
    }
    const unsigned long long singleSize = 3ULL * 1024 * 1024 + 5;

    const auto writeInPieces = [&](CHAR16* fileName, unsigned long long totalSize)
    {
        LargeFileWriter writer;
        if (!writer.open(fileName, totalSize, piecesDirectory))
        {
            return false;
        }
        const unsigned long long pieceSize = 7777777;
        for (unsigned long long offset = 0; offset < totalSize; offset += pieceSize)
        {
            const unsigned long long size = totalSize - offset < pieceSize ? totalSize - offset : pieceSize;
            if (!writer.write(data.data() + offset, size))
            {
                return false;
            }
        }
        return writer.close();
    };

    ASSERT_EQ(saveLargeFile(chunkedName, chunkedSize, data.data(), wholeDirectory, false), (long long)chunkedSize);
    ASSERT_EQ(saveLargeFile(singleName, singleSize, data.data(), wholeDirectory, false), (long long)singleSize);
    ASSERT_TRUE(writeInPieces(chunkedName, chunkedSize));
    ASSERT_TRUE(writeInPieces(singleName, singleSize));

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    unsigned int fileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator("tmp_file_no_uefi_writer_whole"))
    {
        const std::filesystem::path twin = std::filesystem::path("tmp_file_no_uefi_writer_pieces") / entry.path().filename();
        ASSERT_TRUE(std::filesystem::exists(twin)) << twin;
        EXPECT_TRUE(readFile(entry.path()) == readFile(twin)) << twin;
        fileCount++;
    }
    EXPECT_EQ(fileCount, 3u);
    unsigned int piecesFileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator("tmp_file_no_uefi_writer_pieces"))
    {
        (void)entry;
        piecesFileCount++;
    }
    EXPECT_EQ(piecesFileCount, 3u);

    std::vector<unsigned char> loaded(chunkedSize, 0xFF);
    ASSERT_EQ(loadLargeFile(chunkedName, chunkedSize, loaded.data(), piecesDirectory), (long long)chunkedSize);
    EXPECT_EQ(memcmp(loaded.data(), data.data(), chunkedSize), 0);

    // A writer that got fewer or more bytes than announced must not report success.
    LargeFileWriter shortWriter;
    ASSERT_TRUE(shortWriter.open(singleName, singleSize, piecesDirectory));
    ASSERT_TRUE(shortWriter.write(data.data(), singleSize - 1));
    EXPECT_FALSE(shortWriter.close());
    LargeFileWriter longWriter;
    ASSERT_TRUE(longWriter.open(singleName, singleSize, piecesDirectory));
    ASSERT_TRUE(longWriter.write(data.data(), singleSize));
    EXPECT_FALSE(longWriter.write(data.data(), 1));
    EXPECT_FALSE(longWriter.close());
}

#ifdef __linux__
TEST_F(TestNoUefiFileIO, WriteFailureReturnsError)
{
    CHAR16 fileName[] = L"/dev/full";
    unsigned char data[64 * 1024];
    memset(data, 0x5A, sizeof(data));

    EXPECT_EQ(save(fileName, sizeof(data), data, nullptr), -1);
}
#endif

#ifdef _WIN32
TEST_F(TestNoUefiFileIO, NativeUnicodePathRoundTrip)
{
    CHAR16 directory[] = L"tmp_file_no_uefi_\u0E44\u0E17\u0E22";
    CHAR16 fileName[] = L"\u0E02\u0E49\u0E2D\u0E21\u0E39\u0E25.bin";
    const unsigned char expected[] = { 0x12, 0x34, 0x56 };
    unsigned char actual[sizeof(expected)] = {};

    ASSERT_TRUE(removeDir(directory));
    ASSERT_EQ(save(fileName, sizeof(expected), expected, directory), sizeof(expected));
    ASSERT_EQ(load(fileName, sizeof(actual), actual, directory), sizeof(actual));
    EXPECT_EQ(memcmp(actual, expected, sizeof(expected)), 0);
    EXPECT_TRUE(removeFile(directory, fileName));
    EXPECT_TRUE(removeDir(directory));
}
#endif

long long loadFile(CHAR16* fileName, unsigned long long totalSize, char* buffer)
{
    FILE* file = nullptr;
    if (openHostFile(&file, getHostFilePath(fileName, nullptr), HostFileMode::ReadBinary) != 0 || !file)
    {
        wprintf(L"Error opening file %s!\n", fileName);
        return -1;
    }
    if (fread(buffer, 1, totalSize, file) != totalSize)
    {
        wprintf(L"Error reading %llu bytes from %s!\n", totalSize, fileName);
        return -1;
    }
    fclose(file);
    return totalSize;
}

long long saveFile(CHAR16* fileName, unsigned long long totalSize, const char* buffer)
{
    FILE* file = nullptr;
    if (openHostFile(&file, getHostFilePath(fileName, nullptr), HostFileMode::WriteBinary) != 0 || !file)
    {
        wprintf(L"Error opening file %s!\n", fileName);
        return -1;
    }
    if (fwrite(buffer, 1, totalSize, file) != totalSize)
    {
        wprintf(L"Error saving %llu bytes from %s!\n", totalSize, fileName);
        return -1;
    }
    fclose(file);
    return totalSize;
}

bool runAsyncSaveFile(int id, bool blocking = true, bool largeFile = false)
{
    bool sts = true;
    CHAR16 fileName[32];
    setText(fileName, L"tmp_file_");
    appendNumber(fileName, id, false);

    if (largeFile)
    {
        long long sts = asyncSaveLargeFile(fileName, MEM_BUFFER_SIZE * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[0]), NULL, false, blocking);
        if (sts <= 0)
        {
            std::lock_guard<std::mutex> lock(gMessageLock);
            std::cout << "runAsyncSaveFile::saveFile failed with size " << MEM_BUFFER_SIZE * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
            sts = false;
        }
    }
    else
    {
        // Try to save the files
        for (int i = 0; i < sizeof(threadData[id].dataPos) / sizeof(threadData[id].dataPos[0]); i++)
        {
            unsigned long long dataStart = threadData[id].dataPos[i][0];
            unsigned long long dataCount = threadData[id].dataPos[i][1];

            CHAR16 partionFileName[256];
            setText(partionFileName, fileName);
            appendText(partionFileName, L".");
            appendNumber(partionFileName, i, false);

            long long sts = asyncSave(partionFileName, dataCount * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[dataStart]), NULL, blocking);
            if (sts <= 0)
            {
                std::lock_guard<std::mutex> lock(gMessageLock);
                std::cout << "runAsyncSaveFile::saveFile failed with size " << dataCount * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
                sts = false;
                break;
            }
        }
    }

    threadFinish[id] = 1;
    return sts;
}

bool prepareAsyncLoadFile(bool largeFile = false)
{
    bool sts = true;
    fileSystem.initTestData();

    for (int id = 0; id < THREAD_COUNT; id++)
    {
        CHAR16 fileName[32];
        setText(fileName, L"tmp_file_");
        appendNumber(fileName, id, false);

        if (largeFile)
        {
            long long sts = saveLargeFile(fileName, MEM_BUFFER_SIZE * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[0]), NULL, false);
            if (sts <= 0)
            {
                std::lock_guard<std::mutex> lock(gMessageLock);
                std::cout << "prepareAsyncLoadFile::saveFile failed with size " << MEM_BUFFER_SIZE * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
                sts = false;
            }
        }
        else
        {
            // Try to save the files
            for (int i = 0; i < sizeof(threadData[id].dataPos) / sizeof(threadData[id].dataPos[0]); i++)
            {
                unsigned long long dataStart = threadData[id].dataPos[i][0];
                unsigned long long dataCount = threadData[id].dataPos[i][1];

                CHAR16 partionFileName[256];
                setText(partionFileName, fileName);
                appendText(partionFileName, L".");
                appendNumber(partionFileName, i, false);

                long long sts = save(partionFileName, dataCount * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[dataStart]), NULL);
                if (sts <= 0)
                {
                    std::lock_guard<std::mutex> lock(gMessageLock);
                    std::cout << "prepareAsyncLoadFile::saveFile failed with size " << dataCount * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
                    sts = false;
                    break;
                }
            }
        }
    }
    return sts;
}

bool runAsyncLoadFile(int id, bool largeFile = false)
{
    bool sts = true;
    CHAR16 fileName[32];
    setText(fileName, L"tmp_file_");
    appendNumber(fileName, id, false);

    if (largeFile)
    {
        long long sts = asyncLoadLargeFile(fileName, MEM_BUFFER_SIZE * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[0]), NULL);
        if (sts <= 0)
        {
            std::lock_guard<std::mutex> lock(gMessageLock);
            std::cout << "runAsyncLoadFile::loadFile failed with size " << MEM_BUFFER_SIZE * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
            sts = false;
        }
    }
    else
    {
        // Try to load the files
        for (int i = 0; i < sizeof(threadData[id].dataPos) / sizeof(threadData[id].dataPos[0]); i++)
        {
            unsigned long long dataStart = threadData[id].dataPos[i][0];
            unsigned long long dataCount = threadData[id].dataPos[i][1];

            CHAR16 partionFileName[256];
            setText(partionFileName, fileName);
            appendText(partionFileName, L".");
            appendNumber(partionFileName, i, false);

            long long sts = asyncLoad(partionFileName, dataCount * sizeof(unsigned int), (unsigned char*)&(threadData[id].memBuffer[dataStart]), NULL);
            if (sts <= 0)
            {
                std::lock_guard<std::mutex> lock(gMessageLock);
                std::cout << "runAsyncLoadFile::loadFile failed with size " << dataCount * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
                sts = false;
                break;
            }
        }
    }

    threadFinish[id] = 1;
    return sts;
}


bool verifyResult(int id, bool largeFile = false)
{
    bool testPass = false;
    CHAR16 fileName[32];
    setText(fileName, L"tmp_file_");
    appendNumber(fileName, id, false);

    if (largeFile)
    {
        long long sts = loadLargeFile(fileName, MEM_BUFFER_SIZE * sizeof(unsigned int), (unsigned char*)buffer.memBuffer, NULL);
        unsigned char* originalData = (unsigned char*)&(threadData[id].memBuffer[0]);
        unsigned char* loadedData = (unsigned char*)&(buffer.memBuffer[0]);
        int result = memcmp(originalData, loadedData, MEM_BUFFER_SIZE * sizeof(unsigned int));
        testPass = (result == 0);
    }
    else
    {
        int matchCount = 0;
        int numberOfFiles = sizeof(threadData[id].dataPos) / sizeof(threadData[id].dataPos[0]);
        for (int i = 0; i < numberOfFiles; i++)
        {
            unsigned long long dataStart = threadData[id].dataPos[i][0];
            unsigned long long dataCount = threadData[id].dataPos[i][1];

            CHAR16 partionFileName[256];
            setText(partionFileName, fileName);
            appendText(partionFileName, L".");
            appendNumber(partionFileName, i, false);

            long long sts = loadFile(partionFileName, dataCount * sizeof(unsigned int), (char*)buffer.memBuffer);

            if (sts != dataCount * sizeof(unsigned int))
            {
                std::cout << "verifyResult failed with size " << dataCount * sizeof(unsigned int) / 1024 << " KB. Error " << sts << std::endl;
                return false;
            }

            unsigned char* originalData = (unsigned char*)&(threadData[id].memBuffer[dataStart]);
            unsigned char* loadedData = (unsigned char*)&(buffer.memBuffer[0]);

            int result = memcmp(originalData, loadedData, dataCount * sizeof(unsigned int));
            if (result == 0)
            {
                matchCount++;
            }
        }
        testPass = (matchCount == numberOfFiles);
    }

    return testPass;
}

int runTestAsyncSaveFile(bool blocking, bool largeFile, bool limitItem)
{
    fileSystem.initTestData();

    // Run the test
    std::vector<std::unique_ptr<std::thread>> threadVec(THREAD_COUNT);
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        threadFinish[i] = 0;
        threadVec[i].reset(new std::thread(runAsyncSaveFile, i, blocking, largeFile));
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    int readyCount = 0;
    while (readyCount < THREAD_COUNT)
    {
        // Don't flush right away. Wait sometimes for simulate
        if (limitItem)
        {
            flushAsyncFileIOBuffer(2);
        }
        else
        {
            unsigned long long waitingTimeInMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - startTime).count();
            if (waitingTimeInMs > 1000)
            {
                startTime = std::chrono::high_resolution_clock::now();
                flushAsyncFileIOBuffer();
            }
        }

        readyCount = 0;
        for (int i = 0; i < THREAD_COUNT; i++)
        {
            char readyFlag = 0;
            readyFlag = threadFinish[i];
            if (readyFlag)
            {
                readyCount++;
            }
        }
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        if (threadVec[i]->joinable())
        {
            threadVec[i]->join();
        }
    }

    // Non blocking, need to flush all data
    if (!blocking)
    {
        flushAsyncFileIOBuffer();
    }

    // Verify result
    int testPass = 0;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        if (verifyResult(i, largeFile))
        {
            testPass++;
        }
    }
    return testPass;
}


int runTestAsyncLoadFile(bool blocking, bool largeFile, bool limitItem)
{
    prepareAsyncLoadFile(largeFile);

    // Run the test
    std::vector<std::unique_ptr<std::thread>> threadVec(THREAD_COUNT);
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        threadFinish[i] = 0;
        threadVec[i].reset(new std::thread(runAsyncLoadFile, i, largeFile));
    }
    auto startTime = std::chrono::high_resolution_clock::now();
    int readyCount = 0;
    while (readyCount < THREAD_COUNT)
    {
        // Don't flush right away. Wait sometimes for simulate
        if (limitItem)
        {
            flushAsyncFileIOBuffer(2);
        }
        else
        {
            unsigned long long waitingTimeInMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - startTime).count();
            if (waitingTimeInMs > 1000)
            {
                startTime = std::chrono::high_resolution_clock::now();
                flushAsyncFileIOBuffer();
            }
        }

        readyCount = 0;
        for (int i = 0; i < THREAD_COUNT; i++)
        {
            char readyFlag = 0;
            readyFlag = threadFinish[i];
            if (readyFlag)
            {
                readyCount++;
            }
        }
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        if (threadVec[i]->joinable())
        {
            threadVec[i]->join();
        }
    }

    // Verify result
    int testPass = 0;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        if (verifyResult(i, largeFile))
        {
            testPass++;
        }
    }
    return testPass;
}

TEST(TestAsyncFileIO, AsyncNonBlockingSaveFile)
{
    if (ASYNC_FILE_IO_WRITE_QUEUE_BUFFER_SIZE > 0)
    {
        EXPECT_EQ(runTestAsyncSaveFile(false, false, false), THREAD_COUNT);
    }
    else
    {
        EXPECT_TRUE(true);
    }
}

TEST(TestAsyncFileIO, AsyncNonBlockingSaveLargeFile)
{
    if (ASYNC_FILE_IO_WRITE_QUEUE_BUFFER_SIZE > 0)
    {
        EXPECT_EQ(runTestAsyncSaveFile(false, true, false), THREAD_COUNT);
    }
    else
    {
        EXPECT_TRUE(true);
    }
}

TEST(TestAsyncFileIO, AsyncNonBlockingSaveFileWithLimitItems)
{
    if (ASYNC_FILE_IO_WRITE_QUEUE_BUFFER_SIZE > 0)
    {
        EXPECT_EQ(runTestAsyncSaveFile(false, false, true), THREAD_COUNT);
    }
    else
    {
        EXPECT_TRUE(true);
    }
}

TEST(TestAsyncFileIO, AsyncNonBlockingSaveLargeFileWithLimitItems)
{
    if (ASYNC_FILE_IO_WRITE_QUEUE_BUFFER_SIZE > 0)
    {
        EXPECT_EQ(runTestAsyncSaveFile(false, true, true), THREAD_COUNT);
    }
    else
    {
        EXPECT_TRUE(true);
    }
}


TEST(TestAsyncFileIO, AsyncSaveFile)
{
    EXPECT_EQ(runTestAsyncSaveFile(true, false, false), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncSaveLargeFile)
{
    EXPECT_EQ(runTestAsyncSaveFile(true, true, false), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncSaveFileWithLimitItems)
{
    EXPECT_EQ(runTestAsyncSaveFile(true, false, true), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncSaveLargeFileWithLimitItems)
{
    EXPECT_EQ(runTestAsyncSaveFile(true, true, true), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncLoadFile)
{
    EXPECT_EQ(runTestAsyncLoadFile(true, false, false), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncLoadLargeFile)
{
    EXPECT_EQ(runTestAsyncLoadFile(true, true, false), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncLoadFileWithLimitItems)
{
    EXPECT_EQ(runTestAsyncLoadFile(true, false, true), THREAD_COUNT);
}

TEST(TestAsyncFileIO, AsyncLoadLargeFileWithLimitItems)
{
    EXPECT_EQ(runTestAsyncLoadFile(true, true, true), THREAD_COUNT);
}

TEST(TestAsyncFileIO, FindKLargest)
{
    constexpr int NUMBER_OF_ELEMENTS = 2025;
    constexpr int K_NUMBER = 245;

    PairStruct<unsigned int, long long> priorityArray[NUMBER_OF_ELEMENTS];

    // Generate the elements
    for (int i = 0; i < NUMBER_OF_ELEMENTS; i++)
    {
        priorityArray[i]._key = i;
        priorityArray[i]._value = random(NUMBER_OF_ELEMENTS);
    }
   
    // Find K largest in priorityArray
    findKLargest(priorityArray, K_NUMBER, NUMBER_OF_ELEMENTS);
    
    // Comparison. Expect all K left items are greater than remained items
    for (int i = 0; i < K_NUMBER; i++)
    {
        for (int j = K_NUMBER; j < NUMBER_OF_ELEMENTS; j++)
        {
            EXPECT_GE(priorityArray[i]._value, priorityArray[j]._value);
        }
    }
}

TEST(TestAsyncFileIO, FindKLargestOneItemArray)
{
    constexpr int NUMBER_OF_ELEMENTS = 1;
    constexpr int K_NUMBER = 1;

    PairStruct<unsigned int, long long> priorityArray[NUMBER_OF_ELEMENTS];
    priorityArray[0]._value = random(1000);

    // Find K largest in priorityArray
    findKLargest(priorityArray, K_NUMBER, NUMBER_OF_ELEMENTS);

    // Comparison. Expect all K left items are greater than remained items
    for (int i = 0; i < K_NUMBER; i++)
    {
        for (int j = K_NUMBER; j < NUMBER_OF_ELEMENTS; j++)
        {
            EXPECT_GE(priorityArray[i]._value, priorityArray[j]._value);
        }
    }
}

TEST(TestAsyncFileIO, FindKLargestDuplicatedItems)
{
    constexpr int NUMBER_OF_ELEMENTS = 1000;
    constexpr int K_NUMBER = 100;

    PairStruct<unsigned int, long long> priorityArray[NUMBER_OF_ELEMENTS];

    // Generate the elements
    const int value = random(NUMBER_OF_ELEMENTS);
    for (int i = 0; i < NUMBER_OF_ELEMENTS; i++)
    {
        priorityArray[i]._key = i;
        priorityArray[i]._value = value;
    }

    // Find K largest in priorityArray
    findKLargest(priorityArray, K_NUMBER, NUMBER_OF_ELEMENTS);

    // Comparison. Expect all K left items are greater than remained items
    for (int i = 0; i < K_NUMBER; i++)
    {
        for (int j = K_NUMBER; j < NUMBER_OF_ELEMENTS; j++)
        {
            EXPECT_GE(priorityArray[i]._value, priorityArray[j]._value);
        }
    }
}

TEST(TestAsyncFileIO, FindKLargestK1)
{
    constexpr int NUMBER_OF_ELEMENTS = 1000;
    constexpr int K_NUMBER = 1;

    PairStruct<unsigned int, long long> priorityArray[NUMBER_OF_ELEMENTS];

    // Generate the elements
    for (int i = 0; i < NUMBER_OF_ELEMENTS; i++)
    {
        priorityArray[i]._key = i;
        priorityArray[i]._value = random(NUMBER_OF_ELEMENTS);
    }

    // Find K largest in priorityArray
    findKLargest(priorityArray, K_NUMBER, NUMBER_OF_ELEMENTS);

    // Comparison. The first item is the largest one
    for (int j = 1; j < NUMBER_OF_ELEMENTS; j++)
    {
        EXPECT_GE(priorityArray[0]._value, priorityArray[j]._value);
    }
}

TEST(TestAsyncFileIO, FindKLargestKDuplicated)
{
    constexpr int NUMBER_OF_ELEMENTS = 1000;
    constexpr int K_NUMBER = 100;

    PairStruct<unsigned int, long long> priorityArray[NUMBER_OF_ELEMENTS];

    // Generate the elements
    std::vector<long long> numbers(NUMBER_OF_ELEMENTS);

    // Constant value for first K_NUMBER + 1 element
    const int value = NUMBER_OF_ELEMENTS + 1;
    for (int i =0; i < K_NUMBER + 1; i++)
    {
        priorityArray[i]._value = value;
    }
    for (int i = K_NUMBER + 1; i < NUMBER_OF_ELEMENTS; i++)
    {
        priorityArray[i]._value = random(NUMBER_OF_ELEMENTS);
    }

    // Shuffle the array
    for (int i = 0; i < NUMBER_OF_ELEMENTS; i++)
    {
        int newLocation = random(NUMBER_OF_ELEMENTS);
        long long tempValue = priorityArray[i]._value;
        priorityArray[i]._value = priorityArray[newLocation]._value;
        priorityArray[newLocation]._value = tempValue;
    }

    // Find K largest in priorityArray
    findKLargest(priorityArray, K_NUMBER, NUMBER_OF_ELEMENTS);

    // Comparison. The first item is the largest one
    for (int j = 0; j < K_NUMBER; j++)
    {
        EXPECT_EQ(priorityArray[j]._value, value);
    }
}


// A failed load() must release the file, or the next save() to it fails on Windows.
TEST(TestFileIO, ShortReadLoadThenSave)
{
    CHAR16 fileName[] = L"fileIoShortRead.tmp";
    unsigned char data[64] = { 1 };
    ASSERT_EQ(save(fileName, 16, data), 16);
    EXPECT_EQ(load(fileName, sizeof(data), data), -1);
    EXPECT_EQ(save(fileName, sizeof(data), data), (long long)sizeof(data));
    EXPECT_TRUE(removeFile(nullptr, fileName));
}

// #453: blocking asyncLoad()/asyncSave() used to return the request size even
// when the underlying load()/save() failed, hiding the error. These drive a
// guaranteed-failing blocking op and assert the real (<0) result is propagated.
// Under NO_UEFI isMainThread() is always false, so the op runs on a worker thread
// while the test thread flushes, as in runTestAsyncSaveFile/LoadFile above.

static long long gFailResult = 0;

static void blockingLoadMissingWorker()
{
    CHAR16 fileName[64];
    setText(fileName, L"nonexistent_file_for_1027_propagation_test.bin");
    static unsigned int loadBuffer[16];
    // Blocking load of a file that does not exist -> load() returns -1.
    gFailResult = asyncLoad(fileName, sizeof(loadBuffer), (unsigned char*)loadBuffer, NULL);
    threadFinish[0] = 1;
}

TEST(TestAsyncFileIO, AsyncBlockingLoadMissingFilePropagatesFailure)
{
    CHAR16 fileName[64];
    setText(fileName, L"nonexistent_file_for_1027_propagation_test.bin");
    removeFile(nullptr, fileName); // make sure it really is missing

    threadFinish[0] = 0;
    gFailResult = 0;
    std::thread worker(blockingLoadMissingWorker);

    while (!threadFinish[0])
    {
        flushAsyncFileIOBuffer();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();

    // Pre-#1027 this returned totalSize; now it must surface the load failure.
    EXPECT_LT(gFailResult, 0);
}

static void blockingSaveBadPathWorker()
{
    CHAR16 fileName[96];
    // A file inside a directory that does not exist cannot be opened for "wb".
    setText(fileName, L"nonexistent_dir_for_1027/cannot_be_created.bin");
    static unsigned int saveBuffer[16] = { 0 };
    // Blocking save (blocking == true) -> save() returns -1 on open failure.
    gFailResult = asyncSave(fileName, sizeof(saveBuffer), (unsigned char*)saveBuffer, NULL, true);
    threadFinish[0] = 1;
}

TEST(TestAsyncFileIO, AsyncBlockingSaveFailurePropagatesFailure)
{
    threadFinish[0] = 0;
    gFailResult = 0;
    std::thread worker(blockingSaveBadPathWorker);

    while (!threadFinish[0])
    {
        flushAsyncFileIOBuffer();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();

    // Pre-#1027 this returned totalSize; now it must surface the save failure.
    EXPECT_LT(gFailResult, 0);
}
