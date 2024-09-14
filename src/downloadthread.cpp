/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2020 Raspberry Pi Ltd
 */

#include "downloadthread.h"
#include "config.h"
#include "dependencies/mountutils/src/mountutils.hpp"
#include "dependencies/drivelist/src/drivelist.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <utime.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex>
#include <QDebug>
#include <QProcess>
#include <QSettings>
#include <QtConcurrent/QtConcurrent>
#include <QtNetwork/QNetworkProxy>

#ifdef Q_OS_LINUX
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "linux/udisks2api.h"
#endif

using namespace std;

bool useSettings;
bool justUpdate;

QByteArray DownloadThread::_proxy;
int DownloadThread::_curlCount = 0;

QSettings settings;

DownloadThread::DownloadThread(const QByteArray &url, const QByteArray &localfilename, const QByteArray &expectedHash, QObject *parent) :
    QThread(parent), _startOffset(0), _lastDlTotal(0), _lastDlNow(0), _verifyTotal(0), _lastVerifyNow(0), _bytesWritten(0), _lastFailureOffset(0), _sectorsStart(-1), _url(url), _filename(localfilename), _expectedHash(expectedHash),
    _firstBlock(nullptr), _cancelled(false), _successful(false), _verifyEnabled(false), _cacheEnabled(false), _lastModified(0), _serverTime(0),  _lastFailureTime(0),
    _inputBufferSize(0), _file(NULL), _writehash(OSLIST_HASH_ALGORITHM), _verifyhash(OSLIST_HASH_ALGORITHM)
{
    if (!_curlCount)
        curl_global_init(CURL_GLOBAL_DEFAULT);
    _curlCount++;
    _ejectEnabled = false;
}

DownloadThread::~DownloadThread()
{
    _cancelled = true;
    wait();
    if (_file.isOpen())
        _file.close();

    if (_firstBlock)
        qFreeAligned(_firstBlock);

    if (!--_curlCount)
        curl_global_cleanup();
}

void DownloadThread::setProxy(const QByteArray &proxy)
{
    _proxy = proxy;
}

QByteArray DownloadThread::proxy()
{
    return _proxy;
}

void DownloadThread::setUserAgent(const QByteArray &ua)
{
    _useragent = ua;
}

/* Curl write callback function, let it call the object oriented version */
size_t DownloadThread::_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return static_cast<DownloadThread *>(userdata)->_writeData(ptr, size * nmemb);
}

int DownloadThread::_curl_xferinfo_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    return (static_cast<DownloadThread *>(userdata)->_progress(dltotal, dlnow, ultotal, ulnow) == false);
}

size_t DownloadThread::_curl_header_callback( void *ptr, size_t size, size_t nmemb, void *userdata)
{
    int len = size*nmemb;
    string headerstr((char *) ptr, len);
    static_cast<DownloadThread *>(userdata)->_header(headerstr);

    return len;
}

QByteArray DownloadThread::_fileGetContentsTrimmed(const QString &filename)
{
    QByteArray result;
    QFile f(filename);

    if (f.exists() && f.open(f.ReadOnly))
    {
        result = f.readAll().trimmed();
        f.close();
    }

    return result;
}

