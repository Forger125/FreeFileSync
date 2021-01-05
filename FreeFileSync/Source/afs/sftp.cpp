// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "sftp.h"
#include <array>
#include <zen/sys_error.h>
#include <zen/thread.h>
#include <zen/globals.h>
#include <zen/file_io.h>
#include <zen/basic_math.h>
#include <zen/socket.h>
#include <zen/open_ssl.h>
#include <libssh2/libssh2_wrap.h> //DON'T include <libssh2_sftp.h> directly!
#include "init_curl_libssh2.h"
#include "ftp_common.h"
#include "abstract_impl.h"
#include "../base/resolve_path.h"

using namespace zen;
using namespace fff;
using AFS = AbstractFileSystem;


namespace
{
Zstring concatenateSftpFolderPathPhrase(const SftpLogin& login, const AfsPath& afsPath); //noexcept

/*
SFTP specification version 3 (implemented by libssh2): https://filezilla-project.org/specs/draft-ietf-secsh-filexfer-02.txt

libssh2: prefer OpenSSL over WinCNG backend:

WinCNG supports the following ciphers:
    rijndael-cbc@lysator.liu.se
    aes256-cbc
    aes192-cbc
    aes128-cbc
    arcfour128
    arcfour
    3des-cbc

OpenSSL supports the same ciphers like WinCNG plus the following:
    aes256-ctr
    aes192-ctr
    aes128-ctr
    cast128-cbc
    blowfish-cbc                    */

const Zchar sftpPrefix[] = Zstr("sftp:");

const std::chrono::seconds SFTP_SESSION_MAX_IDLE_TIME           (20);
const std::chrono::seconds SFTP_SESSION_CLEANUP_INTERVAL         (4); //facilitate default of 5-seconds delay for error retry
const std::chrono::seconds SFTP_CHANNEL_LIMIT_DETECTION_TIME_OUT(30);

//rw- r-- r-- [0644] default permissions for newly created files
const long SFTP_DEFAULT_PERMISSION_FILE = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;

//rwx r-x r-x [0755] default permissions for newly created folders
const long SFTP_DEFAULT_PERMISSION_FOLDER = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IXUSR |
                                            LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
                                            LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH;

//attention: if operation fails due to time out, e.g. file copy, the cleanup code may hang, too => total delay = 2 x time out interval

const size_t SFTP_OPTIMAL_BLOCK_SIZE_READ  = 8 * MAX_SFTP_READ_SIZE;     //https://github.com/libssh2/libssh2/issues/90
const size_t SFTP_OPTIMAL_BLOCK_SIZE_WRITE = 8 * MAX_SFTP_OUTGOING_SIZE; //
static_assert(MAX_SFTP_READ_SIZE == 30000 && MAX_SFTP_OUTGOING_SIZE == 30000, "reevaluate optimal block sizes if these constants change!");

/* Perf Test, Sourceforge frs, SFTP upload, compressed 25 MB test file:

SFTP_OPTIMAL_BLOCK_SIZE_READ:              SFTP_OPTIMAL_BLOCK_SIZE_WRITE:
    multiples of                               multiples of
    MAX_SFTP_READ_SIZE  KB/s                   MAX_SFTP_OUTGOING_SIZE  KB/s
                 1       650                                1          140
                 2      1000                                2          280
                 4      1800                                4          320
                 8      1800                                8          320
                16      1800                               16          320
                32      1800                               32          320
    Filezilla download speed: 1800 KB/s        Filezilla upload speed: 560 KB/s
    DSL maximum download speed: 3060 KB/s      DSL maximum upload speed: 620 KB/s


Perf Test 2: FFS hompage (2020-04-24)

SFTP_OPTIMAL_BLOCK_SIZE_READ:              SFTP_OPTIMAL_BLOCK_SIZE_WRITE:
    multiples of                               multiples of
    MAX_SFTP_READ_SIZE  MB/s                   MAX_SFTP_OUTGOING_SIZE  KB/s
                 1      0,76                                1           210
                 2      1,78                                2           430
                 4      3,80                                4           870
                 8      5,82                                8          1178
                16      5,80                               16          1178
                32      5,80                               32          1178
    Filezilla download speed: 5,62 MB/s        Filezilla upload speed: 980 KB/s
    DSL maximum download speed: 5,96 MB/s      DSL maximum upload speed: 1220 KB/s

=> libssh2_sftp_read/libssh2_sftp_write may take quite long for 16x and larger => use smallest multiple that fills bandwidth!            */


//use all configuration data that *defines* an SSH session as key when buffering sessions! This is what user expects, e.g. when changing settings in SFTP login dialog
struct SshSessionId
{
    /*explicit*/ SshSessionId(const SftpLogin& login) :
        server(login.server),
        port(login.port),
        username(login.username),
        authType(login.authType),
        password(login.password),
        privateKeyFilePath(login.privateKeyFilePath),
        allowZlib(login.allowZlib) {}

