// -*-Mode: C++;-*-

//*BeginLicense**************************************************************
//
//---------------------------------------------------------------------------
// TAZeR (github.com/pnnl/tazer/)
//---------------------------------------------------------------------------
//
// Copyright ((c)) 2019, Battelle Memorial Institute
//
// 1. Battelle Memorial Institute (hereinafter Battelle) hereby grants
//    permission to any person or entity lawfully obtaining a copy of
//    this software and associated documentation files (hereinafter "the
//    Software") to redistribute and use the Software in source and
//    binary forms, with or without modification.  Such person or entity
//    may use, copy, modify, merge, publish, distribute, sublicense,
//    and/or sell copies of the Software, and may permit others to do
//    so, subject to the following conditions:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimers.
//
//    * Redistributions in binary form must reproduce the above
//      copyright notice, this list of conditions and the following
//      disclaimer in the documentation and/or other materials provided
//      with the distribution.
//
//    * Other than as used herein, neither the name Battelle Memorial
//      Institute or Battelle may be used in any form whatsoever without
//      the express written consent of Battelle.
//
// 2. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
//    CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
//    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//    MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//    DISCLAIMED. IN NO EVENT SHALL BATTELLE OR CONTRIBUTORS BE LIABLE
//    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
//    OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
//    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//    LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
//    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
//    DAMAGE.
//
// ***
//
// This material was prepared as an account of work sponsored by an
// agency of the United States Government.  Neither the United States
// Government nor the United States Department of Energy, nor Battelle,
// nor any of their employees, nor any jurisdiction or organization that
// has cooperated in the development of these materials, makes any
// warranty, express or implied, or assumes any legal liability or
// responsibility for the accuracy, completeness, or usefulness or any
// information, apparatus, product, software, or process disclosed, or
// represents that its use would not infringe privately owned rights.
//
// Reference herein to any specific commercial product, process, or
// service by trade name, trademark, manufacturer, or otherwise does not
// necessarily constitute or imply its endorsement, recommendation, or
// favoring by the United States Government or any agency thereof, or
// Battelle Memorial Institute. The views and opinions of authors
// expressed herein do not necessarily state or reflect those of the
// United States Government or any agency thereof.
//
//                PACIFIC NORTHWEST NATIONAL LABORATORY
//                             operated by
//                               BATTELLE
//                               for the
//                  UNITED STATES DEPARTMENT OF ENERGY
//                   under Contract DE-AC05-76RL01830
//
//*EndLicense****************************************************************

#include "Config.h"
#include "ConnectionPool.h"
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
//#include "ErrorTester.h"
#include "InputFile.h"
#include "OutputFile.h"
#include "RSocketAdapter.h"
#include "Request.h"
#include "ReaderWriterLock.h"
#include "TazerFile.h"
#include "TazerFileDescriptor.h"
#include "TazerFileStream.h"
#include "Timer.h"
#include "Trackable.h"
#include "UnixIO.h"
#include "ThreadPool.h"
#include "PriorityThreadPool.h"

#define ADD_THROW __THROW

//#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#define DPRINTF(...)

static Timer timer;

std::once_flag log_flag;
bool init = false;
ReaderWriterLock vLock;

std::unordered_set<std::string>* track_files = NULL; //this is a pointer cause we access in ((attribute)) constructor and initialization isnt guaranteed
static std::unordered_set<int> track_fd;
static std::unordered_set<FILE *> track_fp;
static std::unordered_set<int> ignore_fd;
static std::unordered_set<FILE *> ignore_fp;

