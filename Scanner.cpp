// CVE-2021-44228-Scan.cpp : This file contains the 'main' function. Program
// execution begins and ends there.
//

#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <time.h>

#include <iostream>
#include <string>
#include <vector>

#include "minizip/unzip.h"
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "zlib/zlib.h"

#define ARGX3(s1, s2, s3) \
  (!stricmp(argv[i], s1) || !stricmp(argv[i], s2) || !stricmp(argv[i], s3))
#define ARG(S) ARGX3("-" #S, "--" #S, "/" #S)
#define ARGPARAMCOUNT(X) ((i + X) <= (argc - 1))

#define SAFE_CLOSE_HANDLE(x)                  \
  do {                                        \
    if ((x) && INVALID_HANDLE_VALUE != (x)) { \
      ::CloseHandle(x);                       \
      (x) = INVALID_HANDLE_VALUE;             \
    }                                         \
  } while (FALSE)

class CReportSummary {
 public:
  uint64_t scannedFiles;
  uint64_t scannedDirectories;
  uint64_t scannedZIPs;
  uint64_t scannedJARs;
  uint64_t scannedWARs;
  uint64_t scannedEARs;
  uint64_t foundVunerabilities;
  uint64_t scanStart;
  uint64_t scanEnd;

  CReportSummary() {
    scannedFiles = 0;
    scannedDirectories = 0;
    scannedZIPs = 0;
    scannedJARs = 0;
    scannedWARs = 0;
    scannedEARs = 0;
    foundVunerabilities = 0;
    scanStart = 0;
    scanEnd = 0;
  }
};

class CReportVunerabilities {
 public:
  std::string file;
  std::string manifestVersion;
  std::string manifestVendor;
  bool detectedLog4j;
  bool detectedLog4j1x;
  bool detectedLog4j2x;
  bool detectedJNDILookupClass;
  bool detectedLog4jManifest;
  std::string log4jVersion;
  std::string log4jVendor;
  bool detectedVulnerableVersion;
  std::string cveStatus;

  CReportVunerabilities(std::string file, std::string manifestVersion,
                        std::string manifestVendor, bool detectedLog4j,
                        bool detectedLog4j1x, bool detectedLog4j2x,
                        bool detectedJNDILookupClass,
                        bool detectedLog4jManifest, std::string log4jVersion,
                        std::string log4jVendor, bool detectedVulnerableVersion,
                        std::string cveStatus) {
    this->file = file;
    this->manifestVersion = manifestVersion;
    this->manifestVendor = manifestVendor;
    this->detectedLog4j = detectedLog4j;
    this->detectedLog4j1x = detectedLog4j1x;
    this->detectedLog4j2x = detectedLog4j2x;
    this->detectedJNDILookupClass = detectedJNDILookupClass;
    this->detectedLog4jManifest = detectedLog4jManifest;
    this->log4jVersion = log4jVersion;
    this->log4jVendor = log4jVendor;
    this->detectedVulnerableVersion = detectedVulnerableVersion;
    this->cveStatus = cveStatus;
  }
};

class CCommandLineOptions {
 public:
  bool scanLocalDrives;
  bool scanNetworkDrives;
  bool scanFile;
  std::string file;
  bool scanDirectory;
  std::string directory;
  bool report;
  bool reportPretty;
  bool reportSig;
  bool verbose;
  bool no_logo;
  bool help;

  CCommandLineOptions() {
    scanLocalDrives = false;
    scanNetworkDrives = false;
    scanFile = false;
    file.clear();
    scanDirectory = false;
    directory.clear();
    report = false;
    reportPretty = false;
    reportSig = false;
    verbose = false;
    no_logo = false;
    help = false;
  }
};

constexpr char* qualys_program_data_locaton = "%ProgramData%\\Qualys";
constexpr char* report_sig_output_file = "log4j_findings.out";
constexpr char* report_sig_status_file = "status.txt";

FILE* status_file = nullptr;
std::vector<std::string> error_array;

CCommandLineOptions cmdline_options;
CReportSummary repSummary;
std::vector<CReportVunerabilities> repVulns;

bool UncompressContents(unzFile zf, std::string& str) {
  int32_t rv = ERROR_SUCCESS;
  char buf[4096];

  rv = unzOpenCurrentFile(zf);
  if (UNZ_OK == rv) {
    do {
      memset(buf, 0, sizeof(buf));
      rv = unzReadCurrentFile(zf, buf, sizeof(buf));
      if (rv < 0 || rv == 0) break;
      str.append(buf, rv);
    } while (rv > 0);
    unzCloseCurrentFile(zf);
  }

  return true;
}

bool SanitizeContents(std::string& str) {
  std::string::iterator iter = str.begin();
  while (iter != str.end()) {
    if (*iter == '\r') {
      iter = str.erase(iter);
    } else {
      ++iter;
    }
  }
  return true;
}

bool StripWhitespace(std::string& str) {
  while (1) {
    if (str.length() == 0) break;
    if (!isascii(str[0])) break;
    if (!isspace(str[0])) break;
    str.erase(0, 1);
  }

  int n = (int)str.length();
  while (n > 0) {
    if (!isascii(str[n - 1])) break;
    if (!isspace(str[n - 1])) break;
    n--;
  }
  str.erase(n, str.length() - n);
  return true;
}

bool GetDictionaryValue(std::string& dict, std::string name,
                        std::string defaultValue, std::string& value) {
  if (std::string::npos != dict.find(name.c_str(), 0)) {
    size_t pos = dict.find(name.c_str(), 0);
    size_t eol = dict.find("\n", pos);
    value = dict.substr(pos + name.size(), eol - (pos + name.size()));
    return true;
  }
  value = defaultValue;
  return false;
}

bool DirectoryExists(const char* dirPath) {
  if (dirPath == NULL) {
    return false;
  }
  DWORD fileAttr = GetFileAttributes(dirPath);
  return (fileAttr != INVALID_FILE_ATTRIBUTES &&
          (fileAttr & FILE_ATTRIBUTE_DIRECTORY));
}