    Zstring server;
    int     port = 0;
    Zstring username;
    SftpAuthType authType = SftpAuthType::password;
    Zstring password;
    Zstring privateKeyFilePath;
    bool allowZlib = false;
    //timeoutSec, traverserChannelsPerConnection => irrelevant for session equality
};

std::weak_ordering operator<=>(const SshSessionId& lhs, const SshSessionId& rhs)
{
    //exactly the type of case insensitive comparison we need for server names!
    if (const std::weak_ordering cmp = compareAsciiNoCase(lhs.server, rhs.server); //https://docs.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfow#IDNs
        std::is_neq(cmp))
        return cmp;

    if (const std::strong_ordering cmp = std::tie(lhs.port, lhs.username, lhs.authType, lhs.allowZlib) <=> //username: case sensitive!
                                         std::tie(rhs.port, rhs.username, rhs.authType, rhs.allowZlib);
        std::is_neq(cmp))
        return cmp;

    switch (lhs.authType)
    {
        case SftpAuthType::password:
            return lhs.password <=> rhs.password; //case sensitive!

        case SftpAuthType::keyFile:
            if (const std::strong_ordering cmp = lhs.password <=> rhs.password; //case sensitive!
                std::is_neq(cmp))
                return cmp;

            return lhs.privateKeyFilePath <=> rhs.privateKeyFilePath; //case sensitive!

        case SftpAuthType::agent:
            return std::weak_ordering::equivalent;
    }
    assert(false);
    return std::weak_ordering::equivalent;
}


std::string getLibssh2Path(const AfsPath& afsPath)
{
    return utfTo<std::string>(getServerRelPath(afsPath));
}


std::wstring getSftpDisplayPath(const Zstring& serverName, const AfsPath& afsPath)
{
    Zstring displayPath = Zstring(sftpPrefix) + Zstr("//") + serverName;
    const Zstring relPath = getServerRelPath(afsPath);
    if (relPath != Zstr("/"))
        displayPath += relPath;
    return utfTo<std::wstring>(displayPath);
}
//don't show username and password!

//===========================================================================================================================

class FatalSshError //=> consider SshSession corrupted and stop use ASAP! same conceptual level like SysError
{
public:
    FatalSshError(const std::wstring& details) : details_(details) {}
    const std::wstring& toString() const { return details_; }

private:
    std::wstring details_;
};


constinit2 Global<UniSessionCounter> globalSftpSessionCount;
GLOBAL_RUN_ONCE(globalSftpSessionCount.set(createUniSessionCounter()));


class SshSession
{
public:
    SshSession(const SshSessionId& sessionId, int timeoutSec) : //throw SysError
        sessionId_(sessionId),
        libsshCurlUnifiedInitCookie_(getLibsshCurlUnifiedInitCookie(globalSftpSessionCount)) //throw SysError
    {
        ZEN_ON_SCOPE_FAIL(cleanup()); //destructor call would lead to member double clean-up!!!

        Zstring serviceName = Zstr("ssh"); //SFTP default port: 22, see %WINDIR%\system32\drivers\etc\services
        if (sessionId_.port > 0)
            serviceName = numberTo<Zstring>(sessionId_.port);

        socket_ = std::make_unique<Socket>(sessionId_.server, serviceName); //throw SysError

        sshSession_ = ::libssh2_session_init();
        if (!sshSession_) //does not set ssh last error; source: only memory allocation may fail
            throw SysError(formatSystemError("libssh2_session_init", formatSshStatusCode(LIBSSH2_ERROR_ALLOC), L""));

        //if zlib compression causes trouble, make it a user setting: https://freefilesync.org/forum/viewtopic.php?t=6663
        //=> surprise: it IS causing trouble: slow-down in local syncs: https://freefilesync.org/forum/viewtopic.php?t=7244#p24250
        if (sessionId.allowZlib)
            if (const int rc = ::libssh2_session_flag(sshSession_, LIBSSH2_FLAG_COMPRESS, 1);
                rc != 0) //does not set SSH last error
                throw SysError(formatSystemError("libssh2_session_flag", formatSshStatusCode(rc), L""));

        ::libssh2_session_set_blocking(sshSession_, 1);

        //we don't consider the timeout part of the session when it comes to reuse! but we already require it during initialization
        ::libssh2_session_set_timeout(sshSession_, timeoutSec * 1000 /*ms*/);


        if (::libssh2_session_handshake(sshSession_, socket_->get()) != 0)
            throw SysError(formatLastSshError("libssh2_session_handshake", nullptr));

        //evaluate fingerprint = libssh2_hostkey_hash(sshSession_, LIBSSH2_HOSTKEY_HASH_SHA1) ???

        const auto usernameUtf8 = utfTo<std::string>(sessionId_.username);
        const auto passwordUtf8 = utfTo<std::string>(sessionId_.password);

        const char* authList = ::libssh2_userauth_list(sshSession_, usernameUtf8);
        if (!authList)
        {
            if (::libssh2_userauth_authenticated(sshSession_) == 0)
                throw SysError(formatLastSshError("libssh2_userauth_list", nullptr));
            //else: SSH_USERAUTH_NONE has authenticated successfully => we're already done
        }
        else
        {
            bool supportAuthPassword    = false;
            bool supportAuthKeyfile     = false;
            bool supportAuthInteractive = false;
            for (const std::string& str : split<std::string>(authList, ',', SplitOnEmpty::skip))
            {
                const std::string authMethod = trimCpy(str);
                if (authMethod == "password")
                    supportAuthPassword = true;
                else if (authMethod == "publickey")
                    supportAuthKeyfile = true;
                else if (authMethod == "keyboard-interactive")
                    supportAuthInteractive = true;
            }

            switch (sessionId_.authType)
            {
                case SftpAuthType::password:
                {
                    if (supportAuthPassword)
                    {
                        if (::libssh2_userauth_password(sshSession_, usernameUtf8, passwordUtf8) != 0)
                            throw SysError(formatLastSshError("libssh2_userauth_password", nullptr));
                    }
                    else if (supportAuthInteractive) //some servers, e.g. web.sourceforge.net, support "keyboard-interactive", but not "password"
                    {
                        std::wstring unexpectedPrompts;

                        auto authCallback = [&](int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses)
                        {
                            //note: FileZilla assumes password requests when it finds "num_prompts == 1" and "!echo" -> prompt may be localized!
                            //test case: sourceforge.net sends a single "Password: " prompt with "!echo"
                            if (num_prompts == 1 && prompts[0].echo == 0)
                            {
                                responses[0].text = //pass ownership; will be ::free()d
                                    ::strdup(passwordUtf8.c_str());
                                responses[0].length = static_cast<unsigned int>(passwordUtf8.size());
                            }
                            else
                                for (int i = 0; i < num_prompts; ++i)
                                    unexpectedPrompts += (unexpectedPrompts.empty() ? L"" : L"|") + utfTo<std::wstring>(std::string(prompts[i].text, prompts[i].length));
                        };
                        using AuthCbType = decltype(authCallback);

                        auto authCallbackWrapper = [](const char* name, int name_len, const char* instruction, int instruction_len,
                                                      int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract)
                        {
                            try
                            {
                                AuthCbType* callback = *reinterpret_cast<AuthCbType**>(abstract); //free this poor little C-API from its shackles and redirect to a proper lambda
                                (*callback)(num_prompts, prompts, responses); //name, instruction are nullptr for sourceforge.net
                            }
                            catch (...) { assert(false); }
                        };

                        if (*::libssh2_session_abstract(sshSession_))
                            throw SysError(L"libssh2_session_abstract: non-null value");

                        *reinterpret_cast<AuthCbType**>(::libssh2_session_abstract(sshSession_)) = &authCallback;
                        ZEN_ON_SCOPE_EXIT(*::libssh2_session_abstract(sshSession_) = nullptr);

                        if (::libssh2_userauth_keyboard_interactive(sshSession_, usernameUtf8, authCallbackWrapper) != 0)
                            throw SysError(formatLastSshError("libssh2_userauth_keyboard_interactive", nullptr) +
                                           (unexpectedPrompts.empty() ? L"" : L"\nUnexpected prompts: " + unexpectedPrompts));
                    }
                    else
                        throw SysError(replaceCpy(_("The server does not support authentication via %x."), L"%x", L"\"username/password\"") +
                                       L'\n' +_("Required:") + L' ' + utfTo<std::wstring>(authList));
                }
                break;

                case SftpAuthType::keyFile:
                {
                    if (!supportAuthKeyfile)
                        throw SysError(replaceCpy(_("The server does not support authentication via %x."), L"%x", L"\"key file\"") +
                                       L'\n' +_("Required:") + L' ' + utfTo<std::wstring>(authList));

                    std::string passphrase = passwordUtf8;
                    std::string pkStream;
                    try
                    {
                        pkStream = getFileContent(sessionId_.privateKeyFilePath, nullptr /*notifyUnbufferedIO*/); //throw FileError
                        trim(pkStream);
                    }
                    catch (const FileError& e) { throw SysError(replaceCpy(e.toString(), L"\n\n", L'\n')); } //errors should be further enriched by context info => SysError

                    //libssh2 doesn't support the PuTTY key file format, but we do!
                    if (isPuttyKeyStream(pkStream))
                    {
                        pkStream = convertPuttyKeyToPkix(pkStream, passphrase); //throw SysError
                        passphrase.clear();
                    }

                    if (::libssh2_userauth_publickey_frommemory(sshSession_, usernameUtf8, pkStream, passphrase) != 0) //const char* passphrase
                    {
                        //libssh2_userauth_publickey_frommemory()'s "Unable to extract public key from private key" isn't exactly *helpful*
                        //=> detect invalid key files and give better error message:
                        const wchar_t* invalidKeyFormat = [&]() -> const wchar_t*
                        {
                            std::string firstLine(pkStream.begin(), std::find_if(pkStream.begin(), pkStream.end(), isLineBreak<char>));
                            trim(firstLine);

                            //"-----BEGIN PUBLIC KEY-----"      OpenSSH SSH-2 public key (X.509 SubjectPublicKeyInfo) = PKIX
                            //"-----BEGIN RSA PUBLIC KEY-----"  OpenSSH SSH-2 public key (PKCS#1 RSAPublicKey)
                            //"---- BEGIN SSH2 PUBLIC KEY ----" SSH-2 public key (RFC 4716 format)
                            if (contains(firstLine, "PUBLIC KEY"))
                                return L"OpenSSH public key";

                            if (startsWith(firstLine, "ssh-") || //ssh-rsa, ssh-dss, ssh-ed25519
                                startsWith(firstLine, "ecdsa-")) //ecdsa-sha2-nistp256, ecdsa-sha2-nistp384, ecdsa-sha2-nistp521
                                return L"OpenSSH public key"; //OpenSSH SSH-2 public key

                            if (std::count(pkStream.begin(), pkStream.end(), ' ') == 2 &&
                            std::all_of(pkStream.begin(), pkStream.end(), [](char c) { return isDigit(c) || c == ' '; }))
                            return L"SSH-1 public key";

                            if (startsWith(firstLine, "PuTTY-User-Key-File-1")) //PuTTY SSH-2 private key
                                return L"Old PuTTY v1 key"; //we only support v2!

                            //"-----BEGIN PRIVATE KEY-----"                => OpenSSH SSH-2 private key (PKCS#8 PrivateKeyInfo)          => should work
                            //"-----BEGIN ENCRYPTED PRIVATE KEY-----"      => OpenSSH SSH-2 private key (PKCS#8 EncryptedPrivateKeyInfo) => should work
                            //"-----BEGIN RSA PRIVATE KEY-----"            => OpenSSH SSH-2 private key (PKCS#1 RSAPrivateKey)           => should work
                            //"-----BEGIN DSA PRIVATE KEY-----"            => OpenSSH SSH-2 private key (PKCS#1 DSAPrivateKey)           => should work
                            //"-----BEGIN EC PRIVATE KEY-----"             => OpenSSH SSH-2 private key (PKCS#1 ECPrivateKey)            => should work
                            //"-----BEGIN OPENSSH PRIVATE KEY-----"        => OpenSSH SSH-2 private key (new format)                     => should work
                            //"---- BEGIN SSH2 ENCRYPTED PRIVATE KEY ----" => ssh.com SSH-2 private key                                  => unclear
                            //"SSH PRIVATE KEY FILE FORMAT 1.1"            => SSH-1 private key                                          => unclear
                            return nullptr; //other: maybe invalid, maybe not
                        }();
                        if (invalidKeyFormat)
                            throw SysError(_("Authentication failed.") + L' ' +
                                           replaceCpy<std::wstring>(L"%x is not an OpenSSH or PuTTY private key file.", L"%x",
                                                                    fmtPath(sessionId_.privateKeyFilePath) + L" [" + invalidKeyFormat + L']'));

                        throw SysError(formatLastSshError("libssh2_userauth_publickey_frommemory", nullptr));
                    }
                }
                break;

                case SftpAuthType::agent:
                {
                    LIBSSH2_AGENT* sshAgent = ::libssh2_agent_init(sshSession_);
                    if (!sshAgent)
                        throw SysError(formatLastSshError("libssh2_agent_init", nullptr));
                    ZEN_ON_SCOPE_EXIT(::libssh2_agent_free(sshAgent));

                    if (::libssh2_agent_connect(sshAgent) != 0)
                        throw SysError(formatLastSshError("libssh2_agent_connect", nullptr));
                    ZEN_ON_SCOPE_EXIT(::libssh2_agent_disconnect(sshAgent));

                    if (::libssh2_agent_list_identities(sshAgent) != 0)
                        throw SysError(formatLastSshError("libssh2_agent_list_identities", nullptr));

                    for (libssh2_agent_publickey* prev = nullptr;;)
                    {
                        libssh2_agent_publickey* identity = nullptr;
                        const int rc = ::libssh2_agent_get_identity(sshAgent, &identity, prev);
                        if (rc == 0) //public key returned
                            ;
                        else if (rc == 1) //no more public keys
                            throw SysError(L"SSH agent contains no matching public key.");
                        else
                            throw SysError(formatLastSshError("libssh2_agent_get_identity", nullptr));

                        if (::libssh2_agent_userauth(sshAgent, usernameUtf8.c_str(), identity) == 0)
                            break; //authentication successful

                        //else: failed => try next public key
                        prev = identity;
                    }
                }
                break;
            }
        }

        lastSuccessfulUseTime_ = std::chrono::steady_clock::now();
    }

    ~SshSession() { cleanup(); }

    const SshSessionId& getSessionId() const { return sessionId_; }

    bool isHealthy() const
    {
        for (const SftpChannelInfo& ci : sftpChannels_)
            if (ci.nbInfo.commandPending)
                return false;

        if (nbInfo_.commandPending)
            return false;

        if (possiblyCorrupted_)
            return false;

        if (std::chrono::steady_clock::now() > lastSuccessfulUseTime_ + SFTP_SESSION_MAX_IDLE_TIME)
            return false;

        return true;
    }

    void markAsCorrupted() { possiblyCorrupted_ = true; }

    struct Details
    {
        LIBSSH2_SESSION* sshSession;
        LIBSSH2_SFTP*   sftpChannel;
    };

    size_t getSftpChannelCount() const { return sftpChannels_.size(); }

    //return "false" if pending
    bool tryNonBlocking(size_t channelNo, std::chrono::steady_clock::time_point commandStartTime, const char* functionName,
                        const std::function<int(const SshSession::Details& sd)>& sftpCommand /*noexcept!*/, int timeoutSec) //throw SysError, FatalSshError
    {
        assert(::libssh2_session_get_blocking(sshSession_));
        ::libssh2_session_set_blocking(sshSession_, 0);
        ZEN_ON_SCOPE_EXIT(::libssh2_session_set_blocking(sshSession_, 1));

        //yes, we're non-blocking, still won't hurt to set the timeout in case libssh2 decides to use it nevertheless
        ::libssh2_session_set_timeout(sshSession_, timeoutSec * 1000 /*ms*/);

        LIBSSH2_SFTP* sftpChannel = channelNo < sftpChannels_.size() ? sftpChannels_[channelNo].sftpChannel : nullptr;
        SftpNonBlockInfo&  nbInfo = channelNo < sftpChannels_.size() ? sftpChannels_[channelNo].nbInfo : nbInfo_;

        if (!nbInfo.commandPending)
            assert(nbInfo.commandStartTime != commandStartTime);
        else if (nbInfo.commandStartTime == commandStartTime && nbInfo.functionName == functionName)
            ; //continue pending SFTP call
        else
        {
            assert(false); //pending sftp command is not completed by client: e.g. libssh2_sftp_close() cleaning up after a timed-out libssh2_sftp_read()
            possiblyCorrupted_ = true; //=> start new command (with new start time), but remember to not trust this session anymore!
        }
        nbInfo.commandPending   = true;
        nbInfo.commandStartTime = commandStartTime;
        nbInfo.functionName     = functionName;

        int rc = LIBSSH2_ERROR_NONE;
        try
        {
            rc = sftpCommand({ sshSession_, sftpChannel }); //noexcept
        }
        catch (...) { assert(false); rc = LIBSSH2_ERROR_BAD_USE; }

        assert(rc >= 0 || ::libssh2_session_last_errno(sshSession_) == rc);
        if (rc < 0 && ::libssh2_session_last_errno(sshSession_) != rc) //when libssh2 fails to properly set last error; e.g. https://github.com/libssh2/libssh2/pull/123
            ::libssh2_session_set_last_error(sshSession_, rc, nullptr);

        //note: even when non-blocking, libssh2 may return LIBSSH2_ERROR_TIMEOUT, but this seems to be an ordinary SSH error

        if (rc >= LIBSSH2_ERROR_NONE || rc == LIBSSH2_ERROR_SFTP_PROTOCOL)
        {
            nbInfo.commandPending = false;                             //
            lastSuccessfulUseTime_ = std::chrono::steady_clock::now(); //[!] LIBSSH2_ERROR_SFTP_PROTOCOL is NOT an SSH error => the SSH session is just fine!

            if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL)
                throw SysError(formatLastSshError(functionName, sftpChannel));
            return true;
        }
        else if (rc == LIBSSH2_ERROR_EAGAIN)
        {
            if (std::chrono::steady_clock::now() > nbInfo.commandStartTime + std::chrono::seconds(timeoutSec))
                //consider SSH session corrupted! => isHealthy() will see pending command
                throw FatalSshError(formatSystemError(functionName, formatSshStatusCode(LIBSSH2_ERROR_TIMEOUT),
                                                      _P("Operation timed out after 1 second.", "Operation timed out after %x seconds.", timeoutSec)));
            return false;
        }
        else //=> SSH session errors only (hopefully!) e.g. LIBSSH2_ERROR_SOCKET_RECV
            //consider SSH session corrupted! => isHealthy() will see pending command
            throw FatalSshError(formatLastSshError(functionName, sftpChannel));
    }