unixopen_t unixopen = NULL;
unixopen_t unixopen64 = NULL;
unixclose_t unixclose = NULL;
unixread_t unixread = NULL;
unixwrite_t unixwrite = NULL;
unixlseek_t unixlseek = NULL;
unixlseek64_t unixlseek64 = NULL;
unixxstat_t unixxstat = NULL;
unixxstat64_t unixxstat64 = NULL;
unixxstat_t unixlxstat = NULL;
unixxstat64_t unixlxstat64 = NULL;
unixfsync_t unixfsync = NULL;
unixfopen_t unixfopen = NULL;
unixfopen_t unixfopen64 = NULL;
unixfclose_t unixfclose = NULL;
unixfread_t unixfread = NULL;
unixfwrite_t unixfwrite = NULL;
unixftell_t unixftell = NULL;
unixfseek_t unixfseek = NULL;
unixrewind_t unixrewind = NULL;
unixfgetc_t unixfgetc = NULL;
unixfgets_t unixfgets = NULL;
unixfputc_t unixfputc = NULL;
unixfputs_t unixfputs = NULL;
unixflockfile_t unixflockfile = NULL;
unixftrylockfile_t unixftrylockfile = NULL;
unixfunlockfile_t unixfunlockfile = NULL;
unixfflush_t unixfflush = NULL;
unixfeof_t unixfeof = NULL;
unixreadv_t unixreadv = NULL;
unixwritev_t unixwritev = NULL;

int removeStr(char *s, const char *r);

/*Templating*************************************************************************************************/

inline bool splitter(std::string tok, std::string full, std::string &path, std::string &file) {
    size_t pos = full.find(tok);
    if (pos != std::string::npos) {
        path = full.substr(0, pos + tok.length());
        if (path.length() < full.length())
            file = full.substr(path.length() + 1);
        else
            file = full;
        return true;
    }
    path = full;
    file = full;
    return false;
}

inline bool checkMeta(const char *pathname, std::string &path, std::string &file, TazerFile::Type &type) {
    if (strstr(pathname, ".meta.")) {
        std::string full(pathname);
        TazerFile::Type tokType[3] = {TazerFile::Input, TazerFile::Output, TazerFile::Local};
        std::string tok[3] = {".meta.in", ".meta.out", ".meta.local"};
        for (unsigned int i = 0; i < 3; i++) {
            if (splitter(tok[i], full, path, file)) {
                type = tokType[i];
                DPRINTF("Path: %s File: %s\n", path.c_str(), file.c_str());
                return true;
            }
        }
    }
    DPRINTF("~ %s Path: %s File: %s\n", pathname, path.c_str(), file.c_str());
    return false;
}

inline bool trackFile(int fd) { return init ? track_fd.count(fd) : false; }
inline bool trackFile(FILE *fp) { return init ? track_fp.count(fp) : false; }
inline bool trackFile(const char *name) { return init ? track_files->count(name) : false; }

inline bool ignoreFile(uint64_t fd) { return init ? ignore_fd.count(fd) : false; }
inline bool ignoreFile(FILE *fp) { return init ? ignore_fp.count(fp) : false; }
inline bool ignoreFile(std::string pathname) {
    if (init) {
        if (pathname.find(Config::filelockCacheFilePath) != std::string::npos) {
            return true;
        }
        if (pathname.find(Config::fileCacheFilePath) != std::string::npos) {
            return true;
        }
        if (pathname.find(Config::burstBufferCacheFilePath) != std::string::npos) {
            return true;
        }
    }
    return false;
}

template <typename T>
inline void removeFileStream(T posixFun, FILE *fp) {}
inline void removeFileStream(unixfclose_t posixFun, FILE *fp) {
    if (posixFun == unixfclose)
        TazerFileStream::removeStream(fp);
}

template <typename Func, typename FuncLocal, typename... Args>
inline auto innerWrapper(int fd, bool &isTazerFile, Func tazerFun, FuncLocal localFun, Args... args) {
    TazerFile *file = NULL;
    unsigned int fp = 0;
    if (init && TazerFileDescriptor::lookupTazerFileDescriptor(fd, file, fp)) {
        isTazerFile = true;

        return tazerFun(file, fp, args...);
    }
    return localFun(args...);
}