std::string GetScanUtilityDirectory() {
  char path[MAX_PATH] = {0};
  std::string utility_dir;
  if (GetModuleFileName(NULL, path, _countof(path))) {
    utility_dir = path;
    std::string::size_type pos = std::string(utility_dir).find_last_of("\\");
    utility_dir = utility_dir.substr(0, pos);
  }
  return utility_dir;
}

bool ExpandEnvironmentVariables(const char* source, std::string& destination) {
  try {
    DWORD dwReserve = ExpandEnvironmentStrings(source, NULL, 0);
    if (dwReserve == 0) {
      return false;
    }
    destination.resize(dwReserve);
    DWORD dwWritten = ExpandEnvironmentStrings(source, &destination[0],
                                               (DWORD)destination.size());
    if (dwWritten == 0) {
      return false;
    }
    // dwWritten includes the null terminating character
    destination.resize(dwWritten - 1);
  } catch (std::bad_alloc&) {
    return false;
  }
  return true;
}

std::string GetReportDirectory() {
  std::string destination_dir;
  std::string report_dir;
  if (ExpandEnvironmentVariables(qualys_program_data_locaton,
                                 destination_dir)) {
    if (DirectoryExists(destination_dir.c_str())) {
      report_dir = destination_dir;
    }
  }
  if (report_dir.empty()) {
    report_dir = GetScanUtilityDirectory();
  }
  return report_dir;
}

std::string GetSignatureReportFilename() {
  return GetReportDirectory() + "\\" + report_sig_output_file;
}

std::string GetSignatureStatusFilename() {
  return GetReportDirectory() + "\\" + report_sig_status_file;
}

int LogStatusMessage(const char* fmt, ...) {
  int retval = 0;
  va_list ap;

  if (fmt == NULL) return 0;

  va_start(ap, fmt);
  retval = vfprintf(stdout, fmt, ap);
  va_end(ap);
  fwprintf(stdout, L"\n");

  if (status_file) {
    va_start(ap, fmt);
    retval = vfprintf(status_file, fmt, ap);
    va_end(ap);
    fwprintf(status_file, L"\n");
    fflush(status_file);
  }

  return retval;
}

int DumpGenericException(const char* szExceptionDescription,
                         DWORD dwExceptionCode, PVOID pExceptionAddress) {
  LogStatusMessage(
      "Unhandled Exception Detected - Reason: %s (0x%x) at address 0x%p\n\n",
      szExceptionDescription, dwExceptionCode, pExceptionAddress);
  return 0;
}