    //returns when traffic is available or time out: both cases are handled by next tryNonBlocking() call
    static void waitForTraffic(const std::vector<SshSession*>& sshSessions, int timeoutSec) //throw FatalSshError
    {
        //reference: session.c: _libssh2_wait_socket()

        SocketType nfds = 0;
        fd_set readfds  = {}; //covers FD_ZERO
        fd_set writefds = {}; //
        int readCount  = 0; //sigh: using fd_set::fd_count is not portable
        int writeCount = 0; //

        std::chrono::steady_clock::time_point startTimeMin = std::chrono::steady_clock::time_point::max();

        for (SshSession* session : sshSessions)
        {
            assert(::libssh2_session_last_errno(session->sshSession_) == LIBSSH2_ERROR_EAGAIN);
            assert(session->nbInfo_.commandPending || std::any_of(session->sftpChannels_.begin(), session->sftpChannels_.end(), [](SftpChannelInfo& ci) { return ci.nbInfo.commandPending; }));

            const int dir = ::libssh2_session_block_directions(session->sshSession_);
            assert(dir != 0); //we assert a blocked direction after libssh2 returned LIBSSH2_ERROR_EAGAIN!
            if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
            {
                if (readCount++ >= FD_SETSIZE)
                    throw FatalSshError(formatSystemError("FD_SET(readfds)", L"", _P("Cannot wait on more than 1 connection at a time.",
                                                                                     "Cannot wait on more than %x connections at a time.", FD_SETSIZE)));
                FD_SET(session->socket_->get(), &readfds);
            }
            if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
            {
                if (writeCount++ >= FD_SETSIZE)
                    throw FatalSshError(formatSystemError("FD_SET(writefds)", L"", _P("Cannot wait on more than 1 connection at a time.",
                                                                                      "Cannot wait on more than %x connections at a time.", FD_SETSIZE)));
                FD_SET(session->socket_->get(), &writefds);
            }

            nfds = std::max(nfds, session->socket_->get());

            for (SftpChannelInfo& ci : session->sftpChannels_)
                if (ci.nbInfo.commandPending)
                    startTimeMin = std::min(startTimeMin, ci.nbInfo.commandStartTime);
            if (session->nbInfo_.commandPending)
                startTimeMin = std::min(startTimeMin, session->nbInfo_.commandStartTime);
        }

        if (readCount > 0 || writeCount > 0)
        {
            assert(startTimeMin != std::chrono::steady_clock::time_point::max());
            const auto now = std::chrono::steady_clock::now();
            const auto endTime = startTimeMin + std::chrono::seconds(timeoutSec);
            if (now > endTime)
                return; //time-out! => let next tryNonBlocking() call fail with detailed error!
            const auto waitTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - now).count();

            struct ::timeval tv = {};
            tv.tv_sec  = static_cast<long>(waitTimeMs / 1000);
            tv.tv_usec = static_cast<long>(waitTimeMs - tv.tv_sec * 1000) * 1000;

            //WSAPoll is broken, ::poll() on macOS even worse: https://daniel.haxx.se/blog/2012/10/10/wsapoll-is-broken/
            //perf: no significant difference compared to ::WSAPoll()
            const int rc = ::select(nfds + 1, //int nfds,
                                    readCount  > 0 ? &readfds  : nullptr, //fd_set* readfds,
                                    writeCount > 0 ? &writefds : nullptr, //fd_set* writefds,
                                    nullptr,  //fd_set* exceptfds,
                                    &tv);     //struct timeval* timeout
            if (rc == 0)
                return; //time-out! => let next tryNonBlocking() call fail with detailed error!
            if (rc < 0)
                //consider SSH sessions corrupted! => isHealthy() will see pending commands
                throw FatalSshError(formatSystemError("select", getLastError()));
        }
        else assert(false);
    }

    static void addSftpChannel(const std::vector<SshSession*>& sshSessions, int timeoutSec) //throw SysError, FatalSshError
    {
        auto addChannelDetails = [](const std::wstring& msg, SshSession& sshSession) //when hitting the server's SFTP channel limit, inform user about channel number
        {
            if (sshSession.sftpChannels_.empty())
                return msg;
            return msg + L' ' + replaceCpy(_("Failed to open SFTP channel number %x."), L"%x", numberTo<std::wstring>(sshSession.sftpChannels_.size() + 1));
        };

        std::optional<SysError>      firstSysError;
        std::optional<FatalSshError> firstFatalError;

        std::vector<SshSession*> pendingSessions = sshSessions;
        const auto sftpCommandStartTime = std::chrono::steady_clock::now();

        for (;;)
        {
            //create all SFTP sessions in parallel => non-blocking
            //note: each libssh2_sftp_init() consists of multiple round-trips => poll until all sessions are finished, don't just init and then block on each!
            for (size_t pos = pendingSessions.size(); pos-- > 0 ; ) //CAREFUL WITH THESE ERASEs (invalidate positions!!!)
                try
                {
                    if (pendingSessions[pos]->tryNonBlocking(static_cast<size_t>(-1), sftpCommandStartTime, "libssh2_sftp_init",
                                                             [&](const SshSession::Details& sd) //noexcept!
                {
                    LIBSSH2_SFTP* sftpChannelNew = ::libssh2_sftp_init(sd.sshSession);
                        if (!sftpChannelNew)
                            return std::min(::libssh2_session_last_errno(sd.sshSession), LIBSSH2_ERROR_SOCKET_NONE);
                        //just in case libssh2 failed to properly set last error; e.g. https://github.com/libssh2/libssh2/pull/123

                        pendingSessions[pos]->sftpChannels_.emplace_back(sftpChannelNew);
                        return LIBSSH2_ERROR_NONE;
                    }, timeoutSec)) //throw SysError, FatalSshError
                    pendingSessions.erase(pendingSessions.begin() + pos); //= not pending
                }
                catch (const SysError& e)
                {
                    if (!firstSysError) //don't throw yet and corrupt other valid, but pending SshSessions! We also don't want to leak LIBSSH2_SFTP* waiting in libssh2 code
                        firstSysError = SysError(addChannelDetails(e.toString(), *pendingSessions[pos]));
                    pendingSessions.erase(pendingSessions.begin() + pos);
                }
                catch (const FatalSshError& e)
                {
                    if (!firstFatalError)
                        firstFatalError = FatalSshError(addChannelDetails(e.toString(), *pendingSessions[pos]));
                    pendingSessions.erase(pendingSessions.begin() + pos);
                }

            if (pendingSessions.empty())
            {
                if (firstFatalError) //throw FatalSshError *before* SysError (later can be retried)
                    throw* firstFatalError;
                if (firstSysError)
                    throw* firstSysError;
                return;
            }

            waitForTraffic(pendingSessions, timeoutSec); //throw FatalSshError
        }
    }

private:
    SshSession           (const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    void cleanup() //attention: may block heavily after error!
    {
        for (SftpChannelInfo& ci : sftpChannels_)
            //ci.nbInfo.commandPending? => may "legitimately" happen when an SFTP command times out
            ::libssh2_sftp_shutdown(ci.sftpChannel);

        if (sshSession_)
        {
            if (!nbInfo_.commandPending && std::all_of(sftpChannels_.begin(), sftpChannels_.end(),
            [](const SftpChannelInfo& ci) { return !ci.nbInfo.commandPending; }))
            ::libssh2_session_disconnect(sshSession_, "FreeFileSync says \"bye\"!"); //= server notification only! no local cleanup apparently
            //else: avoid further stress on the broken SSH session and take French leave

            //nbInfo_.commandPending? => have to clean up, no matter what!
            ::libssh2_session_free(sshSession_);
        }
    }

    std::wstring formatLastSshError(const char* functionName, LIBSSH2_SFTP* sftpChannel /*optional*/) const
    {
        char* lastErrorMsg = nullptr; //owned by "sshSession"
        const int sshStatusCode = ::libssh2_session_last_error(sshSession_, &lastErrorMsg, nullptr, false /*want_buf*/);
        assert(lastErrorMsg);

        std::wstring errorMsg;
        if (lastErrorMsg)
            errorMsg = trimCpy(utfTo<std::wstring>(lastErrorMsg));

        if (sftpChannel && sshStatusCode == LIBSSH2_ERROR_SFTP_PROTOCOL)
            errorMsg += (errorMsg.empty() ? L"" : L" - ") + formatSftpStatusCode(::libssh2_sftp_last_error(sftpChannel));

        return formatSystemError(functionName, formatSshStatusCode(sshStatusCode), errorMsg);
    }

    struct SftpNonBlockInfo
    {
        bool commandPending = false;
        std::chrono::steady_clock::time_point commandStartTime; //specified by client, try to detect libssh2 usage errors
        std::string functionName;
    };

    struct SftpChannelInfo
    {
        explicit SftpChannelInfo(LIBSSH2_SFTP* sc) : sftpChannel(sc) {}

        LIBSSH2_SFTP* sftpChannel = nullptr;
        SftpNonBlockInfo nbInfo;
    };

    std::unique_ptr<Socket> socket_; //*bound* after constructor has run
    LIBSSH2_SESSION* sshSession_ = nullptr;
    std::vector<SftpChannelInfo> sftpChannels_;
    bool possiblyCorrupted_ = false;

    SftpNonBlockInfo nbInfo_; //for SSH session, e.g. libssh2_sftp_init()

    const SshSessionId sessionId_;
    const std::shared_ptr<UniCounterCookie> libsshCurlUnifiedInitCookie_;
    std::chrono::steady_clock::time_point lastSuccessfulUseTime_; //...of the SSH session (but not necessarily the SFTP functionality!)
};

//===========================================================================================================================
//===========================================================================================================================

class SftpSessionManager //reuse (healthy) SFTP sessions globally
{
    struct IdleSshSessions;

public:
    SftpSessionManager() : sessionCleaner_([this]
    {
        setCurrentThreadName(Zstr("Session Cleaner[SFTP]"));
        runGlobalSessionCleanUp(); /*throw ThreadStopRequest*/
    }) {}

    struct ReUseOnDelete
    {
        void operator()(SshSession* s) const;
    };

    class SshSessionShared
    {
    public:
        SshSessionShared(std::unique_ptr<SshSession, ReUseOnDelete>&& idleSession, int timeoutSec) :
            session_(std::move(idleSession)) /*bound!*/, timeoutSec_(timeoutSec) { /*assert(session_->isHealthy());*/ }

        //we need two-step initialization: 1. constructor is FAST and noexcept 2. init() is SLOW and throws
        void init() //throw SysError, FatalSshError
        {
            if (session_->getSftpChannelCount() == 0) //make sure the SSH session contains at least one SFTP channel
                SshSession::addSftpChannel({ session_.get() }, timeoutSec_); //throw SysError, FatalSshError
        }

        //bool isHealthy() const { return session_->isHealthy(); }

        void executeBlocking(const char* functionName, const std::function<int(const SshSession::Details& sd)>& sftpCommand /*noexcept!*/) //throw SysError, FatalSshError
        {
            assert(threadId_ == std::this_thread::get_id());
            assert(session_->getSftpChannelCount() > 0);
            const auto sftpCommandStartTime = std::chrono::steady_clock::now();

            for (;;)
                if (session_->tryNonBlocking(0, sftpCommandStartTime, functionName, sftpCommand, timeoutSec_)) //throw SysError, FatalSshError
                    return;
                else //pending
                    SshSession::waitForTraffic({ session_.get() }, timeoutSec_); //throw FatalSshError
        }