bool DownloadThread::_openAndPrepareDevice()
{
    QSettings settings_;
    std::cout << "_____________________________-DEBUG-_______________________________________" << std::endl;

    if (settings_.value("justUpdate").toBool())
    {
    std::cout << "running update procedure without modifying the image" << std::endl;
    return true;
    }

    emit preparationStatusUpdate(tr("opening drive"));

    if (_filename.startsWith("/dev/update-"))
    {
    std::cout << "running update procedure without modifying the image" << std::endl;
    }

    if (_filename.startsWith("/dev/"))
    {
        unmount_disk(_filename.constData());
    }

    _file.setFileName(_filename);

#ifdef Q_OS_WIN
    qDebug() << "device" << _filename;

    std::regex windriveregex("\\\\\\\\.\\\\PHYSICALDRIVE([0-9]+)", std::regex_constants::icase);
    std::cmatch m;

    if (std::regex_match(_filename.constData(), m, windriveregex))
    {

        _nr = QByteArray::fromStdString(m[1]);

        if (!_nr.isEmpty()) {
            qDebug() << "Removing partition table from Windows drive #" << _nr << "(" << _filename << ")";

            QProcess proc;
            proc.start("diskpart");
            proc.waitForStarted();
            proc.write("select disk "+_nr+"\r\n"
                                              "clean\r\n"
                                              "rescan\r\n");
            proc.closeWriteChannel();
            proc.waitForFinished();

            if (proc.exitCode())
            {
                emit error(tr("Error running diskpart: %1").arg(QString(proc.readAllStandardError())));
                return false;
            }
        }
    }

    auto l = Drivelist::ListStorageDevices();
    QByteArray devlower = _filename.toLower();
    QByteArray driveLetter;
    for (auto i : l)
    {
        if (QByteArray::fromStdString(i.device).toLower() == devlower)
        {
            if (i.mountpoints.size() == 1)
            {
                driveLetter = QByteArray::fromStdString(i.mountpoints.front());
                if (driveLetter.endsWith("\\"))
                    driveLetter.chop(1);
            }
            else if (i.mountpoints.size() > 1)
            {
                emit error(tr("Error removing existing partitions"));
                return false;
            }
            else
            {
                qDebug() << "Device no longer has any volumes. Nothing to lock.";
            }
        }
    }

    if (!driveLetter.isEmpty())
    {
        _volumeFile.setFileName("\\\\.\\"+driveLetter);
        if (_volumeFile.open(QIODevice::ReadWrite))
            _volumeFile.lockVolume();
    }

#endif

#ifdef Q_OS_DARWIN
    _filename.replace("/dev/disk", "/dev/rdisk");

    auto authopenresult = _file.authOpen(_filename);

    if (authopenresult == _file.authOpenCancelled) {
        /* User cancelled authentication */
        emit error(tr("Authentication cancelled"));
        return false;
    } else if (authopenresult == _file.authOpenError) {
        QString msg = tr("Error running authopen to gain access to disk device '%1'").arg(QString(_filename));
        msg += "<br>"+tr("Please verify if 'Raspberry Pi Imager' is allowed access to 'removable volumes' in privacy settings (under 'files and folders' or alternatively give it 'full disk access').");
        QStringList args("x-apple.systempreferences:com.apple.preference.security?Privacy_RemovableVolume");
        QProcess::execute("open", args);
        emit error(msg);
        return false;
    }
#else
    if (!_file.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
    {
#ifdef Q_OS_LINUX
#ifndef QT_NO_DBUS
        /* Opening device directly did not work, ask udisks2 to do it for us,
         * if necessary prompting for authorization */
        UDisks2Api udisks;
        int fd = udisks.authOpen(_filename);
        if (fd != -1)
        {
            _file.open(fd, QIODevice::ReadWrite | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle);
        }
        else
#endif
        {
            emit error(tr("Cannot open storage device '%1'.").arg(QString(_filename)));
            return false;
        }
#endif
    }
#endif

#ifdef Q_OS_LINUX
    /* Optional optimizations for Linux */

    if (_filename.startsWith("/dev/"))
    {
        QString devname = _filename.mid(5);

        /* On some internal SD card readers CID/CSD is available, print it for debugging purposes */
        QByteArray cid = _fileGetContentsTrimmed("/sys/block/"+devname+"/device/cid");
        QByteArray csd = _fileGetContentsTrimmed("/sys/block/"+devname+"/device/csd");
        if (!cid.isEmpty())
            qDebug() << "SD card CID:" << cid;
        if (!csd.isEmpty())
            qDebug() << "SD card CSD:" << csd;

        QByteArray discardmax = _fileGetContentsTrimmed("/sys/block/"+devname+"/queue/discard_max_bytes");

        if (discardmax.isEmpty() || discardmax == "0")
        {
            qDebug() << "BLKDISCARD not supported";
        }
        else
        {
            /* DISCARD/TRIM the SD card */
            uint64_t devsize, range[2];
            int fd = _file.handle();

            if (::ioctl(fd, BLKGETSIZE64, &devsize) == -1) {
                qDebug() << "Error getting device/sector size with BLKGETSIZE64 ioctl():" << strerror(errno);
            }
            else
            {
                qDebug() << "Try to perform TRIM/DISCARD on device";
                range[0] = 0;
                range[1] = devsize;
                emit preparationStatusUpdate(tr("discarding existing data on drive"));
                _timer.start();
                if (::ioctl(fd, BLKDISCARD, &range) == -1)
                {
                    qDebug() << "BLKDISCARD failed.";
                }
                else
                {
                    qDebug() << "BLKDISCARD successful. Discarding took" << _timer.elapsed() / 1000 << "seconds";
                }
            }
        }
    }
#endif

#ifndef Q_OS_WIN
    // Zero out MBR
    qint64 knownsize = _file.size();
    QByteArray emptyMB(1024*1024, 0);

    emit preparationStatusUpdate(tr("zeroing out first and last MB of drive"));
    qDebug() << "Zeroing out first and last MB of drive";
    _timer.start();

    if (!_file.write(emptyMB.data(), emptyMB.size()) || !_file.flush())
    {
        emit error(tr("Write error while zero'ing out MBR"));
        return false;
    }

    // Zero out last part of card (may have GPT backup table)
    if (knownsize > emptyMB.size())
    {
        if (!_file.seek(knownsize-emptyMB.size())
            || !_file.write(emptyMB.data(), emptyMB.size())
            || !_file.flush()
            || !::fsync(_file.handle()))
        {
            emit error(tr("Write error while trying to zero out last part of card.<br>"
                          "Card could be advertising wrong capacity (possible counterfeit)."));
            return false;
        }
    }
    emptyMB.clear();
    _file.seek(0);
    qDebug() << "Done zeroing out start and end of drive. Took" << _timer.elapsed() / 1000 << "seconds";
#endif

#ifdef Q_OS_LINUX
    _sectorsStart = _sectorsWritten();
#endif

    return true;
}

void DownloadThread::run()
{
    if (isImage() && !_openAndPrepareDevice())
    {
        return;
    }

    qDebug() << "Image URL:" << _url;
    if (_url.startsWith("file://") && _url.at(7) != '/')
    {
        /* libcurl does not like UNC paths in the form of file://1.2.3.4/share */
        _url.replace("file://", "file:////");
        qDebug() << "Corrected UNC URL to:" << _url;
    }

    char errorBuf[CURL_ERROR_SIZE] = {0};
    _c = curl_easy_init();
    curl_easy_setopt(_c, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(_c, CURLOPT_WRITEFUNCTION, &DownloadThread::_curl_write_callback);
    curl_easy_setopt(_c, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_c, CURLOPT_XFERINFOFUNCTION, &DownloadThread::_curl_xferinfo_callback);
    curl_easy_setopt(_c, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(_c, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(_c, CURLOPT_URL, _url.constData());
    curl_easy_setopt(_c, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(_c, CURLOPT_MAXREDIRS, 10);
    curl_easy_setopt(_c, CURLOPT_ERRORBUFFER, errorBuf);
    curl_easy_setopt(_c, CURLOPT_FAILONERROR, 1);
    curl_easy_setopt(_c, CURLOPT_HEADERFUNCTION, &DownloadThread::_curl_header_callback);
    curl_easy_setopt(_c, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(_c, CURLOPT_CONNECTTIMEOUT, 30);
    curl_easy_setopt(_c, CURLOPT_LOW_SPEED_TIME, 60);
    curl_easy_setopt(_c, CURLOPT_LOW_SPEED_LIMIT, 100);
    if (_inputBufferSize)
        curl_easy_setopt(_c, CURLOPT_BUFFERSIZE, _inputBufferSize);

    if (!_useragent.isEmpty())
        curl_easy_setopt(_c, CURLOPT_USERAGENT, _useragent.constData());

    if (_proxy.isEmpty())
    {
#ifndef QT_NO_NETWORKPROXY
        /* Ask OS for proxy information. */
        QNetworkProxyQuery npq{QUrl{_url}};
        QList<QNetworkProxy> proxyList = QNetworkProxyFactory::systemProxyForQuery(npq);
        if (!proxyList.isEmpty())
        {
            QNetworkProxy proxy = proxyList.first();
            if (proxy.type() != proxy.NoProxy)
            {
                QUrl proxyUrl;

                proxyUrl.setScheme(proxy.type() == proxy.Socks5Proxy ? "socks5h" : "http");
                proxyUrl.setHost(proxy.hostName());
                proxyUrl.setPort(proxy.port());
                qDebug() << "Using proxy server:" << proxyUrl;

                if (!proxy.user().isEmpty())
                {
                    proxyUrl.setUserName(proxy.user());
                    proxyUrl.setPassword(proxy.password());
                }

                _proxy = proxyUrl.toEncoded();
            }
        }
#endif
    }

    if (!_proxy.isEmpty())
        curl_easy_setopt(_c, CURLOPT_PROXY, _proxy.constData());

    emit preparationStatusUpdate(tr("starting download"));
    _timer.start();
    CURLcode ret = curl_easy_perform(_c);

    /* Deal with badly configured HTTP servers that terminate the connection quickly
       if connections stalls for some seconds while kernel commits buffers to slow SD card.
       And also reconnect if we detect from our end that transfer stalled for more than one minute */
    while (ret == CURLE_PARTIAL_FILE || ret == CURLE_OPERATION_TIMEDOUT || (ret == CURLE_RECV_ERROR && _lastDlNow != _lastFailureOffset) )
    {
        time_t t = time(NULL);
        qDebug() << "HTTP connection lost. Time:" << t;

        /* If last failure happened less than 5 seconds ago, something else may
           be wrong. Sleep some time to prevent hammering server */
        if (t - _lastFailureTime < 5)
        {
            qDebug() << "Sleeping 5 seconds";
            ::sleep(5);
        }
        _lastFailureTime = t;

        _startOffset = _lastDlNow;
        _lastFailureOffset = _lastDlNow;
        curl_easy_setopt(_c, CURLOPT_RESUME_FROM_LARGE, _startOffset);

        ret = curl_easy_perform(_c);
    }

    curl_easy_cleanup(_c);

    switch (ret)
    {
    case CURLE_OK:
        _successful = true;
        qDebug() << "Download done in" << _timer.elapsed() / 1000 << "seconds";
        _onDownloadSuccess();
        break;
    case CURLE_WRITE_ERROR:
        deleteDownloadedFile();

#ifdef Q_OS_WIN
        if (_file.errorCode() == ERROR_ACCESS_DENIED)
        {
            QString msg = tr("Access denied error while writing file to disk.");
            QSettings registry("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\Controlled Folder Access",
                               QSettings::Registry64Format);
            if (registry.value("EnableControlledFolderAccess").toInt() == 1)
            {
                msg += "<br>"+tr("Controlled Folder Access seems to be enabled. Please add both openhdimagewriter.exe and fat32format.exe to the list of allowed apps and try again.");
            }
            _onDownloadError(msg);
        }
        else
#endif
            if (!_cancelled)
                _onDownloadError(tr("Error writing file to disk"));
        break;
    case CURLE_ABORTED_BY_CALLBACK:
        deleteDownloadedFile();
        break;
    default:
        deleteDownloadedFile();
        QString errorMsg;

        if (!errorBuf[0])
            /* No detailed error message text provided, use standard text for libcurl result code */
            errorMsg += curl_easy_strerror(ret);
        else
            errorMsg += errorBuf;

        char *ipstr;
        if (curl_easy_getinfo(_c, CURLINFO_PRIMARY_IP, &ipstr) == CURLE_OK && ipstr && ipstr[0])
            errorMsg += QString(" - Server IP: ")+ipstr;

        _onDownloadError(tr("Error downloading: %1").arg(errorMsg));
    }
}

size_t DownloadThread::_writeData(const char *buf, size_t len)
{
    _writeCache(buf, len);

    if (!_filename.isEmpty())
    {
        return _writeFile(buf, len);
    }
    else
    {
        _buf.append(buf, len);
        return len;
    }
}

void DownloadThread::_writeCache(const char *buf, size_t len)
{
    if (!_cacheEnabled || _cancelled)
        return;

    if (_cachefile.write(buf, len) != len)
    {
        qDebug() << "Error writing to cache file. Disabling caching.";
        _cacheEnabled = false;
        _cachefile.remove();
    }
}

void DownloadThread::setCacheFile(const QString &filename, qint64 filesize)
{
    _cachefile.setFileName(filename);
    if (_cachefile.open(QIODevice::WriteOnly))
    {
        _cacheEnabled = true;
        if (filesize)
        {
            /* Pre-allocate space */
            _cachefile.resize(filesize);
        }
    }
    else
    {
        qDebug() << "Error opening cache file for writing. Disabling caching.";
    }
}

void DownloadThread::_hashData(const char *buf, size_t len)
{
    _writehash.addData(buf, len);
}

size_t DownloadThread::_writeFile(const char *buf, size_t len)
{
    if (_cancelled)
        return len;

    if (!_firstBlock)
    {
        _writehash.addData(buf, len);
        _firstBlock = (char *) qMallocAligned(len, 4096);
        _firstBlockSize = len;
        ::memcpy(_firstBlock, buf, len);

        return _file.seek(len) ? len : 0;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QFuture<void> wh = QtConcurrent::run(&DownloadThread::_hashData, this, buf, len);
#else
    QFuture<void> wh = QtConcurrent::run(this, &DownloadThread::_hashData, buf, len);
#endif

    qint64 written = _file.write(buf, len);
    _bytesWritten += written;

    if ((size_t) written != len)
    {
        qDebug() << "Write error:" << _file.errorString() << "while writing len:" << len;
    }

    wh.waitForFinished();
    return (written < 0) ? 0 : written;
}

bool DownloadThread::_progress(curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    if (dltotal)
        _lastDlTotal = _startOffset + dltotal;
    _lastDlNow   = _startOffset + dlnow;

    return !_cancelled;
}

void DownloadThread::_header(const string &header)
{
    if (header.compare(0, 6, "Date: ") == 0)
    {
        _serverTime = curl_getdate(header.data()+6, NULL);
    }
    else if (header.compare(0, 15, "Last-Modified: ") == 0)
    {
        _lastModified = curl_getdate(header.data()+15, NULL);
    }
    qDebug() << "Received header:" << header.c_str();
}

void DownloadThread::cancelDownload()
{
    _cancelled = true;
    //deleteDownloadedFile();
}

QByteArray DownloadThread::data()
{
    return _buf;
}

bool DownloadThread::successfull()
{
    return _successful;
}

time_t DownloadThread::lastModified()
{
    return _lastModified;
}

time_t DownloadThread::serverTime()
{
    return _serverTime;
}

void DownloadThread::deleteDownloadedFile()
{
    if (!_filename.isEmpty())
    {
        _file.close();
        if (_cachefile.isOpen())
            _cachefile.remove();
#ifdef Q_OS_WIN
        _volumeFile.close();
#endif

        if (!_filename.startsWith("/dev/") && !_filename.startsWith("\\\\.\\"))
        {
            //_file.remove();
        }
    }
}

uint64_t DownloadThread::dlNow()
{
    return _lastDlNow;
}

uint64_t DownloadThread::dlTotal()
{
    return _lastDlTotal;
}

uint64_t DownloadThread::verifyNow()
{
    return _lastVerifyNow;
}

uint64_t DownloadThread::verifyTotal()
{
    return _verifyTotal;
}

uint64_t DownloadThread::bytesWritten()
{
    if (_sectorsStart != -1)
        return qMin((uint64_t) (_sectorsWritten()-_sectorsStart)*512, (uint64_t) _bytesWritten);
    else
        return _bytesWritten;
}

void DownloadThread::_onDownloadSuccess()
{
    _writeComplete();
}

void DownloadThread::_onDownloadError(const QString &msg)
{
    _cancelled = true;
    emit error(msg);
}

void DownloadThread::_closeFiles()
{
    _file.close();
#ifdef Q_OS_WIN
    _volumeFile.close();
#endif
    if (_cachefile.isOpen())
        _cachefile.close();
}

void DownloadThread::_writeComplete()
{
    QByteArray computedHash = _writehash.result().toHex();
    qDebug() << "Hash of uncompressed image:" << computedHash;
    if (!_expectedHash.isEmpty() && _expectedHash != computedHash)
    {
        qDebug() << "Mismatch with expected hash:" << _expectedHash;
        if (_cachefile.isOpen())
            _cachefile.remove();
        DownloadThread::_onDownloadError(tr("Download corrupt. Hash does not match"));
        _closeFiles();
        return;
    }
    if (_cacheEnabled && _expectedHash == computedHash)
    {
        _cachefile.close();
        emit cacheFileUpdated(computedHash);
    }

    if (!_file.flush())
    {
        DownloadThread::_onDownloadError(tr("Error writing to storage (while flushing)"));
        _closeFiles();
        return;
    }

#ifndef Q_OS_WIN
    if (::fsync(_file.handle()) != 0) {
        DownloadThread::_onDownloadError(tr("Error writing to storage (while fsync)"));
        _closeFiles();
        return;
    }
#endif

    qDebug() << "Write done in" << _timer.elapsed() / 1000 << "seconds";

    /* Verify */
    if (_verifyEnabled && !_verify())
    {
        _closeFiles();
        return;
    }

    emit finalizing();

    if (_firstBlock)
    {
        qDebug() << "Writing first block (which we skipped at first)";
        _file.seek(0);
        if (!_file.write(_firstBlock, _firstBlockSize) || !_file.flush())
        {
            qFreeAligned(_firstBlock);
            _firstBlock = nullptr;

            DownloadThread::_onDownloadError(tr("Error writing first block (partition table)"));
            return;
        }
        _bytesWritten += _firstBlockSize;
        qFreeAligned(_firstBlock);
        _firstBlock = nullptr;
    }

    _closeFiles();

#ifdef Q_OS_DARWIN
    QThread::sleep(1);
    _filename.replace("/dev/rdisk", "/dev/disk");
#endif
    QSettings settings_;

    useSettings = settings_.value("useSettings").toBool();
    justUpdate = settings_.value("justUpdate").toBool();
    // qDebug() << "This is the Debug Filename" << _filename;
    // qDebug() << "This is the Debug Settings" << useSettings;
    // qDebug() << "This is the Debug Update" << justUpdate;

    if (!useSettings)
        eject_disk(_filename.constData());

    if (useSettings)
    {
        if (!_customizeImage())
            return;

        if (_ejectEnabled)
            eject_disk(_filename.constData());
    }

    emit success();
}

bool DownloadThread::_verify()
{
    char *verifyBuf = (char *) qMallocAligned(IMAGEWRITER_VERIFY_BLOCKSIZE, 4096);
    _lastVerifyNow = 0;
    _verifyTotal = _file.pos();
    QElapsedTimer t1;
    t1.start();

#ifdef Q_OS_LINUX
    /* Make sure we are reading from the drive and not from cache */
    //fcntl(_file.handle(), F_SETFL, O_DIRECT | fcntl(_file.handle(), F_GETFL));
    posix_fadvise(_file.handle(), 0, 0, POSIX_FADV_DONTNEED);
#endif

    if (!_firstBlock)
    {
        _file.seek(0);
    }
    else
    {
        _verifyhash.addData(_firstBlock, _firstBlockSize);
        _file.seek(_firstBlockSize);
        _lastVerifyNow += _firstBlockSize;
    }

    while (_verifyEnabled && _lastVerifyNow < _verifyTotal && !_cancelled)
    {
        qint64 lenRead = _file.read(verifyBuf, qMin((qint64) IMAGEWRITER_VERIFY_BLOCKSIZE, (qint64) (_verifyTotal-_lastVerifyNow) ));
        if (lenRead == -1)
        {
            DownloadThread::_onDownloadError(tr("Error reading from storage.<br>"
                                                "SD card may be broken."));
            return false;
        }

        _verifyhash.addData(verifyBuf, lenRead);
        _lastVerifyNow += lenRead;
    }
    qFreeAligned(verifyBuf);

    qDebug() << "Verify hash:" << _verifyhash.result().toHex();
    qDebug() << "Verify done in" << t1.elapsed() / 1000.0 << "seconds";

    if (_verifyhash.result() == _writehash.result() || !_verifyEnabled || _cancelled)
    {
        return true;
    }
    else
    {
        DownloadThread::_onDownloadError(tr("Verifying write failed. Contents of SD card is different from what was written to it."));
    }

    return false;
}

void DownloadThread::setVerifyEnabled(bool verify)
{
    _verifyEnabled = verify;
}

bool DownloadThread::isImage()
{
    return true;
}

void DownloadThread::setInputBufferSize(int len)
{
    _inputBufferSize = len;
}

qint64 DownloadThread::_sectorsWritten()
{
#ifdef Q_OS_LINUX
    if (!_filename.startsWith("/dev/"))
        return -1;

    QFile f("/sys/class/block/"+_filename.mid(5)+"/stat");
    if (!f.open(f.ReadOnly))
        return -1;
    QByteArray ioline = f.readAll().simplified();
    f.close();

    QList<QByteArray> stats = ioline.split(' ');

    if (stats.count() >= 6)
        return stats.at(6).toLongLong(); /* write sectors */
#endif
    return -1;
}

bool DownloadThread::_customizeImage()
{
    QString folder;
    std::vector<std::string> mountpoints;
    QByteArray devlower = _filename.toLower();

    emit preparationStatusUpdate(tr("Waiting for FAT partition to be mounted"));

#ifdef Q_OS_WIN
    qDebug() << "Running diskpart rescan";
    QProcess proc;
    proc.setProcessChannelMode(proc.MergedChannels);
    proc.start("diskpart");
    proc.waitForStarted();
    proc.write("rescan\r\n");
    proc.closeWriteChannel();
    proc.waitForFinished();
    qDebug() << proc.readAll();
#endif

    /* See if OS auto-mounted the device */
    for (int tries = 0; tries < 3; tries++)
    {
        QThread::sleep(1);
        auto l = Drivelist::ListStorageDevices();
        for (auto i : l)
        {
            if (QByteArray::fromStdString(i.device).toLower() == devlower && i.mountpoints.size())
            {
                mountpoints = i.mountpoints;
                break;
            }
        }
    }

#ifdef Q_OS_WIN
    if (mountpoints.empty() && !_nr.isEmpty()) {
        qDebug() << "Windows did not assign drive letter automatically. Ask diskpart to do so manually.";
        proc.start("diskpart");
        proc.waitForStarted();
        proc.write("select disk "+_nr+"\r\n"
                                          "select partition 1\r\n"
                                          "assign\r\n");
        proc.closeWriteChannel();
        proc.waitForFinished();
        qDebug() << proc.readAll();

        auto l = Drivelist::ListStorageDevices();
        for (auto i : l)
        {
            if (QByteArray::fromStdString(i.device).toLower() == devlower && i.mountpoints.size())
            {
                mountpoints = i.mountpoints;
                break;
            }
        }
    }
#endif

#ifdef Q_OS_LINUX
    bool manualmount = false;

    if (mountpoints.empty())
    {
        /* Manually mount folder */
        manualmount = true;
        QByteArray fatpartition = _filename;
        if (isdigit(fatpartition.at(fatpartition.length()-1)))
            fatpartition += "p1";
        else
            fatpartition += "1";

        if (::access(devlower.constData(), W_OK) != 0)
        {
            /* Not running as root, try to outsource mounting to udisks2 */
#ifndef QT_NO_DBUS
            UDisks2Api udisks2;
            QString mp = udisks2.mountDevice(fatpartition);
            if (!mp.isEmpty())
                mountpoints.push_back(mp.toStdString());
#endif
        }
        else
        {
            /* Running as root, attempt running mount directly */
            QTemporaryDir td;
            QStringList args;
            mountpoints.push_back(td.path().toStdString());
            args << "-t" << "vfat" << fatpartition << td.path();

            if (QProcess::execute("mount", args) != 0)
            {
                emit error(tr("Error mounting FAT32 partition"));
                return false;
            }
            td.setAutoRemove(false);
        }
    }
#endif

    if (mountpoints.empty())
    {
        //
        qDebug() << "drive info. searching for:" << devlower;
        auto l = Drivelist::ListStorageDevices();
        for (auto i : l)
        {
            qDebug() << "drive" << QByteArray::fromStdString(i.device).toLower();
            for (auto mp : i.mountpoints) {
                qDebug() << "mountpoint:" << QByteArray::fromStdString(mp);
            }
        }
        //

        emit error(tr("This Image doesn't support OpenHD settings, proceed with caution!"));
        return false;
    }

    /* Some operating system take longer to complete mounting FAT32
       wait up to 3 seconds for config.txt file to appear */
    QString configFilename;
    bool foundFile = false;

    for (int tries = 0; tries < 3; tries++)
    {
        /* Search all mountpoints, as on some systems FAT partition
           may not be first volume */
        for (auto mp : mountpoints)
        {
            folder = QString::fromStdString(mp);
            if (folder.right(1) == '\\')
                folder.chop(1);
            configFilename = folder+"/config.txt";

            if (QFile::exists(configFilename))
            {
                foundFile = true;
                break;
            }
        }
        if (foundFile)
            break;
        QThread::sleep(1);
    }

    if (!foundFile)
    {
        emit error(tr("Unable to customize. File '%1' does not exist.").arg(configFilename));
        return false;
    }

    emit preparationStatusUpdate(tr("Customizing image"));

    if (!_config.isEmpty())
    {
        auto configItems = _config.split('\n');
        configItems.removeAll("");
        QByteArray config;

        QFile f(configFilename);
        if (f.open(f.ReadOnly))
        {
            config = f.readAll();
            f.close();
        }

        for (QByteArray item : configItems)
        {
            if (config.contains("#"+item)) {
                /* Uncomment existing line */
                config.replace("#"+item, item);
            } else if (config.contains("\n"+item)) {
                /* config.txt already contains the line */
            } else {
                /* Append new line to config.txt */
                if (config.right(1) != "\n")
                    config += "\n"+item+"\n";
                else
                    config += item+"\n";
            }
        }

        if (f.open(f.WriteOnly) && f.write(config) == config.length())
        {
            f.close();
        }
        else
        {
            emit error(tr("Error writing to config.txt on FAT partition"));
            return false;
        }
    }

    if (_initFormat == "auto")
    {
        /* Do an attempt at auto-detecting what customization format a custom
           image provided by the user supports */
        QByteArray issue;
        QFile fi(folder+"/issue.txt");
        if (fi.exists() && fi.open(fi.ReadOnly))
        {
            issue = fi.readAll();
            fi.close();
        }

        if (QFile::exists(folder+"/user-data"))
        {
            /* If we have user-data file on FAT partition, then it must be cloudinit */
            _initFormat = "cloudinit";
            qDebug() << "user-data found on FAT partition. Assuming cloudinit support";
        }
        else if (issue.contains("pi-gen"))
        {
            /* If issue.txt mentions pi-gen, and there is no user-data file assume
             * it is a RPI OS flavor, and use the old systemd unit firstrun script stuff */
            _initFormat = "systemd";
            qDebug() << "copying openhd detection file";
        }
        else
        {
            /* Fallback to writing cloudinit file, as it does not hurt having one
             * Will just have no customization if OS does not support it */
            _initFormat = "cloudinit";
            qDebug() << "Unknown what customization method image supports. Falling back to cloudinit";
        }
    }

    /* Here is the start of the OpenHD settings routine
         */

    if (justUpdate) {
        qDebug() << "Writing OpenHD-Update";
        qDebug() << justUpdate;
    }


    if (useSettings) {
        qDebug() << "Writing OpenHD-Settings";
        QSettings settings_;


        QString cameraName = settings_.value("camera").toString();
        QString sbcValue = settings_.value("sbc").toString();
        QString modeValue = settings_.value("mode").toString();
        QString bindPhraseSaved = settings_.value("bindPhrase").toString();
        QString hotspot = settings_.value("hotspot").toString();
        QString bootType = settings_.value("bootType").toString();

        if (!bindPhraseSaved.isEmpty()){
            qDebug() << "BindPhrase found" << bindPhraseSaved;
            QFile Bp(folder+"/openhd"+"/password.txt");
            if (Bp.open(QIODevice::WriteOnly))
            {
                // Convert bindPhrase to UTF-8 bytes and write to the file
                QByteArray bindPhraseBytes = bindPhraseSaved.toUtf8();
                qint64 bytesWritten = Bp.write(bindPhraseBytes);
                Bp.close();

                if (bytesWritten == bindPhraseBytes.length())
                {
                    // Successfully wrote the password to the file
                }
                else
                {
                    emit error(tr("Error writing password to password.txt on FAT partition"));
                    return false;
                }
            }
            else
            {
                emit error(tr("Error creating password.txt on FAT partition"));
                return false;
            }
        }
        if (!cameraName.isEmpty()){
            QString cameraValue;
            qDebug() << "Camera found" << cameraName;
            QFile cam(folder+"/openhd/camera1.txt");
            //Encode camera name to Cam-int
            if (cam.open(QIODevice::WriteOnly))
            {
                if (sbcValue == "rpi"){
                    //RaspberryPi
                    if (cameraName == "OV5647"){
                    cameraValue="30";
                    }
                    else if (cameraName == "IMX219"){
                    cameraValue="31";
                    }
                    else if (cameraName == "IMX708"){
                    cameraValue="32";
                    }
                    else if (cameraName == "IMX477"){
                    cameraValue="33";
                    }
                    else if (cameraName == "HDMI"){
                    cameraValue="20";
                    }
                    //Arducam
                    else if (cameraName == "SkyMasterHDR708"){
                    cameraValue="40";
                    }
                    else if (cameraName == "SkyVisionPro519"){
                    cameraValue="41";
                    }
                    else if (cameraName == "IMX477m"){
                    cameraValue="42";
                    }
                    else if (cameraName == "IMX462"){
                    cameraValue="43";
                    }
                    else if (cameraName == "IMX327"){
                    cameraValue="44";
                    }
                    else if (cameraName == "IMX290"){
                    cameraValue="45";
                    }
                    else if (cameraName == "IMX462MINI"){
                    cameraValue="46";
                    }
                    //Veye
                    else if (cameraName == "2MPCAMERAS"){
                    cameraValue="60";
                    }
                    else if (cameraName == "CSIMX307"){
                    cameraValue="61";
                    }
                    else if (cameraName == "CSSC137"){
                    cameraValue="62";
                    }
                    else if (cameraName == "MVCAM"){
                    cameraValue="63";
                    }               
                }
                else if (sbcValue == "zero3w"){
                    if (cameraName == "HDMI"){
                    cameraValue="90";
                    }
                    if (cameraName == "IMX462"){
                    cameraValue="94";
                    }
                    if (cameraName == "IMX519"){
                    cameraValue="95";
                    }
                    if (cameraName == "IMX219"){
                    cameraValue="92";
                    }
                    else if (cameraName == "OV5647"){
                    cameraValue="91";
                    }
                    else if (cameraName == "IMX708"){
                    cameraValue="93";
                    }
                    else if (cameraName == "VEYE"){
                    cameraValue="97";
                    }
                    else if (cameraName == "OHD-JAGUAR"){
                    cameraValue="96";
                    }               
                }
                else if ((sbcValue == "rock-5b") || (sbcValue == "rock-5a")) {
                    if (cameraName == "HDMI"){
                    cameraValue="80";
                    }
                    if (cameraName == "IMX219"){
                    cameraValue="82";
                    }
                    else if (cameraName == "OV5647"){
                    cameraValue="81";
                    }
                    else if (cameraName == "IMX708"){
                    cameraValue="83";
                    }
                    else if (cameraName == "IMX462"){
                    cameraValue="84";
                    }
                    else if (cameraName == "IMX415"){
                    cameraValue="85";
                    }
                    else if (cameraName == "IMX477"){
                    cameraValue="86";
                    }
                    else if (cameraName == "IMX519"){
                    cameraValue="87";
                    }
                    else if (cameraName == "OHD-JAGUAR"){
                    cameraValue="88";
                    }               
                }

                if (cameraName == "FILESRC"){
                    cameraValue="4";
                }
                else if (cameraName == "IP-CAMERA"){
                    cameraValue="3";
                }
                else if (cameraName == "EXTERNAL"){
                    cameraValue="2";
                }
                else if (cameraName == "USB"){
                    cameraValue="1";
                }
                else if (cameraName == "TESTPATTERN"){
                    cameraValue="0";
                }
                
                QByteArray camBytes = cameraValue.toUtf8();
                qint64 bytesWritten = cam.write(camBytes);
                cam.close();


                if (bytesWritten == camBytes.length())
                {
                    // Successfully wrote the camera to the file
                }
                else
                {
                    emit error(tr("Error writing camera to camera1.txt on FAT partition"));
                    return false;
                }
            }
            else
            {
                emit error(tr("Error creating camera1.txt on FAT partition"));
                return false;
            }
        }
        if (!sbcValue.isEmpty()){
            QFile sbc(folder + "/openhd" + "/" + sbcValue + ".txt");
            if (sbc.open(QIODevice::WriteOnly)) {
                QTextStream stream(&sbc);
                sbc.close();
            } else {
                emit error(tr("This Image does not support settings, yet !"));
                return false;
            }
        }
        // if (!hotspot.isEmpty()){
        //     QFile Hs(folder+"/openhd"+"/wifi_hotspot.txt");
        //     if (Hs.open(QIODevice::WriteOnly)) {
        //         QTextStream stream(&Hs);
        //         Hs.close();
        //     } else {
        //         emit error(tr("Error creating hotspot.txt file on FAT partition"));
        //         return false;
        //     }
        // }
        if (modeValue == "debug"){
            QFile Db(folder+"/openhd"+"/debug.txt");
            if (Db.open(QIODevice::WriteOnly)) {
                QTextStream stream(&Db);
                Db.close();
            } else {
                emit error(tr("Error creating debug.txt file on FAT partition"));
                return false;
            }
        }
        if (!sbcValue.isEmpty()) {
            QFile sbc(folder+"/openhd"+"/"+sbcValue+".txt");
            if (sbc.open(QIODevice::WriteOnly)) {
                QTextStream stream(&sbc);
                sbc.close();
            } else {
                emit error(tr("This Image does not support settings, yet !"));
                return false;
            }
        }
        if (bootType == "Air"){
            QFile air(folder+"/openhd"+"/air.txt");
            if (air.open(QIODevice::WriteOnly)) {
                QTextStream stream(&air);
                air.close();
            } else {
                emit error(tr("Error creating air.txt file on FAT partition"));
                return false;
            }
        }
        else if(bootType == "Ground"){
            QFile ground(folder+"/openhd"+"/ground.txt");
            if (ground.open(QIODevice::WriteOnly)) {
                QTextStream stream(&ground);
                ground.close();
            } else {
                emit error(tr("Error creating air.txt file on FAT partition"));
                return false;
            }
        }
        // if (!cameraName.isEmpty()) {
        //     QFile cam(folder+"/openhd"+"/+camera1+.txt");
        //     if (cam.open(QIODevice::WriteOnly)) {
        //         QTextStream stream(&cam);
        //         cam.close();
        //     } else {
        //         emit error(tr("Error creating Camera file on FAT partition"));
        //         return false;
        //     }
        // }
    }

    emit finalizing();

#ifdef Q_OS_LINUX
    if (manualmount)
    {
        if (::access(devlower.constData(), W_OK) != 0)
        {
#ifndef QT_NO_DBUS
            UDisks2Api udisks2;
            udisks2.unmountDrive(devlower);
#endif
        }
        else
        {
            QStringList args;
            args << folder;
            QProcess::execute("umount", args);
            QDir d;
            d.rmdir(folder);
        }
    }
#endif

#ifndef Q_OS_WIN
    ::sync();
#endif

    return true;
}