int DumpExceptionRecord(PEXCEPTION_POINTERS pExPtrs) {
  PVOID pExceptionAddress = pExPtrs->ExceptionRecord->ExceptionAddress;
  DWORD dwExceptionCode = pExPtrs->ExceptionRecord->ExceptionCode;

  switch (dwExceptionCode) {
    case 0xE06D7363:
      DumpGenericException("Out Of Memory (C++ Exception)", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_ACCESS_VIOLATION:
      char szStatus[256];
      char szSubStatus[256];
      strcpy_s(szStatus, "Access Violation");
      strcpy_s(szSubStatus, "");
      if (pExPtrs->ExceptionRecord->NumberParameters == 2) {
        switch (pExPtrs->ExceptionRecord->ExceptionInformation[0]) {
          case 0:  // read attempt
            sprintf_s(szSubStatus, "read attempt to address 0x%p",
                      (void*)pExPtrs->ExceptionRecord->ExceptionInformation[1]);
            break;
          case 1:  // write attempt
            sprintf_s(szSubStatus, "write attempt to address 0x%p",
                      (void*)pExPtrs->ExceptionRecord->ExceptionInformation[1]);
            break;
        }
      }
      LogStatusMessage(
          "Unhandled Exception Detected - Reason: % s(0x % x) at address 0x % "
          "p % s\n\n",
          szStatus, dwExceptionCode, pExceptionAddress, szSubStatus);
      break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      DumpGenericException("Data Type Misalignment", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_BREAKPOINT:
      DumpGenericException("Breakpoint Encountered", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_SINGLE_STEP:
      DumpGenericException("Single Instruction Executed", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      DumpGenericException("Array Bounds Exceeded", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
      DumpGenericException("Float Denormal Operand", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      DumpGenericException("Divide by Zero", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_INEXACT_RESULT:
      DumpGenericException("Float Inexact Result", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_INVALID_OPERATION:
      DumpGenericException("Float Invalid Operation", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_OVERFLOW:
      DumpGenericException("Float Overflow", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_STACK_CHECK:
      DumpGenericException("Float Stack Check", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_FLT_UNDERFLOW:
      DumpGenericException("Float Underflow", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      DumpGenericException("Integer Divide by Zero", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_INT_OVERFLOW:
      DumpGenericException("Integer Overflow", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_PRIV_INSTRUCTION:
      DumpGenericException("Privileged Instruction", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_IN_PAGE_ERROR:
      DumpGenericException("In Page Error", dwExceptionCode, pExceptionAddress);
      break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      DumpGenericException("Illegal Instruction", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      DumpGenericException("Noncontinuable Exception", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_STACK_OVERFLOW:
      DumpGenericException("Stack Overflow", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_INVALID_DISPOSITION:
      DumpGenericException("Invalid Disposition", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_GUARD_PAGE:
      DumpGenericException("Guard Page Violation", dwExceptionCode,
                           pExceptionAddress);
      break;
    case EXCEPTION_INVALID_HANDLE:
      DumpGenericException("Invalid Handle", dwExceptionCode,
                           pExceptionAddress);
      break;
    case CONTROL_C_EXIT:
      DumpGenericException("Ctrl+C Exit", dwExceptionCode, pExceptionAddress);
      break;
    default:
      DumpGenericException("Unknown exception", dwExceptionCode,
                           pExceptionAddress);
      break;
  }

  return 0;
}

LONG CALLBACK CatchUnhandledExceptionFilter(PEXCEPTION_POINTERS pExPtrs) {
  CHAR szMiniDumpFileName[MAX_PATH];
  HANDLE hDumpFile = NULL;
  SYSTEMTIME sysTime;
  SECURITY_ATTRIBUTES saMiniDumpSecurity;

  LogStatusMessage("Run status : Failed\n");

  // Attempt to dump an unhandled exception banner just in case things are
  // so bad that a minidump cannot be created.
  DumpExceptionRecord(pExPtrs);

  // Create a directory to dump the minidump files into
  SecureZeroMemory(&saMiniDumpSecurity, sizeof(saMiniDumpSecurity));
  saMiniDumpSecurity.nLength = sizeof(saMiniDumpSecurity);
  saMiniDumpSecurity.bInheritHandle = FALSE;

  // Construct a valid minidump filename that will be unique.
  // Use the '.mdmp' extension so it'll be recognize by the Windows debugging
  // tools.
  GetLocalTime(&sysTime);
  sprintf_s(szMiniDumpFileName,
            "%s\\%0.2d%0.2d%0.4d%d%0.2d%0.2d%0.4d.mdmp",
            GetScanUtilityDirectory().c_str(),
            sysTime.wMonth,
            sysTime.wDay,
            sysTime.wYear,
            sysTime.wHour,
            sysTime.wMinute,
            sysTime.wSecond,
            sysTime.wMilliseconds);

  LogStatusMessage("Creating minidump file %s with crash details.\n",
                   szMiniDumpFileName);

  // Create the file to dump the minidump data into...
  //
  hDumpFile = CreateFile(szMiniDumpFileName, GENERIC_WRITE, NULL, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  if (hDumpFile != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION eiMinidumpInfo;
    SecureZeroMemory(&eiMinidumpInfo, sizeof(eiMinidumpInfo));
    eiMinidumpInfo.ThreadId = GetCurrentThreadId();
    eiMinidumpInfo.ExceptionPointers = pExPtrs;
    eiMinidumpInfo.ClientPointers = FALSE;

    //
    // Write the Mini Dump to disk
    //
    if (!MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
            (MINIDUMP_TYPE)(MiniDumpNormal |
                            MiniDumpWithPrivateReadWriteMemory |
                            MiniDumpWithDataSegs | MiniDumpWithHandleData |
                            MiniDumpWithFullMemoryInfo |
                            MiniDumpWithThreadInfo |
                            MiniDumpWithUnloadedModules |
                            MiniDumpWithIndirectlyReferencedMemory),
            &eiMinidumpInfo, NULL, NULL)) {
      // Either the state of the process is beyond our ability to be able
      // to scape together a usable dump file or we are on XP/2k3 and
      // not all of the dump flags are supported.  Retry using dump flags
      // that are supported by XP.
      //
      if (!MiniDumpWriteDump(
              GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
              (MINIDUMP_TYPE)(MiniDumpNormal |
                              MiniDumpWithPrivateReadWriteMemory |
                              MiniDumpWithDataSegs | MiniDumpWithHandleData),
              &eiMinidumpInfo, NULL, NULL)) {
        // Well out XP/2k3 compatible list of parameters didn't work, it
        // doesn't look like we will be able to get anything useful.
        //
        // Close things down and delete the file if it exists.
        //
        SAFE_CLOSE_HANDLE(hDumpFile);
        DeleteFile(szMiniDumpFileName);

        LogStatusMessage("Failed to create minidump file %s.\n", szMiniDumpFileName);
      }
    }

    SAFE_CLOSE_HANDLE(hDumpFile);
  }

  TerminateProcess(GetCurrentProcess(),
                   pExPtrs->ExceptionRecord->ExceptionCode);
  return 0;
}

bool Log4jVersionCheck(std::string version) {
  int major = atoi(version.c_str());
  if (major < 2) return false;

  int minor = atoi(
      version
          .substr(version.find(".", 0) + 1, version.rfind(".", version.size()))
          .c_str());
  if (minor >= 15) return false;

  int revision =
      atoi(version
               .substr(version.rfind(".", version.size()) + 1,
                       version.size() - (version.rfind(".", version.size())))
               .c_str());
  if (revision >= 2) return false;

  return true;
}

int32_t ScanFileArchive(std::string file, std::string alternate) {
  int32_t rv = ERROR_SUCCESS;
  unsigned long bytesWritten = 0;
  unzFile zf = NULL;
  unz_file_info64 file_info;
  char* p = NULL;
  char buf[256];
  char filename[_MAX_PATH + 1];
  char tmpPath[_MAX_PATH + 1];
  char tmpFilename[_MAX_PATH + 1];
  bool foundLog4j = false;
  bool foundLog4j1x = false;
  bool foundLog4j2x = false;
  bool foundJNDILookupClass = false;
  bool foundManifest = false;
  bool foundLog4j1xPOM = false;
  bool foundLog4j2xPOM = false;
  bool foundLog4j2xCorePOM = false;
  bool foundManifestVendor = false;
  bool foundManifestVersion = false;
  bool foundLog4jManifest = false;
  bool foundVulnerableVersion = false;
  std::string manifest;
  std::string pomLog4j1x;
  std::string pomLog4j2x;
  std::string pomLog4j2xCore;
  std::string manifestVendor;
  std::string manifestVersion;
  std::string log4jVendor;
  std::string log4jVersion;

  if (!alternate.empty()) {
    zf = unzOpen64(alternate.c_str());
  } else {
    zf = unzOpen64(file.c_str());
  }
  if (NULL != zf) {
    //
    // Check to see if there is evidence of Log4j being in the archive
    //
    rv = unzGoToFirstFile(zf);
    if (UNZ_OK == rv) {
      do {
        rv = unzGetCurrentFileInfo64(zf, &file_info, filename,
                                     _countof(filename), NULL, 0, NULL, 0);

        if (UNZ_OK == rv) {
          p = strstr(filename, "org/apache/log4j");
          if (NULL != p) {
            foundLog4j = true;
            foundLog4j1x = true;
          }
          p = strstr(filename, "org/apache/logging/log4j");
          if (NULL != p) {
            foundLog4j = true;
            foundLog4j2x = true;
          }
          if (0 ==
              stricmp(
                  filename,
                  "org/apache/logging/log4j/core/lookup/JndiLookup.class")) {
            foundJNDILookupClass = true;
          }
          if (0 ==
              stricmp(filename, "META-INF/maven/log4j/log4j/pom.properties")) {
            foundLog4j1xPOM = true;
            UncompressContents(zf, pomLog4j1x);
          }
          p = strstr(filename, "META-INF/maven/org.apache.logging.log4j");
          if (NULL != p) {
            if (0 == stricmp(filename,
                             "META-INF/maven/org.apache.logging.log4j/"
                             "log4j-core/pom.properties")) {
              foundLog4j2xCorePOM = true;
              UncompressContents(zf, pomLog4j2xCore);
            } else {
              p = strstr(filename, "/pom.properties");
              if (NULL != p) {
                foundLog4j2xPOM = true;
                UncompressContents(zf, pomLog4j2x);
              }
            }
          }
          if (0 == stricmp(filename, "META-INF/MANIFEST.MF")) {
            foundManifest = true;
            UncompressContents(zf, manifest);
          }

          //
          // Add Support for nested archive files
          //
          p = &filename[0] + (strlen(filename) - 4);
          if ((0 == stricmp(p, ".jar")) || (0 == stricmp(p, ".war")) ||
              (0 == stricmp(p, ".ear")) || (0 == stricmp(p, ".zip"))) {
            if (0 == stricmp(p, ".jar")) {
              repSummary.scannedJARs++;
            }
            if (0 == stricmp(p, ".war")) {
              repSummary.scannedWARs++;
            }
            if (0 == stricmp(p, ".ear")) {
              repSummary.scannedEARs++;
            }
            if (0 == stricmp(p, ".zip")) {
              repSummary.scannedZIPs++;
            }

            GetTempPath(_countof(tmpPath), tmpPath);
            GetTempFileName(tmpPath, "qua", 0, tmpFilename);

            HANDLE h =
                CreateFile(tmpFilename, GENERIC_READ | GENERIC_WRITE, NULL,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);

            if (h != INVALID_HANDLE_VALUE) {
              rv = unzOpenCurrentFile(zf);
              if (UNZ_OK == rv) {
                do {
                  memset(buf, 0, sizeof(buf));
                  rv = unzReadCurrentFile(zf, buf, sizeof(buf));
                  if (rv < 0 || rv == 0) break;
                  WriteFile(h, buf, rv, &bytesWritten, NULL);
                } while (rv > 0);
                unzCloseCurrentFile(zf);
              }
              CloseHandle(h);

              std::string masked_filename = file + "!" + filename;
              std::string alternate_filename = tmpFilename;

              ScanFileArchive(masked_filename, alternate_filename);

              DeleteFile(alternate_filename.c_str());
            }
          }
        }

        rv = unzGoToNextFile(zf);
      } while (UNZ_OK == rv);
    }

    unzClose(zf);
  }

  //
  // If we have detected some evidence of Log4j then lets check to see if we can
  // detect CVE-2021-44228
  //
  if (foundLog4j) {
    std::string cveStatus;

    SanitizeContents(pomLog4j1x);
    SanitizeContents(pomLog4j2xCore);
    SanitizeContents(pomLog4j2x);
    SanitizeContents(manifest);

    if (foundLog4j1x) {
      GetDictionaryValue(pomLog4j1x, "artifactId=", "Unknown", log4jVendor);
      GetDictionaryValue(pomLog4j1x, "version=", "Unknown", log4jVersion);
    }
    if (foundLog4j2x) {
      if (foundLog4j2xCorePOM) {
        GetDictionaryValue(pomLog4j2xCore, "artifactId=", "Unknown",
                           log4jVendor);
        GetDictionaryValue(pomLog4j2xCore, "version=", "Unknown", log4jVersion);
      } else {
        GetDictionaryValue(pomLog4j2x, "artifactId=", "Unknown", log4jVendor);
        GetDictionaryValue(pomLog4j2x, "version=", "Unknown", log4jVersion);
      }
    }

    if (foundManifest) {
      foundManifestVendor = GetDictionaryValue(
          manifest, "Implementation-Vendor-Id:", "Unknown", manifestVendor);
      if (!foundManifestVendor) {
        foundManifestVendor = GetDictionaryValue(
            manifest, "Bundle-Vendor:", "Unknown", manifestVendor);
      }
      foundManifestVersion = GetDictionaryValue(
          manifest, "Implementation-Version:", "Unknown", manifestVersion);
      if (!foundManifestVersion) {
        foundManifestVersion = GetDictionaryValue(
            manifest, "Bundle-Version:", "Unknown", manifestVersion);
      }

      StripWhitespace(manifestVendor);
      StripWhitespace(manifestVersion);

      if (foundManifestVendor) {
        if (std::string::npos != manifestVendor.find("log4j", 0)) {
          foundLog4jManifest = true;
        }
        if (std::string::npos !=
            manifestVendor.find("org.apache.logging.log4j", 0)) {
          foundLog4jManifest = true;
        }
      }
    }

    if (foundLog4j1xPOM || foundLog4j2xPOM || foundLog4j2xCorePOM) {
      if (Log4jVersionCheck(log4jVersion)) {
        foundVulnerableVersion = true;
      }
    }

    if (foundLog4j2x && foundJNDILookupClass && foundVulnerableVersion) {
      repSummary.foundVunerabilities++;
      cveStatus = "Potentially Vulnerable";
    } else if (!foundJNDILookupClass && !foundManifestVendor &&
               !foundManifestVersion) {
      cveStatus = "N/A";
    } else if (!foundJNDILookupClass && foundLog4j2x && foundLog4jManifest) {
      cveStatus = "Mitigated";
    } else if (foundJNDILookupClass && foundLog4j2x &&
               !foundVulnerableVersion) {
      cveStatus = "Mitigated";
    } else if (!foundJNDILookupClass && foundLog4j1x &&
               !foundVulnerableVersion) {
      cveStatus = "N/A";
    } else {
      cveStatus = "Unknown";
    }

    repVulns.push_back(CReportVunerabilities(
        file, manifestVersion, manifestVendor, foundLog4j, foundLog4j1x,
        foundLog4j2x, foundJNDILookupClass, foundLog4jManifest, log4jVersion,
        log4jVendor, foundVulnerableVersion, cveStatus));

    if (!cmdline_options.no_logo) {
      printf(
          "Log4j Found: '%s' ( Manifest Vendor: %s, Manifest Version: %s, JDNI "
          "Class: %s, Log4j Vendor: %s, Log4j Version: %s, CVE Status: %s )\n",
          file.c_str(), manifestVendor.c_str(), manifestVersion.c_str(),
          foundJNDILookupClass ? "Found" : "NOT Found", log4jVendor.c_str(),
          log4jVersion.c_str(), cveStatus.c_str());
    }
  }

  return rv;
}

int32_t ScanFile(std::string file) {
  int32_t rv = ERROR_SUCCESS;
  char drive[_MAX_DRIVE];
  char dir[_MAX_DIR];
  char fname[_MAX_FNAME];
  char ext[_MAX_EXT];

  if (0 == _splitpath_s(file.c_str(), drive, dir, fname, ext)) {
    if (0 == stricmp(ext, ".jar")) {
      repSummary.scannedJARs++;
      rv = ScanFileArchive(file, "");
    }
    if (0 == stricmp(ext, ".war")) {
      repSummary.scannedWARs++;
      rv = ScanFileArchive(file, "");
    }
    if (0 == stricmp(ext, ".ear")) {
      repSummary.scannedEARs++;
      rv = ScanFileArchive(file, "");
    }
    if (0 == stricmp(ext, ".zip")) {
      repSummary.scannedZIPs++;
      rv = ScanFileArchive(file, "");
    }

  } else {
    rv = errno;
  }

  return rv;
}

int32_t ScanDirectory(std::string directory) {
  int32_t rv = ERROR_SUCCESS;
  std::string search = directory + std::string("*.*");
  WIN32_FIND_DATA FindFileData;
  HANDLE hFind;

  hFind = FindFirstFile(search.c_str(), &FindFileData);
  if (hFind == INVALID_HANDLE_VALUE) {
    rv = GetLastError();
  } else {
    do {
      std::string filename(FindFileData.cFileName);

      if ((filename.size() == 1) && (filename == ".")) continue;
      if ((filename.size() == 2) && (filename == "..")) continue;
      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ==
          FILE_ATTRIBUTE_REPARSE_POINT)
        continue;
      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ==
          FILE_ATTRIBUTE_DEVICE)
        continue;
      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_OFFLINE) ==
          FILE_ATTRIBUTE_OFFLINE)
        continue;
      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) ==
          FILE_ATTRIBUTE_TEMPORARY)
        continue;
      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_VIRTUAL) ==
          FILE_ATTRIBUTE_VIRTUAL)
        continue;

      if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
          FILE_ATTRIBUTE_DIRECTORY) {
        repSummary.scannedDirectories++;

        std::string dir =
            directory + std::string(FindFileData.cFileName) + std::string("\\");
        rv = ScanDirectory(dir);
        if (ERROR_SUCCESS != rv) {
          if (cmdline_options.verbose) {
            printf("Failed to process directory '%s' (rv: %d)\n", dir.c_str(),
                   rv);
          }
          if (rv != -100) {
            char err[1024] = {};
            sprintf_s(err, "Failed to process directory '%s' (rv: %d)",
                      dir.c_str(), rv);

            error_array.push_back(err);
          }
        }

        // TODO: Look for suspect directory structures containing raw log4j java
        // classes
        //

      } else {
        repSummary.scannedFiles++;

        std::string file = directory + std::string(FindFileData.cFileName);
        rv = ScanFile(file);
        if (ERROR_SUCCESS != rv) {
          if (cmdline_options.verbose) {
            printf("Failed to process file '%s' (rv: %d)\n", file.c_str(), rv);
          }
          if (rv != -100) {
            char err[1024] = {};
            sprintf_s(err, "Failed to process file '%s' (rv: %d)\n",
                      file.c_str(), rv);

            error_array.push_back(err);
          }
        }
      }

    } while (FindNextFile(hFind, &FindFileData));
    FindClose(hFind);
  }

  return rv;
}

int32_t ScanLocalDrives() {
  int32_t rv = ERROR_SUCCESS;
  DWORD rt = 0;
  char drives[256];

  strcpy_s(drives, "");
  rt = GetLogicalDriveStrings(_countof(drives), drives);
  for (uint32_t i = 0; i < rt; i += 4) {
    char* drive = &drives[i];
    DWORD type = GetDriveType(drive);
    if ((DRIVE_FIXED == type) || (DRIVE_RAMDISK == type)) {
      ScanDirectory(drive);
    }
  }

  return rv;
}

int32_t ScanNetworkDrives() {
  int32_t rv = ERROR_SUCCESS;
  DWORD rt = 0;
  char drives[256];

  strcpy_s(drives, "");
  rt = GetLogicalDriveStrings(_countof(drives), drives);
  for (uint32_t i = 0; i < rt; i += 4) {
    char* drive = &drives[i];
    DWORD type = GetDriveType(drive);
    if (DRIVE_REMOTE == type) {
      ScanDirectory(drive);
    }
  }

  return rv;
}

int32_t GenerateReportSummary(rapidjson::Document& doc) {
  int32_t rv = ERROR_SUCCESS;

  rapidjson::Value vScanDate(rapidjson::kStringType);
  rapidjson::Value vScanDuration(rapidjson::kNumberType);
  rapidjson::Value vScannedFiles(rapidjson::kNumberType);
  rapidjson::Value vScannedDirectories(rapidjson::kNumberType);
  rapidjson::Value vScannedJARs(rapidjson::kNumberType);
  rapidjson::Value vScannedWARs(rapidjson::kNumberType);
  rapidjson::Value vScannedEARs(rapidjson::kNumberType);
  rapidjson::Value vScannedZIPs(rapidjson::kNumberType);
  rapidjson::Value vVulnerabilitiesFound(rapidjson::kNumberType);
  rapidjson::Value oSummary(rapidjson::kObjectType);

  char buf[64] = {0};
  struct tm* tm = NULL;

  tm = localtime((time_t*)&repSummary.scanStart);
  strftime(buf, _countof(buf) - 1, "%FT%T%z", tm);

  vScanDate.SetString(&buf[0], doc.GetAllocator());
  vScanDuration.SetInt64(repSummary.scanEnd - repSummary.scanStart);
  vScannedFiles.SetInt64(repSummary.scannedFiles);
  vScannedDirectories.SetInt64(repSummary.scannedDirectories);
  vScannedJARs.SetInt64(repSummary.scannedJARs);
  vScannedWARs.SetInt64(repSummary.scannedWARs);
  vScannedEARs.SetInt64(repSummary.scannedEARs);
  vScannedZIPs.SetInt64(repSummary.scannedZIPs);
  vVulnerabilitiesFound.SetInt64(repSummary.foundVunerabilities);

  oSummary.AddMember("scanDuration", vScanDuration, doc.GetAllocator());
  oSummary.AddMember("scannedFiles", vScannedFiles, doc.GetAllocator());
  oSummary.AddMember("scannedDirectories", vScannedDirectories,
                     doc.GetAllocator());
  oSummary.AddMember("scannedJARs", vScannedJARs, doc.GetAllocator());
  oSummary.AddMember("scannedWARs", vScannedWARs, doc.GetAllocator());
  oSummary.AddMember("scannedEARs", vScannedEARs, doc.GetAllocator());
  oSummary.AddMember("scannedZIPs", vScannedZIPs, doc.GetAllocator());
  oSummary.AddMember("vulnerabilitiesFound", vVulnerabilitiesFound,
                     doc.GetAllocator());

  doc.AddMember("scanSummary", oSummary, doc.GetAllocator());

  return rv;
}

int32_t GenerateReportDetail(rapidjson::Document& doc) {
  int32_t rv = ERROR_SUCCESS;
  rapidjson::Value oDetails(rapidjson::kArrayType);

  for (size_t i = 0; i < repVulns.size(); i++) {
    CReportVunerabilities vuln = repVulns[i];

    rapidjson::Value vFile(rapidjson::kStringType);
    rapidjson::Value vManifestVendor(rapidjson::kStringType);
    rapidjson::Value vManifestVersion(rapidjson::kStringType);
    rapidjson::Value vDetectedLog4j(rapidjson::kTrueType);
    rapidjson::Value vDetectedLog4j1x(rapidjson::kTrueType);
    rapidjson::Value vDetectedLog4j2x(rapidjson::kTrueType);
    rapidjson::Value vDetectedJNDILookupClass(rapidjson::kTrueType);
    rapidjson::Value vDetectedLog4jManifest(rapidjson::kTrueType);
    rapidjson::Value vLog4jVendor(rapidjson::kStringType);
    rapidjson::Value vLog4jVersion(rapidjson::kStringType);
    rapidjson::Value vDetectedVulnerableVersion(rapidjson::kTrueType);
    rapidjson::Value vCVEStatus(rapidjson::kStringType);
    rapidjson::Value oDetail(rapidjson::kObjectType);

    vFile.SetString(vuln.file.c_str(), doc.GetAllocator());
    vManifestVendor.SetString(vuln.manifestVendor.c_str(), doc.GetAllocator());
    vManifestVersion.SetString(vuln.manifestVersion.c_str(),
                               doc.GetAllocator());
    vDetectedLog4j.SetBool(vuln.detectedLog4j);
    vDetectedLog4j1x.SetBool(vuln.detectedLog4j1x);
    vDetectedLog4j2x.SetBool(vuln.detectedLog4j2x);
    vDetectedJNDILookupClass.SetBool(vuln.detectedJNDILookupClass);
    vDetectedLog4jManifest.SetBool(vuln.detectedLog4jManifest);
    vLog4jVendor.SetString(vuln.log4jVendor.c_str(), doc.GetAllocator());
    vLog4jVersion.SetString(vuln.log4jVersion.c_str(), doc.GetAllocator());
    vDetectedVulnerableVersion.SetBool(vuln.detectedVulnerableVersion);
    vCVEStatus.SetString(vuln.cveStatus.c_str(), doc.GetAllocator());

    oDetail.AddMember("file", vFile, doc.GetAllocator());
    oDetail.AddMember("manifestVendor", vManifestVendor, doc.GetAllocator());
    oDetail.AddMember("manifestVersion", vManifestVersion, doc.GetAllocator());
    oDetail.AddMember("detectedLog4j", vDetectedLog4j, doc.GetAllocator());
    oDetail.AddMember("detectedLog4j1x", vDetectedLog4j1x, doc.GetAllocator());
    oDetail.AddMember("detectedLog4j2x", vDetectedLog4j2x, doc.GetAllocator());
    oDetail.AddMember("detectedJNDILookupClass", vDetectedJNDILookupClass,
                      doc.GetAllocator());
    oDetail.AddMember("detectedLog4jManifest", vDetectedLog4jManifest,
                      doc.GetAllocator());
    oDetail.AddMember("log4jVendor", vLog4jVendor, doc.GetAllocator());
    oDetail.AddMember("log4jVersion", vLog4jVersion, doc.GetAllocator());
    oDetail.AddMember("detectedVulnerableVersion", vDetectedVulnerableVersion,
                      doc.GetAllocator());
    oDetail.AddMember("cveStatus", vCVEStatus, doc.GetAllocator());

    oDetails.PushBack(oDetail, doc.GetAllocator());
  }

  doc.AddMember("scanDetails", oDetails, doc.GetAllocator());
  return rv;
}

int32_t GenerateJSONReport() {
  int32_t rv = ERROR_SUCCESS;
  rapidjson::Document doc;
  rapidjson::StringBuffer buffer;

  doc.Parse("{}");

  GenerateReportSummary(doc);
  GenerateReportDetail(doc);

  if (cmdline_options.reportPretty) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
  } else {
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
  }

  printf("%s", buffer.GetString());
  return rv;
}

int32_t GenerateSignatureReport() {
  int32_t rv = ERROR_SUCCESS;

  // signature output should go into a file always
  // 1. First check if %programdata%\Qualys\QualysAgent exist
  // 2. If not exist then current direcotry will be used

  FILE* signature_file = nullptr;
  fopen_s(&signature_file, GetSignatureReportFilename().c_str(), "w+");

  if (signature_file) {
    for (size_t i = 0; i < repVulns.size(); i++) {
      CReportVunerabilities vuln = repVulns[i];

      fprintf_s(signature_file,
                "Source: Manifest Vendor: %s, Manifest Version: %s, JDNI Class: %s, Log4j Vendor: %s, Log4j Version: %s, CVE Status: %s\n",
                vuln.manifestVendor.c_str(),
                vuln.manifestVersion.c_str(),
                vuln.detectedJNDILookupClass ? "Found" : "NOT Found",
                vuln.log4jVendor.c_str(),
                vuln.log4jVersion.c_str(),
                vuln.cveStatus.c_str());
      fprintf_s(signature_file, "JNDI-Class: JNDI Class %s\n", vuln.detectedJNDILookupClass ? "Found" : "NOT Found");
      fprintf_s(signature_file, "CVE-Status: %s\n", vuln.cveStatus.c_str());
      fprintf_s(signature_file, "Path=%s\n", vuln.file.c_str());
      fprintf_s(signature_file, "%s %s\n", vuln.log4jVendor.c_str(),
                vuln.log4jVersion.c_str());
      fprintf_s(signature_file,
                "------------------------------------------------------------------------\n");
    }

    fclose(signature_file);
  } else {
    for (size_t i = 0; i < repVulns.size(); i++) {
      CReportVunerabilities vuln = repVulns[i];

      printf("Source: Manifest Vendor: %s, Manifest Version: %s, JDNI Class: %s, Log4j Vendor: %s, Log4j Version: %s, CVE Status: %s\n",
             vuln.manifestVendor.c_str(),
             vuln.manifestVersion.c_str(),
             vuln.detectedJNDILookupClass ? "Found" : "NOT Found",
             vuln.log4jVendor.c_str(),
             vuln.log4jVersion.c_str(),
             vuln.cveStatus.c_str());
      printf("JNDI-Class: JNDI Class %s\n", vuln.detectedJNDILookupClass ? "Found" : "NOT Found");
      printf("CVE-Status: %s\n", vuln.cveStatus.c_str());
      printf("Path=%s\n", vuln.file.c_str());
      printf("%s %s\n", vuln.log4jVendor.c_str(), vuln.log4jVersion.c_str());
      printf("------------------------------------------------------------------------\n");
    }
  }

  return rv;
}

int32_t PrintHelp(_In_ int32_t argc, _In_ char* argv[]) {
  int32_t rv = ERROR_SUCCESS;

  printf("/scan\n");
  printf("  Scan local drives for vunerable JAR, WAR, EAR, ZIP files used by various Java applications.\n");
  printf("/scan_network\n");
  printf("  Scan network drives for vunerable JAR, WAR, EAR, ZIP files used by various Java applications.\n");
  printf("/scan_directory \"C:\\Some\\Path\"\n");
  printf("  Scan a specific directory for vunerable JAR, WAR, EAR, ZIP files used by various Java applications.\n");
  printf("/scan_file \"C:\\Some\\Path\\Some.jar\"\n");
  printf("  Scan a specific file for CVE-2021-44228.\n");
  printf("/report\n");
  printf("  Generate a JSON report of possible detections of CVE-2021-44228.\n");
  printf("/report_pretty\n");
  printf("  Generate a human readable JSON report of possible detections of CVE-2021-44228.\n");
  printf("/report_sig\n");
  printf("  Generate a signature report of possible detections of CVE-2021-44228.\n");
  printf("\n");

  return rv;
}

int32_t ProcessCommandLineOptions(int32_t argc, char* argv[]) {
  int32_t rv = ERROR_SUCCESS;

  for (int32_t i = 1; i < argc; i++) {
    if (0) {
    } else if (ARG(scan)) {
      cmdline_options.scanLocalDrives = true;
    } else if (ARG(scan_network)) {
      cmdline_options.scanNetworkDrives = true;
    } else if (ARG(scan_file) && ARGPARAMCOUNT(1)) {
      cmdline_options.scanFile = true;
      cmdline_options.file = argv[i + 1];
    } else if (ARG(scan_directory) && ARGPARAMCOUNT(1)) {
      cmdline_options.scanDirectory = true;
      cmdline_options.directory = argv[i + 1];
    } else if (ARG(report)) {
      cmdline_options.no_logo = true;
      cmdline_options.report = true;
    } else if (ARG(report_pretty)) {
      cmdline_options.no_logo = true;
      cmdline_options.report = true;
      cmdline_options.reportPretty = true;
    } else if (ARG(report_sig)) {
      cmdline_options.no_logo = true;
      cmdline_options.report = true;
      cmdline_options.reportSig = true;
    } else if (ARG(nologo)) {
      cmdline_options.no_logo = true;
    } else if (ARG(v) || ARG(verbose)) {
      cmdline_options.verbose = true;
    } else if (ARG(h) || ARG(help)) {
      cmdline_options.help = true;
    }
  }

  //
  // Check to make sure the directory path is normalized
  //
  if (cmdline_options.scanDirectory) {
    if ((0 == cmdline_options.directory.substr(0, 1).compare("\"")) ||
        (0 == cmdline_options.directory.substr(0, 1).compare("'"))) {
      cmdline_options.directory.erase(0, 1);
    }
    if ((0 == cmdline_options.directory.substr(cmdline_options.directory.size() - 1, 1).compare("\"")) ||
        (0 == cmdline_options.directory.substr(cmdline_options.directory.size() - 1, 1).compare("'"))) {
      cmdline_options.directory.erase(cmdline_options.directory.size() - 1, 1);
    }
    if (0 != cmdline_options.directory.substr(cmdline_options.directory.size() - 1, 1).compare("\\")) {
      cmdline_options.directory += "\\";
    }
  }

  return rv;
}

int32_t __cdecl main(int32_t argc, char* argv[]) {
  int32_t rv = ERROR_SUCCESS;

  SetUnhandledExceptionFilter(CatchUnhandledExceptionFilter);

#ifndef _WIN64
  using typeWow64DisableWow64FsRedirection = BOOL(WINAPI*)(PVOID OlValue);
  typeWow64DisableWow64FsRedirection Wow64DisableWow64FsRedirection;
  BOOL bIs64BitWindows = FALSE;
  PVOID pHandle;

  if (!IsWow64Process(GetCurrentProcess(), &bIs64BitWindows)) {
    printf("Failed to determine if process is running as WoW64.\n");
    goto END;
  }

  if (bIs64BitWindows) {
    Wow64DisableWow64FsRedirection =
        (typeWow64DisableWow64FsRedirection)GetProcAddress(
            GetModuleHandle("Kernel32.DLL"), "Wow64DisableWow64FsRedirection");

    if (Wow64DisableWow64FsRedirection) {
      Wow64DisableWow64FsRedirection(&pHandle);
    }
  }
#endif

  rv = ProcessCommandLineOptions(argc, argv);
  if (ERROR_SUCCESS != rv) {
    printf("Failed to process command line options.\n");
    goto END;
  }

  if (!cmdline_options.no_logo) {
    printf("Qualys CVE-2021-44228 Log4j Vulnerability Scanner 1.2.5\n");
    printf("https://www.qualys.com/\n\n");
  }

  if (cmdline_options.help) {
    PrintHelp(argc, argv);
    goto END;
  }

  if (!cmdline_options.scanLocalDrives && !cmdline_options.scanNetworkDrives &&
      !cmdline_options.scanDirectory && !cmdline_options.scanFile) {
    cmdline_options.scanLocalDrives = true;
  }

  if (cmdline_options.reportSig) {
    fopen_s(&status_file, GetSignatureStatusFilename().c_str(), "w");
  }

  repSummary.scanStart = time(0);

  if (!cmdline_options.reportSig) {
    char buf[64] = {0};
    struct tm* tm = NULL;

    tm = localtime((time_t*)&repSummary.scanStart);
    strftime(buf, _countof(buf) - 1, "%FT%T%z", tm);

    LogStatusMessage("Scan start time : %s\n", buf);
  }


  if (cmdline_options.scanLocalDrives) {
    if (!cmdline_options.no_logo) {
      printf("Scanning Local Drives...\n");
    }
    ScanLocalDrives();
  }

  if (cmdline_options.scanNetworkDrives) {
    if (!cmdline_options.no_logo) {
      printf("Scanning Network Drives...\n");
    }
    ScanNetworkDrives();
  }

  if (cmdline_options.scanDirectory) {
    if (!cmdline_options.no_logo) {
      printf("Scanning '%s'...\n", cmdline_options.directory.c_str());
    }
    ScanDirectory(cmdline_options.directory);
  }

  if (cmdline_options.scanFile) {
    if (!cmdline_options.no_logo) {
      printf("Scanning '%s'...\n", cmdline_options.file.c_str());
    }
    ScanFile(cmdline_options.file);
  }

  repSummary.scanEnd = time(0);

  if (!cmdline_options.reportSig) {
    char buf[64] = {0};
    struct tm* tm = NULL;

    tm = localtime((time_t*)&repSummary.scanEnd);
    strftime(buf, _countof(buf) - 1, "%FT%T%z", tm);

    LogStatusMessage("\nScan end time : %s\n", buf);
  }


  if (!cmdline_options.no_logo) {
    char buf[64] = {0};
    struct tm* tm = NULL;

    tm = localtime((time_t*)&repSummary.scanStart);
    strftime(buf, _countof(buf) - 1, "%FT%T%z", tm);

    printf("\nScan Summary:\n");
    printf("\tScan Date:\t\t %s\n", buf);
    printf("\tScan Duration:\t\t %lld Seconds\n",
           repSummary.scanEnd - repSummary.scanStart);
    printf("\tFiles Scanned:\t\t %lld\n", repSummary.scannedFiles);
    printf("\tDirectories Scanned:\t %lld\n", repSummary.scannedDirectories);
    printf("\tJAR(s) Scanned:\t\t %lld\n", repSummary.scannedJARs);
    printf("\tWAR(s) Scanned:\t\t %lld\n", repSummary.scannedWARs);
    printf("\tEAR(s) Scanned:\t\t %lld\n", repSummary.scannedEARs);
    printf("\tZIP(s) Scanned:\t\t %lld\n", repSummary.scannedZIPs);
    printf("\tVulnerabilities Found:\t %lld\n", repSummary.foundVunerabilities);
  }

  if (cmdline_options.report) {
    if (!cmdline_options.reportSig) {
      GenerateJSONReport();
    } else {
      GenerateSignatureReport();
    }
  }

END:

  if (cmdline_options.reportSig) {
    if (error_array.empty()) {
      LogStatusMessage("Run status : Success\n");
      LogStatusMessage("Result file location : %s\n", GetSignatureReportFilename().c_str());
    } else {
      LogStatusMessage("Run status : Partially Successful\n");
      LogStatusMessage("Result file location : %s\n", GetSignatureReportFilename().c_str());

      LogStatusMessage("Errors :\n");
      for (const auto& e : error_array) {
        LogStatusMessage("%s\n", e.c_str());
      }
    }
  }
  if (status_file) {
    fclose(status_file);
  }

  return rv;
}