    private:
        std::unique_ptr<SshSession, ReUseOnDelete> session_; //bound!
        const std::thread::id threadId_ = std::this_thread::get_id();
        const int timeoutSec_;
    };

    class SshSessionExclusive
    {
    public:
        SshSessionExclusive(std::unique_ptr<SshSession, ReUseOnDelete>&& idleSession, int timeoutSec) :
            session_(std::move(idleSession)) /*bound!*/, timeoutSec_(timeoutSec) { /*assert(session_->isHealthy());*/ }

        bool tryNonBlocking(size_t channelNo, std::chrono::steady_clock::time_point commandStartTime, const char* functionName, //throw SysError, FatalSshError
                            const std::function<int(const SshSession::Details& sd)>& sftpCommand /*noexcept!*/)
        {
            return session_->tryNonBlocking(channelNo, commandStartTime, functionName, sftpCommand, timeoutSec_); //throw SysError, FatalSshError
        }

        void finishBlocking(size_t channelNo, std::chrono::steady_clock::time_point commandStartTime, const char* functionName,
                            const std::function<int(const SshSession::Details& sd)>& sftpCommand /*noexcept!*/)
        {
            for (;;)
                try
                {
                    if (session_->tryNonBlocking(channelNo, commandStartTime, functionName, sftpCommand, timeoutSec_)) //throw SysError, FatalSshError
                        return;
                    else //pending
                        SshSession::waitForTraffic({ session_.get() }, timeoutSec_); //throw FatalSshError
                }
                catch (const SysError&     ) { return; }
                catch (const FatalSshError&) { return; }
        }

        size_t getSftpChannelCount() const { return session_->getSftpChannelCount(); }
        void markAsCorrupted() { session_->markAsCorrupted(); }

        static void addSftpChannel(const std::vector<SshSessionExclusive*>& exSessions) //throw SysError, FatalSshError
        {
            std::vector<SshSession*> sshSessions;
            for (SshSessionExclusive* exSession : exSessions)
                sshSessions.push_back(exSession->session_.get());

            int timeoutSec = 0;
            for (SshSessionExclusive* exSession : exSessions)
                timeoutSec = std::max(timeoutSec, exSession->timeoutSec_);

            SshSession::addSftpChannel(sshSessions, timeoutSec); //throw SysError, FatalSshError
        }

        static void waitForTraffic(const std::vector<SshSessionExclusive*>& exSessions) //throw FatalSshError
        {
            std::vector<SshSession*> sshSessions;
            for (SshSessionExclusive* exSession : exSessions)
                sshSessions.push_back(exSession->session_.get());

            int timeoutSec = 0;
            for (SshSessionExclusive* exSession : exSessions)
                timeoutSec = std::max(timeoutSec, exSession->timeoutSec_);

            SshSession::waitForTraffic(sshSessions, timeoutSec); //throw FatalSshError
        }

        Zstring getServerName() const { return session_->getSessionId().server; }

    private:
        std::unique_ptr<SshSession, ReUseOnDelete> session_; //bound!
        const int timeoutSec_;
    };


    std::shared_ptr<SshSessionShared> getSharedSession(const SftpLogin& login) //throw SysError
    {
        Protected<IdleSshSessions>& sessionStore = getSessionStore(login);

        const std::thread::id threadId = std::this_thread::get_id();
        std::shared_ptr<SshSessionShared> sharedSession; //no need to protect against concurrency: same thread!

        sessionStore.access([&](IdleSshSessions& sessions)
        {
            std::weak_ptr<SshSessionShared>& sharedSessionWeak = sessions.sshSessionsWithThreadAffinity[threadId]; //get or create
            if (auto session = sharedSessionWeak.lock())
                //dereference session ONLY after affinity to THIS thread was confirmed!!!
                //assume "isHealthy()" to avoid hitting server connection limits: (clean up of !isHealthy() after use; idle sessions via worker thread)
                sharedSession = session;

            if (!sharedSession)
                //assume "isHealthy()" to avoid hitting server connection limits: (clean up of !isHealthy() after use; idle sessions via worker thread)
                if (!sessions.idleSshSessions.empty())
                {
                    std::unique_ptr<SshSession, ReUseOnDelete> sshSession(sessions.idleSshSessions.back().release());
                    /**/                                                  sessions.idleSshSessions.pop_back();
                    sharedSessionWeak = sharedSession = std::make_shared<SshSessionShared>(std::move(sshSession), login.timeoutSec); //still holding lock => constructor must be *fast*!
                }
        });

        //create new SFTP session outside the lock: 1. don't block other threads 2. non-atomic regarding "sessionStore"! => one session too many is not a problem!
        if (!sharedSession)
        {
            sharedSession = std::make_shared<SshSessionShared>(std::unique_ptr<SshSession, ReUseOnDelete>(new SshSession(login, login.timeoutSec)), login.timeoutSec); //throw SysError
            sessionStore.access([&](IdleSshSessions& sessions)
            {
                sessions.sshSessionsWithThreadAffinity[threadId] = sharedSession;
            });
        }

        //finish two-step initialization outside the lock: SLOW!
        try
        {
            sharedSession->init(); //throw SysError, FatalSshError
        }
        catch (const FatalSshError& e) { throw SysError(e.toString()); } //session corrupted => is not returned => no special handling required

        return sharedSession;
    }


    std::unique_ptr<SshSessionExclusive> getExclusiveSession(const SftpLogin& login) //throw SysError
    {
        Protected<IdleSshSessions>& sessionStore = getSessionStore(login);

        std::unique_ptr<SshSession, ReUseOnDelete> sshSession;

        sessionStore.access([&](IdleSshSessions& sessions)
        {
            //assume "isHealthy()" to avoid hitting server connection limits: (clean up of !isHealthy() after use, idle sessions via worker thread)
            if (!sessions.idleSshSessions.empty())
            {
                sshSession.reset(sessions.idleSshSessions.back().release());
                /**/             sessions.idleSshSessions.pop_back();
            }
        });

        //create new SFTP session outside the lock: 1. don't block other threads 2. non-atomic regarding "sessionStore"! => one session too many is not a problem!
        if (!sshSession)
            sshSession.reset(new SshSession(login, login.timeoutSec)); //throw SysError

        return std::make_unique<SshSessionExclusive>(std::move(sshSession), login.timeoutSec);
    }

private:
    SftpSessionManager           (const SftpSessionManager&) = delete;
    SftpSessionManager& operator=(const SftpSessionManager&) = delete;

    Protected<IdleSshSessions>& getSessionStore(const SshSessionId& sessionId)
    {
        //single global session store per login; life-time bound to globalInstance => never remove a sessionStore!!!
        Protected<IdleSshSessions>* store = nullptr;

        globalSessionStore_.access([&](GlobalSshSessions& sessionsById)
        {
            store = &sessionsById[sessionId]; //get or create
        });
        static_assert(std::is_same_v<GlobalSshSessions, std::map<SshSessionId, Protected<IdleSshSessions>>>, "require std::map so that the pointers we return remain stable");

        return *store;
    }

    //run a dedicated clean-up thread => it's unclear when the server let's a connection time out, so we do it preemptively
    //context of worker thread:
    void runGlobalSessionCleanUp() //throw ThreadStopRequest
    {
        std::chrono::steady_clock::time_point lastCleanupTime;
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();

            if (now < lastCleanupTime + SFTP_SESSION_CLEANUP_INTERVAL)
                interruptibleSleep(lastCleanupTime + SFTP_SESSION_CLEANUP_INTERVAL - now); //throw ThreadStopRequest

            lastCleanupTime = std::chrono::steady_clock::now();

            std::vector<Protected<IdleSshSessions>*> sessionStores; //pointers remain stable, thanks to std::map<>

            globalSessionStore_.access([&](GlobalSshSessions& sessionsById)
            {
                for (auto& [sessionId, idleSession] : sessionsById)
                    sessionStores.push_back(&idleSession);
            });

            for (Protected<IdleSshSessions>* sessionStore : sessionStores)
                for (bool done = false; !done;)
                    sessionStore->access([&](IdleSshSessions& sessions)
                {
                    for (std::unique_ptr<SshSession>& sshSession : sessions.idleSshSessions)
                        if (!sshSession->isHealthy()) //!isHealthy() sessions are destroyed after use => in this context this means they have been idle for too long
                        {
                            sshSession.swap(sessions.idleSshSessions.back());
                            /**/            sessions.idleSshSessions.pop_back(); //run ~SshSession *inside* the lock! => avoid hitting server limits!
                            std::this_thread::yield();
                            return; //don't hold lock for too long: delete only one session at a time, then yield...
                        }
                    std::erase_if(sessions.sshSessionsWithThreadAffinity, [](const auto& v) { return !v.second.lock(); }); //clean up dangling weak pointer
                    done = true;
                });
        }
    }

    struct IdleSshSessions
    {
        std::vector<std::unique_ptr<SshSession>>                   idleSshSessions; //extract *temporarily* from this list during use
        std::map<std::thread::id, std::weak_ptr<SshSessionShared>> sshSessionsWithThreadAffinity; //Win32 thread IDs may be REUSED! still, shouldn't be a problem...
    };

    using GlobalSshSessions = std::map<SshSessionId, Protected<IdleSshSessions>>;

    Protected<GlobalSshSessions> globalSessionStore_;
    InterruptibleThread sessionCleaner_;
};

//--------------------------------------------------------------------------------------
UniInitializer globalStartupInitSftp(*globalSftpSessionCount.get());

constinit2 Global<SftpSessionManager> globalSftpSessionManager; //caveat: life time must be subset of static UniInitializer!
//--------------------------------------------------------------------------------------


void SftpSessionManager::ReUseOnDelete::operator()(SshSession* s) const
{
    //assert(s); -> custom deleter is only called on non-null pointer
    if (s->isHealthy()) //thread that created the "!isHealthy()" session is responsible for clean up (avoid hitting server connection limits!)
        if (std::shared_ptr<SftpSessionManager> mgr = globalSftpSessionManager.get())
        {
            Protected<IdleSshSessions>& sessionStore = mgr->getSessionStore(s->getSessionId());
            sessionStore.access([&](IdleSshSessions& sessions)
            {
                sessions.idleSshSessions.emplace_back(s); //pass ownership
            });
            return;
        }

    delete s;
}


std::shared_ptr<SftpSessionManager::SshSessionShared> getSharedSftpSession(const SftpLogin& login) //throw SysError
{
    if (const std::shared_ptr<SftpSessionManager> mgr = globalSftpSessionManager.get())
        return mgr->getSharedSession(login); //throw SysError

    throw SysError(formatSystemError("getSharedSftpSession", L"", L"Function call not allowed during init/shutdown."));
}


std::unique_ptr<SftpSessionManager::SshSessionExclusive> getExclusiveSftpSession(const SftpLogin& login) //throw SysError
{
    if (const std::shared_ptr<SftpSessionManager> mgr = globalSftpSessionManager.get())
        return mgr->getExclusiveSession(login); //throw SysError

    throw SysError(formatSystemError("getExclusiveSftpSession", L"", L"Function call not allowed during init/shutdown."));
}


void runSftpCommand(const SftpLogin& login, const char* functionName,
                    const std::function<int(const SshSession::Details& sd)>& sftpCommand /*noexcept!*/) //throw SysError
{
    std::shared_ptr<SftpSessionManager::SshSessionShared> asyncSession = getSharedSftpSession(login); //throw SysError
    //no need to protect against concurrency: shared session is (temporarily) bound to current thread
    try
    {
        asyncSession->executeBlocking(functionName, sftpCommand); //throw SysError, FatalSshError
    }
    catch (const FatalSshError& e) { throw SysError(e.toString()); } //SSH session corrupted! => we stop using session => map to SysError is okay
}