template <typename Func, typename FuncLocal, typename... Args>
inline auto innerWrapper(FILE *fp, bool &isTazerFile, Func tazerFun, FuncLocal localFun, Args... args) {
    if (init) {
        ReaderWriterLock *lock = NULL;
        int fd = TazerFileStream::lookupStream(fp, lock);
        if (fd != -1) {
            isTazerFile = true;
            lock->writerLock();
            TazerFile *file = NULL;
            unsigned int pos = 0;
            TazerFileDescriptor::lookupTazerFileDescriptor(fd, file, pos);
            auto ret = tazerFun(file, pos, fd, args...);
            lock->writerUnlock();
            removeFileStream(localFun, fp);
            return ret;
        }
    }
    return localFun(args...);
}

template <typename Func, typename FuncPosix, typename... Args>
inline auto innerWrapper(const char *pathname, bool &isTazerFile, Func tazerFun, FuncPosix posixFun, Args... args) {
    std::string path;
    std::string file;
    TazerFile::Type type;
    if (init && checkMeta(pathname, path, file, type)) {
        isTazerFile = true;
        return tazerFun(file, path, type, args...);
    }
    return posixFun(args...);
}

template <typename T1, typename T2>
inline void addToSet(std::unordered_set<int> &set, T1 value, T2 posixFun) {}
inline void addToSet(std::unordered_set<int> &set, int value, unixopen_t posixFun) {
    if (posixFun == unixopen || posixFun == unixopen64)
        set.emplace(value);
}
inline void addToSet(std::unordered_set<FILE *> &set, FILE *value, unixfopen_t posixFun) {
    if (posixFun == unixfopen)
        set.emplace(value);
}

template <typename T1, typename T2>
inline void removeFromSet(std::unordered_set<int> &set, T1 value, T2 posixFun) {}
inline void removeFromSet(std::unordered_set<int> &set, int value, unixclose_t posixFun) {
    if (posixFun == unixclose)
        set.erase(value);
}
inline void removeFromSet(std::unordered_set<FILE *> &set, FILE *value, unixfclose_t posixFun) {
    if (posixFun == unixfclose)
        set.erase(value);
}

template <typename FileId, typename Func, typename FuncPosix, typename... Args>
auto outerWrapper(const char *name, FileId fileId, Timer::Metric metric, Func tazerFun, FuncPosix posixFun, Args... args) {
    if (!init) {
        posixFun = (FuncPosix)dlsym(RTLD_NEXT, name);
        return posixFun(args...);
    }

    timer.start();

    //Check if this is a special file to track (from environment variable)
    bool track = trackFile(fileId);

    //Check for files internal to tazer
    bool ignore = ignoreFile(fileId);

    //Check if a tazer meta-file
    bool isTazerFile = false;

    //Do the work
    auto retValue = innerWrapper(fileId, isTazerFile, tazerFun, posixFun, args...);
    if (ignore) {
        //Maintain the ignore_fd set
        addToSet(ignore_fd, retValue, posixFun);
        removeFromSet(ignore_fd, retValue, posixFun);
        timer.end(Timer::MetricType::local, Timer::Metric::dummy); //to offset the call to start()
    }
    else { //End Timers!
        if (track) {
            //Maintain the track_fd set
            addToSet(track_fd, retValue, posixFun);
            removeFromSet(track_fd, retValue, posixFun);
            if (std::string("read").compare(std::string(name)) == 0 ||
            std::string("write").compare(std::string(name)) == 0){
                ssize_t ret = *reinterpret_cast<ssize_t*> (&retValue);
                if (ret != -1) {
                    timer.addAmt(Timer::MetricType::local, metric,ret);
                }
            }
            timer.end(Timer::MetricType::local, metric);
        }
        else if (isTazerFile){
            timer.end(Timer::MetricType::tazer, metric);
        }
        else{
            if (std::string("read").compare(std::string(name)) == 0 ||
            std::string("write").compare(std::string(name)) == 0){
                ssize_t ret = *reinterpret_cast<ssize_t*> (&retValue);
                if (ret != -1) {
                    timer.addAmt(Timer::MetricType::system, metric,ret);
                }
            }
            timer.end(Timer::MetricType::system, metric);
        }
    }
    return retValue;
}