//===========================================================================================================================
//===========================================================================================================================
struct SftpItemDetails
{
    AFS::ItemType type;
    uint64_t      fileSize;
    time_t        modTime;
};
struct SftpItem
{
    Zstring         itemName;
    SftpItemDetails details;
};
std::vector<SftpItem> getDirContentFlat(const SftpLogin& login, const AfsPath& dirPath) //throw FileError
{
    LIBSSH2_SFTP_HANDLE* dirHandle = nullptr;
    try
    {
        runSftpCommand(login, "libssh2_sftp_opendir", //throw SysError
                       [&](const SshSession::Details& sd) //noexcept!
        {
            dirHandle = ::libssh2_sftp_opendir(sd.sftpChannel, getLibssh2Path(dirPath));
            if (!dirHandle)
                return std::min(::libssh2_session_last_errno(sd.sshSession), LIBSSH2_ERROR_SOCKET_NONE);
            return LIBSSH2_ERROR_NONE;
        });
    }
    catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot open directory %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, dirPath))), e.toString()); }

    ZEN_ON_SCOPE_EXIT(try
    {
        runSftpCommand(login, "libssh2_sftp_closedir", //throw SysError
        [&](const SshSession::Details& sd) { return ::libssh2_sftp_closedir(dirHandle); }); //noexcept!
    }
    catch (SysError&) {});

    std::vector<SftpItem> output;
    for (;;)
    {
        std::array<char, 1024> buf; //libssh2 sample code uses 512; in practice NAME_MAX(255)+1 should suffice: https://serverfault.com/questions/9546/filename-length-limits-on-linux
        LIBSSH2_SFTP_ATTRIBUTES attribs = {};
        int rc = 0;
        try
        {
            runSftpCommand(login, "libssh2_sftp_readdir", //throw SysError
            [&](const SshSession::Details& sd) { return rc = ::libssh2_sftp_readdir(dirHandle, &buf[0], buf.size(), &attribs); }); //noexcept!
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read directory %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, dirPath))), e.toString()); }

        if (rc == 0) //no more items
            return output;

        const std::string_view sftpItemName = makeStringView(&buf[0], rc);

        if (sftpItemName == "." || sftpItemName == "..") //check needed for SFTP, too!
            continue;

        const Zstring& itemName = utfTo<Zstring>(sftpItemName);
        const AfsPath itemPath(nativeAppendPaths(dirPath.value, itemName));

        if ((attribs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) == 0) //server probably does not support these attributes => fail at folder level
            throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, itemPath))), L"File attributes not available.");

        if (LIBSSH2_SFTP_S_ISLNK(attribs.permissions))
        {
            if ((attribs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) == 0) //server probably does not support these attributes => fail at folder level
                throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, itemPath))), L"Modification time not supported.");
            output.push_back({ itemName, { AFS::ItemType::symlink, 0, static_cast<time_t>(attribs.mtime) }});
        }
        else if (LIBSSH2_SFTP_S_ISDIR(attribs.permissions))
            output.push_back({ itemName, { AFS::ItemType::folder, 0, static_cast<time_t>(attribs.mtime) }});
        else //a file or named pipe, ect: LIBSSH2_SFTP_S_ISREG, LIBSSH2_SFTP_S_ISCHR, LIBSSH2_SFTP_S_ISBLK, LIBSSH2_SFTP_S_ISFIFO, LIBSSH2_SFTP_S_ISSOCK
        {
            if ((attribs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) == 0) //server probably does not support these attributes => fail at folder level
                throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, itemPath))), L"Modification time not supported.");
            if ((attribs.flags & LIBSSH2_SFTP_ATTR_SIZE) == 0)
                throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, itemPath))), L"File size not supported.");
            output.push_back({ itemName, { AFS::ItemType::file, attribs.filesize, static_cast<time_t>(attribs.mtime) }});
        }
    }
}


SftpItemDetails getSymlinkTargetDetails(const SftpLogin& login, const AfsPath& linkPath) //throw FileError
{
    LIBSSH2_SFTP_ATTRIBUTES attribsTrg = {};
    try
    {
        runSftpCommand(login, "libssh2_sftp_stat", //throw SysError
        [&](const SshSession::Details& sd) { return ::libssh2_sftp_stat(sd.sftpChannel, getLibssh2Path(linkPath), &attribsTrg); }); //noexcept!
    }
    catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, linkPath))), e.toString()); }

    if ((attribsTrg.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) == 0)
        throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, linkPath))), L"File attributes not available.");

    if (LIBSSH2_SFTP_S_ISDIR(attribsTrg.permissions))
        return { AFS::ItemType::folder, 0, static_cast<time_t>(attribsTrg.mtime) };
    else
    {
        if ((attribsTrg.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) == 0) //server probably does not support these attributes => should fail at folder level!
            throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, linkPath))), L"Modification time not supported.");
        if ((attribsTrg.flags & LIBSSH2_SFTP_ATTR_SIZE) == 0)
            throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getSftpDisplayPath(login.server, linkPath))), L"File size not supported.");

        return { AFS::ItemType::file, attribsTrg.filesize, static_cast<time_t>(attribsTrg.mtime) };
    }
}


class SingleFolderTraverser
{
public:
    using WorkItem = std::pair<AfsPath, std::shared_ptr<AFS::TraverserCallback>>;

    SingleFolderTraverser(const SftpLogin& login, const std::vector<std::pair<AfsPath, std::shared_ptr<AFS::TraverserCallback>>>& workload /*throw X*/) :
        login_(login)
    {
        for (const auto& [folderPath, cb] : workload)
            workload_.push_back(WorkItem{folderPath, cb});

        while (!workload_.empty())
        {
            auto wi = std::move(workload_.    front()); //yes, no strong exception guarantee (std::bad_alloc)
            /**/                workload_.pop_front();  //
            const auto& [folderPath, cb] = wi;

            tryReportingDirError([&] //throw X
            {
                traverseWithException(folderPath, *cb); //throw FileError, X
            }, *cb);
        }
    }

private:
    SingleFolderTraverser           (const SingleFolderTraverser&) = delete;
    SingleFolderTraverser& operator=(const SingleFolderTraverser&) = delete;

    void traverseWithException(const AfsPath& dirPath, AFS::TraverserCallback& cb) //throw FileError, X
    {
        for (const SftpItem& item : getDirContentFlat(login_, dirPath)) //throw FileError
        {
            const AfsPath itemPath(nativeAppendPaths(dirPath.value, item.itemName));

            switch (item.details.type)
            {
                case AFS::ItemType::file:
                    cb.onFile({ item.itemName, item.details.fileSize, item.details.modTime, AFS::FileId(), false /*isFollowedSymlink*/ }); //throw X
                    break;

                case AFS::ItemType::folder:
                    if (std::shared_ptr<AFS::TraverserCallback> cbSub = cb.onFolder({ item.itemName, false /*isFollowedSymlink*/ })) //throw X
                        workload_.push_back(WorkItem{ itemPath, std::move(cbSub) });
                    break;

                case AFS::ItemType::symlink:
                    switch (cb.onSymlink({ item.itemName, item.details.modTime })) //throw X
                    {
                        case AFS::TraverserCallback::LINK_FOLLOW:
                        {
                            SftpItemDetails targetDetails = {};
                            if (!tryReportingItemError([&] //throw X
                        {
                            targetDetails = getSymlinkTargetDetails(login_, itemPath); //throw FileError
                            }, cb, item.itemName))
                            continue;

                            if (targetDetails.type == AFS::ItemType::folder)
                            {
                                if (std::shared_ptr<AFS::TraverserCallback> cbSub = cb.onFolder({ item.itemName, true /*isFollowedSymlink*/ })) //throw X
                                    workload_.push_back(WorkItem{ itemPath, std::move(cbSub) });
                            }
                            else //a file or named pipe, etc.
                                cb.onFile({ item.itemName, targetDetails.fileSize, targetDetails.modTime, AFS::FileId(), true /*isFollowedSymlink*/ }); //throw X
                        }
                        break;

                        case AFS::TraverserCallback::LINK_SKIP:
                            break;
                    }
                    break;
            }
        }
    }

    const SftpLogin login_;
    RingBuffer<WorkItem> workload_;
};


void traverseFolderRecursiveSftp(const SftpLogin& login, const std::vector<std::pair<AfsPath, std::shared_ptr<AFS::TraverserCallback>>>& workload /*throw X*/, size_t) //throw X
{
    SingleFolderTraverser dummy(login, workload); //throw X
}

//===========================================================================================================================

struct InputStreamSftp : public AFS::InputStream
{
    InputStreamSftp(const SftpLogin& login, const AfsPath& filePath, const IOCallback& notifyUnbufferedIO /*throw X*/) : //throw FileError
        displayPath_(getSftpDisplayPath(login.server, filePath)),
        notifyUnbufferedIO_(notifyUnbufferedIO)
    {
        try
        {
            session_ = getSharedSftpSession(login); //throw SysError

            session_->executeBlocking("libssh2_sftp_open", //throw SysError, FatalSshError
                                      [&](const SshSession::Details& sd) //noexcept!
            {
                fileHandle_ = ::libssh2_sftp_open(sd.sftpChannel, getLibssh2Path(filePath), LIBSSH2_FXF_READ, 0);
                if (!fileHandle_)
                    return std::min(::libssh2_session_last_errno(sd.sshSession), LIBSSH2_ERROR_SOCKET_NONE);
                return LIBSSH2_ERROR_NONE;
            });
        }
        catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot open file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
        catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot open file %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => stop using session
    }

    ~InputStreamSftp()
    {
        try
        {
            session_->executeBlocking("libssh2_sftp_close", //throw SysError, FatalSshError
            [&](const SshSession::Details& sd) { return ::libssh2_sftp_close(fileHandle_); }); //noexcept!
        }
        catch (const SysError&) {}
        catch (const FatalSshError&) {} //SSH session corrupted! => stop using session
    }

    size_t read(void* buffer, size_t bytesToRead) override //throw FileError, (ErrorFileLocked), X; return "bytesToRead" bytes unless end of stream!
    {
        const size_t blockSize = getBlockSize();
        assert(memBuf_.size() >= blockSize);
        assert(bufPos_ <= bufPosEnd_ && bufPosEnd_ <= memBuf_.size());

        auto       it    = static_cast<std::byte*>(buffer);
        const auto itEnd = it + bytesToRead;
        for (;;)
        {
            const size_t junkSize = std::min(static_cast<size_t>(itEnd - it), bufPosEnd_ - bufPos_);
            std::memcpy(it, &memBuf_[0] + bufPos_, junkSize);
            bufPos_ += junkSize;
            it      += junkSize;

            if (it == itEnd)
                break;
            //--------------------------------------------------------------------
            const size_t bytesRead = tryRead(&memBuf_[0], blockSize); //throw FileError; may return short, only 0 means EOF! => CONTRACT: bytesToRead > 0
            bufPos_ = 0;
            bufPosEnd_ = bytesRead;

            if (notifyUnbufferedIO_) notifyUnbufferedIO_(bytesRead); //throw X

            if (bytesRead == 0) //end of file
                break;
        }
        return it - static_cast<std::byte*>(buffer);
    }

    size_t getBlockSize() const override { return SFTP_OPTIMAL_BLOCK_SIZE_READ; } //non-zero block size is AFS contract!

    std::optional<AFS::StreamAttributes> getAttributesBuffered() override //throw FileError
    {
        return {}; //although have an SFTP stream handle, attribute access requires an extra (expensive) round-trip!
        //PERF: test case 148 files, 1MB: overall copy time increases by 20% if libssh2_sftp_fstat() gets called per each file
    }

private:
    size_t tryRead(void* buffer, size_t bytesToRead) //throw FileError; may return short, only 0 means EOF! => CONTRACT: bytesToRead > 0
    {
        //libssh2_sftp_read has same semantics as Posix read:
        if (bytesToRead == 0) //"read() with a count of 0 returns zero" => indistinguishable from end of file! => check!
            throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));
        assert(bytesToRead == getBlockSize());

        ssize_t bytesRead = 0;
        try
        {
            session_->executeBlocking("libssh2_sftp_read", //throw SysError, FatalSshError
                                      [&](const SshSession::Details& sd) //noexcept!
            {
                bytesRead = ::libssh2_sftp_read(fileHandle_, static_cast<char*>(buffer), bytesToRead);
                return static_cast<int>(bytesRead);
            });

            if (static_cast<size_t>(bytesRead) > bytesToRead) //better safe than sorry
                throw SysError(formatSystemError("libssh2_sftp_read", L"", L"Buffer overflow.")); //user should never see this
        }
        catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
        catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => caller (will/should) stop using session

        return bytesRead; //"zero indicates end of file"
    }

    const std::wstring displayPath_;
    LIBSSH2_SFTP_HANDLE* fileHandle_ = nullptr;
    const IOCallback notifyUnbufferedIO_; //throw X
    std::shared_ptr<SftpSessionManager::SshSessionShared> session_;

    std::vector<std::byte> memBuf_ = std::vector<std::byte>(getBlockSize());
    size_t bufPos_    = 0; //buffered I/O; see file_io.cpp
    size_t bufPosEnd_ = 0; //
};

//===========================================================================================================================

//libssh2_sftp_open fails with generic LIBSSH2_FX_FAILURE if already existing
struct OutputStreamSftp : public AFS::OutputStreamImpl
{
    OutputStreamSftp(const SftpLogin& login, //throw FileError
                     const AfsPath& filePath,
                     std::optional<time_t> modTime,
                     const IOCallback& notifyUnbufferedIO /*throw X*/) :
        filePath_(filePath),
        displayPath_(getSftpDisplayPath(login.server, filePath)),
        modTime_(modTime),
        notifyUnbufferedIO_(notifyUnbufferedIO)
    {
        try
        {
            session_ = getSharedSftpSession(login); //throw SysError

            session_->executeBlocking("libssh2_sftp_open", //throw SysError, FatalSshError
                                      [&](const SshSession::Details& sd) //noexcept!
            {
                fileHandle_ = ::libssh2_sftp_open(sd.sftpChannel, getLibssh2Path(filePath),
                                                  LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
                                                  SFTP_DEFAULT_PERMISSION_FILE); //note: server may also apply umask! (e.g. 0022 for ffs.org)
                if (!fileHandle_)
                    return std::min(::libssh2_session_last_errno(sd.sshSession), LIBSSH2_ERROR_SOCKET_NONE);
                return LIBSSH2_ERROR_NONE;
            });
        }
        catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
        catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => stop using session

        //NOTE: fileHandle_ still unowned until end of constructor!!!

        //pre-allocate file space? not supported
    }

    ~OutputStreamSftp()
    {
        if (fileHandle_)
            try
            {
                close(); //throw FileError
            }
            catch (FileError&) {}
    }

    void write(const void* buffer, size_t bytesToWrite) override //throw FileError, X
    {
        const size_t blockSize = getBlockSize();
        assert(memBuf_.size() >= blockSize);
        assert(bufPos_ <= bufPosEnd_ && bufPosEnd_ <= memBuf_.size());

        auto       it    = static_cast<const std::byte*>(buffer);
        const auto itEnd = it + bytesToWrite;
        for (;;)
        {
            if (memBuf_.size() - bufPos_ < blockSize) //support memBuf_.size() > blockSize to reduce memmove()s, but perf test shows: not really needed!
                // || bufPos_ == bufPosEnd_) -> not needed while memBuf_.size() == blockSize
            {
                std::memmove(&memBuf_[0], &memBuf_[0] + bufPos_, bufPosEnd_ - bufPos_);
                bufPosEnd_ -= bufPos_;
                bufPos_ = 0;
            }

            const size_t junkSize = std::min(static_cast<size_t>(itEnd - it), blockSize - (bufPosEnd_ - bufPos_));
            std::memcpy(&memBuf_[0] + bufPosEnd_, it, junkSize);
            bufPosEnd_ += junkSize;
            it         += junkSize;

            if (it == itEnd)
                return;
            //--------------------------------------------------------------------
            const size_t bytesWritten = tryWrite(&memBuf_[bufPos_], blockSize); //throw FileError; may return short! CONTRACT: bytesToWrite > 0
            bufPos_ += bytesWritten;
            if (notifyUnbufferedIO_) notifyUnbufferedIO_(bytesWritten); //throw X!
        }
    }

    AFS::FinalizeResult finalize() override //throw FileError, X
    {
        assert(bufPosEnd_ - bufPos_ <= getBlockSize());
        assert(bufPos_ <= bufPosEnd_ && bufPosEnd_ <= memBuf_.size());
        while (bufPos_ != bufPosEnd_)
        {
            const size_t bytesWritten = tryWrite(&memBuf_[bufPos_], bufPosEnd_ - bufPos_); //throw FileError; may return short! CONTRACT: bytesToWrite > 0
            bufPos_ += bytesWritten;
            if (notifyUnbufferedIO_) notifyUnbufferedIO_(bytesWritten); //throw X!
        }

        //~OutputStreamSftp() would call this one, too, but we want to propagate errors if any:
        close(); //throw FileError

        //it seems libssh2_sftp_fsetstat() triggers bugs on synology server => set mtime by path! https://freefilesync.org/forum/viewtopic.php?t=1281

        AFS::FinalizeResult result;
        //result.fileId = ... -> not supported by SFTP
        try
        {
            setModTimeIfAvailable(); //throw FileError, follows symlinks
            /* is setting modtime after closing the file handle a pessimization?
                SFTP:   no, needed for functional correctness (synology server), just as for Native */
        }
        catch (const FileError& e) { result.errorModTime = e; /*slicing?*/ }

        return result;
    }

private:
    size_t getBlockSize() const { return SFTP_OPTIMAL_BLOCK_SIZE_WRITE; } //non-zero block size is AFS contract!

    void close() //throw FileError
    {
        try
        {
            if (!fileHandle_)
                throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));
            ZEN_ON_SCOPE_EXIT(fileHandle_ = nullptr);