/*Posix******************************************************************************************************/

int tazerOpen(std::string name, std::string metaName, TazerFile::Type type, const char *pathname, int flags, int mode);
int open(const char *pathname, int flags, ...);
int open64(const char *pathname, int flags, ...);

int tazerClose(TazerFile *file, unsigned int fp, int fd);
int close(int fd);

ssize_t tazerRead(TazerFile *file, unsigned int fp, int fd, void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);

ssize_t tazerWrite(TazerFile *file, unsigned int fp, int fd, const void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);

template <typename T>
T tazerLseek(TazerFile *file, unsigned int fp, int fd, T offset, int whence);
off_t lseek(int fd, off_t offset, int whence) ADD_THROW;
off64_t lseek64(int fd, off64_t offset, int whence) ADD_THROW;

int innerStat(int version, const char *filename, struct stat *buf);
int innerStat(int version, const char *filename, struct stat64 *buf);
template <typename T>
int tazerStat(std::string name, std::string metaName, TazerFile::Type type, int version, const char *filename, T *buf);
int __xstat(int version, const char *filename, struct stat *buf) ADD_THROW;
int __xstat64(int version, const char *filename, struct stat64 *buf) ADD_THROW;
int __lxstat(int version, const char *filename, struct stat *buf) ADD_THROW;
int __lxstat64(int version, const char *filename, struct stat64 *buf) ADD_THROW;

int tazerFsync(TazerFile *file, unsigned int fp, int fd);
int fsync(int fd);

template <typename Func, typename FuncLocal>
ssize_t tazerVector(const char *name, Timer::Metric metric, Func tazerFun, FuncLocal localFun, int fd, const struct iovec *iov, int iovcnt);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
/*Streaming**************************************************************************************************/

FILE *tazerFopen(std::string name, std::string metaName, TazerFile::Type type, const char *__restrict fileName, const char *__restrict modes);
FILE *fopen(const char *__restrict fileName, const char *__restrict modes);
FILE *fopen64(const char *__restrict fileName, const char *__restrict modes);

int tazerFclose(TazerFile *file, unsigned int pos, int fd, FILE *fp);
int fclose(FILE *fp);

size_t tazerFread(TazerFile *file, unsigned int pos, int fd, void *__restrict ptr, size_t size, size_t n, FILE *__restrict fp);
size_t fread(void *__restrict ptr, size_t size, size_t n, FILE *__restrict fp);

size_t tazerFwrite(TazerFile *file, unsigned int pos, int fd, const void *__restrict ptr, size_t size, size_t n, FILE *__restrict fp);
size_t fwrite(const void *__restrict ptr, size_t size, size_t n, FILE *__restrict fp);

long int tazerFtell(TazerFile *file, unsigned int pos, int fd, FILE *fp);
long int ftell(FILE *fp);

int tazerFseek(TazerFile *file, unsigned int pos, int fd, FILE *fp, long int off, int whence);
int fseek(FILE *fp, long int off, int whence);

int tazerFgetc(TazerFile *file, unsigned int pos, int fd, FILE *fp);
int fgetc(FILE *fp);

char *tazerFgets(TazerFile *file, unsigned int pos, int fd, char *__restrict s, int n, FILE *__restrict fp);
char *fgets(char *__restrict s, int n, FILE *__restrict fp);

int tazerFputc(TazerFile *file, unsigned int pos, int fd, int c, FILE *fp);
int fputc(int c, FILE *fp);

int tazerFputs(TazerFile *file, unsigned int pos, int fd, const char *__restrict s, FILE *__restrict fp);
int fputs(const char *__restrict s, FILE *__restrict fp);

int tazerFeof(TazerFile *file, unsigned int pos, int fd, FILE *fp);
int feof(FILE *fp) ADD_THROW;

off_t tazerRewind(TazerFile *file, unsigned int pos, int fd, int fd2, off_t offset, int whence);
void rewind(FILE *fp);