            session_->executeBlocking("libssh2_sftp_close", //throw SysError, FatalSshError
            [&](const SshSession::Details& sd) { return ::libssh2_sftp_close(fileHandle_); }); //noexcept!
        }
        catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
        catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => caller (will/should) stop using session
    }

    size_t tryWrite(const void* buffer, size_t bytesToWrite) //throw FileError; may return short! CONTRACT: bytesToWrite > 0
    {
        if (bytesToWrite == 0)
            throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));
        assert(bytesToWrite <= getBlockSize());

        ssize_t bytesWritten = 0;
        try
        {
            session_->executeBlocking("libssh2_sftp_write", //throw SysError, FatalSshError
                                      [&](const SshSession::Details& sd) //noexcept!
            {
                bytesWritten = ::libssh2_sftp_write(fileHandle_, static_cast<const char*>(buffer), bytesToWrite);
                return static_cast<int>(bytesWritten);
            });

            if (bytesWritten > static_cast<ssize_t>(bytesToWrite)) //better safe than sorry
                throw SysError(formatSystemError("libssh2_sftp_write", L"", L"Buffer overflow."));
        }
        catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
        catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => caller (will/should) stop using session

        //bytesWritten == 0 is no error according to doc!
        return bytesWritten;
    }

    void setModTimeIfAvailable() const //throw FileError, follows symlinks
    {
        assert(!fileHandle_);
        if (modTime_)
        {
            LIBSSH2_SFTP_ATTRIBUTES attribNew = {};
            attribNew.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
            attribNew.mtime = static_cast<decltype(attribNew.mtime)>(*modTime_);        //32-bit target! loss of data!
            attribNew.atime = static_cast<decltype(attribNew.atime)>(::time(nullptr));  //

            try
            {
                session_->executeBlocking("libssh2_sftp_setstat", //throw SysError, FatalSshError
                [&](const SshSession::Details& sd) { return ::libssh2_sftp_setstat(sd.sftpChannel, getLibssh2Path(filePath_), &attribNew); }); //noexcept!
            }
            catch (const SysError&      e) { throw FileError(replaceCpy(_("Cannot write modification time of %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
            catch (const FatalSshError& e) { throw FileError(replaceCpy(_("Cannot write modification time of %x."), L"%x", fmtPath(displayPath_)), e.toString()); } //SSH session corrupted! => caller (will/should) stop using session
        }
    }

    const AfsPath filePath_;
    const std::wstring displayPath_;
    LIBSSH2_SFTP_HANDLE* fileHandle_ = nullptr;
    const std::optional<time_t> modTime_;
    const IOCallback notifyUnbufferedIO_; //throw X
    std::shared_ptr<SftpSessionManager::SshSessionShared> session_;

    std::vector<std::byte> memBuf_ = std::vector<std::byte>(getBlockSize());
    size_t bufPos_    = 0; //buffered I/O see file_io.cpp
    size_t bufPosEnd_ = 0; //
};

//===========================================================================================================================

class SftpFileSystem : public AbstractFileSystem
{
public:
    SftpFileSystem(const SftpLogin& login) : login_(login) {}

    const SftpLogin& getLogin() const { return login_; }

    AfsPath getHomePath() const //throw FileError
    {
        try
        {
            //we never ever change the SFTP working directory, right? ...right?
            return getServerRealPath("."); //throw SysError
            //use "~" instead? NO: libssh2_sftp_realpath() fails with LIBSSH2_FX_NO_SUCH_FILE
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot determine final path for %x."), L"%x", fmtPath(getDisplayPath(AfsPath(Zstr("~"))))), e.toString()); }
    }

private:
    Zstring getInitPathPhrase(const AfsPath& afsPath) const override { return concatenateSftpFolderPathPhrase(login_, afsPath); }

    std::wstring getDisplayPath(const AfsPath& afsPath) const override { return getSftpDisplayPath(login_.server, afsPath); }

    bool isNullFileSystem() const override { return login_.server.empty(); }

    std::weak_ordering compareDeviceSameAfsType(const AbstractFileSystem& afsRhs) const override
    {
        const SftpLogin& lhs = login_;
        const SftpLogin& rhs = static_cast<const SftpFileSystem&>(afsRhs).login_;

        //exactly the type of case insensitive comparison we need for server names!
        if (const std::weak_ordering cmp = compareAsciiNoCase(lhs.server, rhs.server); //https://docs.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfow#IDNs
            std::is_neq(cmp))
            return cmp;

        //port does NOT create a *different* data source!!! -> same thing for password!

        //consider username: different users may have different views and folder access rights!

        return lhs.username <=> rhs.username; //case sensitive!
    }

    //----------------------------------------------------------------------------------------------------------------
    ItemType getItemType(const AfsPath& afsPath) const override //throw FileError
    {
        try
        {
            LIBSSH2_SFTP_ATTRIBUTES attr = {};
            runSftpCommand(login_, "libssh2_sftp_lstat", //throw SysError
            [&](const SshSession::Details& sd) { return ::libssh2_sftp_lstat(sd.sftpChannel, getLibssh2Path(afsPath), &attr); }); //noexcept!

            if ((attr.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) == 0)
                throw SysError(formatSystemError("libssh2_sftp_lstat", L"", L"File attributes not available."));

            if (LIBSSH2_SFTP_S_ISLNK(attr.permissions))
                return ItemType::symlink;
            if (LIBSSH2_SFTP_S_ISDIR(attr.permissions))
                return ItemType::folder;
            return ItemType::file; //LIBSSH2_SFTP_S_ISREG || LIBSSH2_SFTP_S_ISCHR || LIBSSH2_SFTP_S_ISBLK || LIBSSH2_SFTP_S_ISFIFO || LIBSSH2_SFTP_S_ISSOCK
        }
        catch (const SysError& e)
        {
            throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getDisplayPath(afsPath))), e.toString());
        }
    }

    std::optional<ItemType> itemStillExists(const AfsPath& afsPath) const override //throw FileError
    {
        //default implementation: folder traversal
        return AFS::itemStillExists(afsPath); //throw FileError
    }
    //----------------------------------------------------------------------------------------------------------------

    //already existing: fail
    void createFolderPlain(const AfsPath& afsPath) const override //throw FileError
    {
        try
        {
            //fails with obscure LIBSSH2_FX_FAILURE if already existing
            runSftpCommand(login_, "libssh2_sftp_mkdir", //throw SysError
                           [&](const SshSession::Details& sd) //noexcept!
            {
                return ::libssh2_sftp_mkdir(sd.sftpChannel, getLibssh2Path(afsPath), SFTP_DEFAULT_PERMISSION_FOLDER);
                //less explicit variant: return ::libssh2_sftp_mkdir(sd.sftpChannel, getLibssh2Path(afsPath), LIBSSH2_SFTP_DEFAULT_MODE);
            });
        }
        catch (const SysError& e) //libssh2_sftp_mkdir reports generic LIBSSH2_FX_FAILURE if existing
        {
            throw FileError(replaceCpy(_("Cannot create directory %x."), L"%x", fmtPath(getDisplayPath(afsPath))), e.toString());
        }
    }

    void removeFilePlain(const AfsPath& afsPath) const override //throw FileError
    {
        try
        {
            runSftpCommand(login_, "libssh2_sftp_unlink", //throw SysError
            [&](const SshSession::Details& sd) { return ::libssh2_sftp_unlink(sd.sftpChannel, getLibssh2Path(afsPath)); }); //noexcept!
        }
        catch (const SysError& e)
        {
            throw FileError(replaceCpy(_("Cannot delete file %x."), L"%x", fmtPath(getDisplayPath(afsPath))), e.toString());
        }
    }

    void removeSymlinkPlain(const AfsPath& afsPath) const override //throw FileError
    {
        this->removeFilePlain(afsPath); //throw FileError
    }

    void removeFolderPlain(const AfsPath& afsPath) const override //throw FileError
    {
        int delResult = LIBSSH2_ERROR_NONE;
        try
        {
            runSftpCommand(login_, "libssh2_sftp_rmdir", //throw SysError
            [&](const SshSession::Details& sd) { return delResult = ::libssh2_sftp_rmdir(sd.sftpChannel, getLibssh2Path(afsPath)); }); //noexcept!
        }
        catch (const SysError& e)
        {
            if (delResult < 0)
            {
                //tested: libssh2_sftp_rmdir will fail for symlinks!
                bool symlinkExists = false;
                try { symlinkExists = getItemType(afsPath) == ItemType::symlink; } /*throw FileError*/ catch (FileError&) {} //previous exception is more relevant

                if (symlinkExists)
                    return removeSymlinkPlain(afsPath); //throw FileError
            }

            throw FileError(replaceCpy(_("Cannot delete directory %x."), L"%x", fmtPath(getDisplayPath(afsPath))), e.toString());
        }
    }

    void removeFolderIfExistsRecursion(const AfsPath& afsPath, //throw FileError
                                       const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion /*throw X*/, //optional
                                       const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion) const override //one call for each object!
    {
        //default implementation: folder traversal
        AFS::removeFolderIfExistsRecursion(afsPath, onBeforeFileDeletion, onBeforeFolderDeletion); //throw FileError, X
    }

    //----------------------------------------------------------------------------------------------------------------
    AfsPath getServerRealPath(const std::string& sftpPath) const //throw SysError
    {
        const size_t bufSize = 10000;
        std::vector<char> buf(bufSize + 1); //ensure buffer is always null-terminated since we don't evaluate the byte count returned by libssh2_sftp_realpath()!

        runSftpCommand(login_, "libssh2_sftp_realpath", //throw SysError
        [&](const SshSession::Details& sd) { return ::libssh2_sftp_realpath(sd.sftpChannel, sftpPath, &buf[0], bufSize); }); //noexcept!

        const std::string sftpPathTrg = &buf[0];
        if (!startsWith(sftpPathTrg, '/'))
            throw SysError(replaceCpy<std::wstring>(L"Invalid path %x.", L"%x", fmtPath(utfTo<std::wstring>(sftpPathTrg))));

        return sanitizeDeviceRelativePath(utfTo<Zstring>(sftpPathTrg)); //code-reuse! but the sanitize part isn't really needed here...
    }

    AbstractPath getSymlinkResolvedPath(const AfsPath& afsPath) const override //throw FileError
    {
        try
        {
            const AfsPath afsPathTrg = getServerRealPath(getLibssh2Path(afsPath)); //throw SysError
            return AbstractPath(makeSharedRef<SftpFileSystem>(login_), afsPathTrg);
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot determine final path for %x."), L"%x", fmtPath(getDisplayPath(afsPath))), e.toString()); }
    }

    bool equalSymlinkContentForSameAfsType(const AfsPath& afsLhs, const AbstractPath& apRhs) const override //throw FileError
    {
        auto getTargetPath = [](const SftpFileSystem& sftpFs, const AfsPath& afsPath)
        {
            const unsigned int bufSize = 10000;
            std::string buf(bufSize + 1, '\0'); //ensure buffer is always null-terminated since we don't evaluate the byte count returned by libssh2_sftp_readlink()!
            try
            {
                runSftpCommand(sftpFs.login_, "libssh2_sftp_readlink", //throw SysError
                [&](const SshSession::Details& sd) { return ::libssh2_sftp_readlink(sd.sftpChannel, getLibssh2Path(afsPath), &buf[0], bufSize); }); //noexcept!
            }
            catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(sftpFs.getDisplayPath(afsPath))), e.toString()); }

            buf.resize(strLength(&buf[0]));
            return buf;
        };

        return getTargetPath(*this, afsLhs) == getTargetPath(static_cast<const SftpFileSystem&>(apRhs.afsDevice.ref()), apRhs.afsPath);
    }
    //----------------------------------------------------------------------------------------------------------------

    //return value always bound:
    std::unique_ptr<InputStream> getInputStream(const AfsPath& afsPath, const IOCallback& notifyUnbufferedIO /*throw X*/) const override //throw FileError, (ErrorFileLocked)
    {
        return std::make_unique<InputStreamSftp>(login_, afsPath, notifyUnbufferedIO); //throw FileError
    }

    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    //=> actual behavior: fail with obscure LIBSSH2_FX_FAILURE error
    std::unique_ptr<OutputStreamImpl> getOutputStream(const AfsPath& afsPath, //throw FileError
                                                      std::optional<uint64_t> streamSize,
                                                      std::optional<time_t> modTime,
                                                      const IOCallback& notifyUnbufferedIO /*throw X*/) const override
    {
        return std::make_unique<OutputStreamSftp>(login_, afsPath, modTime, notifyUnbufferedIO); //throw FileError
    }

    //----------------------------------------------------------------------------------------------------------------
    void traverseFolderRecursive(const TraverserWorkload& workload /*throw X*/, size_t parallelOps) const override
    {
        traverseFolderRecursiveSftp(login_, workload /*throw X*/, parallelOps); //throw X
    }
    //----------------------------------------------------------------------------------------------------------------

    //symlink handling: follow
    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    FileCopyResult copyFileForSameAfsType(const AfsPath& afsSource, const StreamAttributes& attrSource, //throw FileError, (ErrorFileLocked), X
                                          const AbstractPath& apTarget, bool copyFilePermissions, const IOCallback& notifyUnbufferedIO /*throw X*/) const override
    {
        //no native SFTP file copy => use stream-based file copy:
        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(AFS::getDisplayPath(apTarget))), _("Operation not supported by device."));

        //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
        return copyFileAsStream(afsSource, attrSource, apTarget, notifyUnbufferedIO); //throw FileError, (ErrorFileLocked), X
    }

    //symlink handling: follow
    //already existing: fail
    void copyNewFolderForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const override //throw FileError
    {
        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(AFS::getDisplayPath(apTarget))), _("Operation not supported by device."));

        //already existing: fail
        AFS::createFolderPlain(apTarget); //throw FileError
    }

    //already existing: fail
    void copySymlinkForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const override
    {
        throw FileError(replaceCpy(replaceCpy(_("Cannot copy symbolic link %x to %y."),
                                              L"%x", L'\n' + fmtPath(getDisplayPath(afsSource))),
                                   L"%y", L'\n' + fmtPath(AFS::getDisplayPath(apTarget))), _("Operation not supported by device."));
    }

    //already existing: undefined behavior! (e.g. fail/overwrite)
    //=> actual behavior: fail with obscure LIBSSH2_FX_FAILURE error
    void moveAndRenameItemForSameAfsType(const AfsPath& pathFrom, const AbstractPath& pathTo) const override //throw FileError, ErrorMoveUnsupported
    {
        auto generateErrorMsg = [&] { return replaceCpy(replaceCpy(_("Cannot move file %x to %y."),
                                                                   L"%x", L'\n' + fmtPath(getDisplayPath(pathFrom))),
                                                        L"%y", L'\n' + fmtPath(AFS::getDisplayPath(pathTo)));
                                    };

        if (std::is_neq(compareDeviceSameAfsType(pathTo.afsDevice.ref())))
            throw ErrorMoveUnsupported(generateErrorMsg(), _("Operation not supported between different devices."));

        try
        {
            runSftpCommand(login_, "libssh2_sftp_rename", //throw SysError
                           [&](const SshSession::Details& sd) //noexcept!
            {
                /* LIBSSH2_SFTP_RENAME_NATIVE:    "The server is free to do the rename operation in whatever way it chooses. Any other set flags are to be taken as hints to the server." No, thanks!
                   LIBSSH2_SFTP_RENAME_OVERWRITE: "No overwriting rename in [SFTP] v3/v4" http://www.greenend.org.uk/rjk/sftp/sftpversions.html

                   Test: LIBSSH2_SFTP_RENAME_OVERWRITE is not honored on freefilesync.org, no matter if LIBSSH2_SFTP_RENAME_NATIVE is set or not
                    => makes sense since SFTP v3 does not honor the additional flags that libssh2 sends!

                   "... the most widespread SFTP server implementation, the OpenSSH, will fail the SSH_FXP_RENAME request if the target file already exists"
                   => incidentally this is just the behavior we want!                              */
                const std::string sftpPathOld = getLibssh2Path(pathFrom);
                const std::string sftpPathNew = getLibssh2Path(pathTo.afsPath);

                return ::libssh2_sftp_rename(sd.sftpChannel, sftpPathOld, sftpPathNew, LIBSSH2_SFTP_RENAME_ATOMIC);
            });
        }
        catch (const SysError& e) //libssh2_sftp_rename_ex reports generic LIBSSH2_FX_FAILURE if target is already existing!
        {
            throw FileError(generateErrorMsg(), e.toString());
        }
    }

    bool supportsPermissions(const AfsPath& afsPath) const override { return false; } //throw FileError
    //wait until there is real demand for copying from and to SFTP with permissions => use stream-based file copy:

    //----------------------------------------------------------------------------------------------------------------
    FileIconHolder getFileIcon      (const AfsPath& afsPath, int pixelSize) const override { return {}; } //throw SysError; optional return value
    ImageHolder    getThumbnailImage(const AfsPath& afsPath, int pixelSize) const override { return {}; } //throw SysError; optional return value

    void authenticateAccess(bool allowUserInteraction) const override {} //throw FileError

    int getAccessTimeout() const override { return login_.timeoutSec; } //returns "0" if no timeout in force

    bool hasNativeTransactionalCopy() const override { return false; }
    //----------------------------------------------------------------------------------------------------------------

    int64_t getFreeDiskSpace(const AfsPath& afsPath) const override //throw FileError, returns < 0 if not available
    {
        //statvfs is an SFTP v3 extension and not supported by all server implementations
        //Mikrotik SFTP server fails with LIBSSH2_FX_OP_UNSUPPORTED and corrupts session so that next SFTP call will hang
        //(Server sends a duplicate SSH_FX_OP_UNSUPPORTED response with seemingly corrupt body and fails to respond from now on)
        //https://freefilesync.org/forum/viewtopic.php?t=618
        //Just discarding the current session is not enough in all cases, e.g. 1. Open SFTP file handle 2. statvfs fails 3. must close file handle
        return -1;
#if 0
        const std::string sftpPath = "/"; //::libssh2_sftp_statvfs will fail if path is not yet existing, OTOH root path should work, too?
        //NO, for correctness we must check free space for the given folder!!

        //"It is unspecified whether all members of the returned struct have meaningful values on all file systems."
        LIBSSH2_SFTP_STATVFS fsStats = {};
        try
        {
            runSftpCommand(login_, "libssh2_sftp_statvfs", //throw SysError
            [&](const SshSession::Details& sd) { return ::libssh2_sftp_statvfs(sd.sftpChannel, sftpPath.c_str(), sftpPath.size(), &fsStats); }); //noexcept!
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot determine free disk space for %x."), L"%x", fmtPath(getDisplayPath(L"/"))), e.toString()); }

        static_assert(sizeof(fsStats.f_bsize) >= 8);
        return fsStats.f_bsize * fsStats.f_bavail;
#endif
    }

    bool supportsRecycleBin(const AfsPath& afsPath) const override { return false; } //throw FileError

    std::unique_ptr<RecycleSession> createRecyclerSession(const AfsPath& afsPath) const override //throw FileError, return value must be bound!
    {
        assert(false); //see supportsRecycleBin()
        throw FileError(L"Recycle bin not supported by device.");
    }

    void recycleItemIfExists(const AfsPath& afsPath) const override //throw FileError
    {
        assert(false); //see supportsRecycleBin()
        throw FileError(replaceCpy(_("Unable to move %x to the recycle bin."), L"%x", fmtPath(getDisplayPath(afsPath))), _("Operation not supported by device."));
    }

    const SftpLogin login_;
};

//===========================================================================================================================

//expects "clean" login data
Zstring concatenateSftpFolderPathPhrase(const SftpLogin& login, const AfsPath& afsPath) //noexcept
{
    Zstring port;
    if (login.port > 0)
        port = Zstr(':') + numberTo<Zstring>(login.port);

    const SftpLogin loginDefault;

    Zstring options;
    if (login.timeoutSec != loginDefault.timeoutSec)
        options += Zstr("|timeout=") + numberTo<Zstring>(login.timeoutSec);

    if (login.traverserChannelsPerConnection != loginDefault.traverserChannelsPerConnection)
        options += Zstr("|chan=") + numberTo<Zstring>(login.traverserChannelsPerConnection);

    if (login.allowZlib)
        options += Zstr("|zlib");

    switch (login.authType)
    {
        case SftpAuthType::password:
            break;

        case SftpAuthType::keyFile:
            options += Zstr("|keyfile=") + login.privateKeyFilePath;
            break;

        case SftpAuthType::agent:
            options += Zstr("|agent");
            break;
    }

    if (login.authType != SftpAuthType::agent)
        if (!login.password.empty()) //password always last => visually truncated by folder input field
            options += Zstr("|pass64=") + encodePasswordBase64(login.password);

    return Zstring(sftpPrefix) + Zstr("//") + encodeFtpUsername(login.username) + Zstr("@") + login.server + port + getServerRelPath(afsPath) + options;
}
}


void fff::sftpInit()
{
    assert(!globalSftpSessionManager.get());
    globalSftpSessionManager.set(std::make_unique<SftpSessionManager>());
}


void fff::sftpTeardown()
{
    assert(globalSftpSessionManager.get());
    globalSftpSessionManager.set(nullptr);
}


AfsPath fff::getSftpHomePath(const SftpLogin& login) //throw FileError
{
    return SftpFileSystem(login).getHomePath(); //throw FileError
}


AfsDevice fff::condenseToSftpDevice(const SftpLogin& login) //noexcept
{
    //clean up input:
    SftpLogin loginTmp = login;
    trim(loginTmp.server);
    trim(loginTmp.username);
    trim(loginTmp.privateKeyFilePath);

    loginTmp.timeoutSec = std::max(1, loginTmp.timeoutSec);
    loginTmp.traverserChannelsPerConnection = std::max(1, loginTmp.traverserChannelsPerConnection);

    if (startsWithAsciiNoCase(loginTmp.server, "http:" ) ||
        startsWithAsciiNoCase(loginTmp.server, "https:") ||
        startsWithAsciiNoCase(loginTmp.server, "ftp:"  ) ||
        startsWithAsciiNoCase(loginTmp.server, "ftps:" ) ||
        startsWithAsciiNoCase(loginTmp.server, "sftp:" ))
        loginTmp.server = afterFirst(loginTmp.server, Zstr(':'), IfNotFoundReturn::none);
    trim(loginTmp.server, true, true, [](Zchar c) { return c == Zstr('/') || c == Zstr('\\'); });

    return makeSharedRef<SftpFileSystem>(loginTmp);
}


SftpLogin fff::extractSftpLogin(const AfsDevice& afsDevice) //noexcept
{
    if (const auto sftpDevice = dynamic_cast<const SftpFileSystem*>(&afsDevice.ref()))
        return sftpDevice->getLogin();

    assert(false);
    return {};
}


int fff::getServerMaxChannelsPerConnection(const SftpLogin& login) //throw FileError
{
    try
    {
        const auto timeoutTime = std::chrono::steady_clock::now() + SFTP_CHANNEL_LIMIT_DETECTION_TIME_OUT;

        std::unique_ptr<SftpSessionManager::SshSessionExclusive> exSession = getExclusiveSftpSession(login); //throw SysError

        ZEN_ON_SCOPE_EXIT(exSession->markAsCorrupted()); //after hitting the server limits, the session might have gone bananas (e.g. server fails on all requests)

        for (;;)
        {
            try
            {
                SftpSessionManager::SshSessionExclusive::addSftpChannel({ exSession.get() }); //throw SysError, FatalSshError
            }
            catch (const SysError&       ) { if (exSession->getSftpChannelCount() == 0) throw;                        return static_cast<int>(exSession->getSftpChannelCount()); }
            catch (const FatalSshError& e) { if (exSession->getSftpChannelCount() == 0) throw SysError(e.toString()); return static_cast<int>(exSession->getSftpChannelCount()); }

            if (std::chrono::steady_clock::now() > timeoutTime)
                throw SysError(_P("Operation timed out after 1 second.", "Operation timed out after %x seconds.",
                                  std::chrono::seconds(SFTP_CHANNEL_LIMIT_DETECTION_TIME_OUT).count()) + L' ' +
                               replaceCpy(_("Failed to open SFTP channel number %x."), L"%x", numberTo<std::wstring>(exSession->getSftpChannelCount() + 1)));
        }
    }
    catch (const SysError& e)
    {
        throw FileError(replaceCpy(_("Unable to connect to %x."), L"%x", fmtPath(login.server)), e.toString());
    }
}


bool fff::acceptsItemPathPhraseSftp(const Zstring& itemPathPhrase) //noexcept
{
    Zstring path = expandMacros(itemPathPhrase); //expand before trimming!
    trim(path);
    return startsWithAsciiNoCase(path, sftpPrefix); //check for explicit SFTP path
}


//syntax: sftp://[<user>[:<password>]@]<server>[:port]/<relative-path>[|option_name=value]
//
//   e.g. sftp://user001:secretpassword@private.example.com:222/mydirectory/
//        sftp://user001@private.example.com/mydirectory|con=2|cpc=10|keyfile=%AppData%\id_rsa|pass64=c2VjcmV0cGFzc3dvcmQ
AbstractPath fff::createItemPathSftp(const Zstring& itemPathPhrase) //noexcept
{
    Zstring pathPhrase = expandMacros(itemPathPhrase); //expand before trimming!
    trim(pathPhrase);

    if (startsWithAsciiNoCase(pathPhrase, sftpPrefix))
        pathPhrase = pathPhrase.c_str() + strLength(sftpPrefix);
    trim(pathPhrase, true, false, [](Zchar c) { return c == Zstr('/') || c == Zstr('\\'); });

    const Zstring credentials = beforeFirst(pathPhrase, Zstr('@'), IfNotFoundReturn::none);
    const Zstring fullPathOpt =  afterFirst(pathPhrase, Zstr('@'), IfNotFoundReturn::all);

    SftpLogin login;
    login.username = decodeFtpUsername(beforeFirst(credentials, Zstr(':'), IfNotFoundReturn::all)); //support standard FTP syntax, even though ':'
    login.password =                    afterFirst(credentials, Zstr(':'), IfNotFoundReturn::none); //is not used by our concatenateSftpFolderPathPhrase()!

    const Zstring fullPath = beforeFirst(fullPathOpt, Zstr('|'), IfNotFoundReturn::all);
    const Zstring options  =  afterFirst(fullPathOpt, Zstr('|'), IfNotFoundReturn::none);

    auto it = std::find_if(fullPath.begin(), fullPath.end(), [](Zchar c) { return c == '/' || c == '\\'; });
    const Zstring serverPort(fullPath.begin(), it);
    const AfsPath serverRelPath = sanitizeDeviceRelativePath({ it, fullPath.end() });

    login.server       = beforeLast(serverPort, Zstr(':'), IfNotFoundReturn::all);
    const Zstring port =  afterLast(serverPort, Zstr(':'), IfNotFoundReturn::none);
    login.port = stringTo<int>(port); //0 if empty

    assert(login.allowZlib == false);

    if (!options.empty())
    {
        for (const Zstring& optPhrase : split(options, Zstr("|"), SplitOnEmpty::skip))
            if (startsWith(optPhrase, Zstr("timeout=")))
                login.timeoutSec = stringTo<int>(afterFirst(optPhrase, Zstr("="), IfNotFoundReturn::none));
            else if (startsWith(optPhrase, Zstr("chan=")))
                login.traverserChannelsPerConnection = stringTo<int>(afterFirst(optPhrase, Zstr("="), IfNotFoundReturn::none));
            else if (startsWith(optPhrase, Zstr("keyfile=")))
            {
                login.authType = SftpAuthType::keyFile;
                login.privateKeyFilePath = afterFirst(optPhrase, Zstr("="), IfNotFoundReturn::none);
            }
            else if (optPhrase == Zstr("agent"))
                login.authType = SftpAuthType::agent;
            else if (startsWith(optPhrase, Zstr("pass64=")))
                login.password = decodePasswordBase64(afterFirst(optPhrase, Zstr("="), IfNotFoundReturn::none));
            else if (optPhrase == Zstr("zlib"))
                login.allowZlib = true;
            else
                assert(false);
    } //fix "-Wdangling-else"

    return AbstractPath(makeSharedRef<SftpFileSystem>(login), serverRelPath);
}
