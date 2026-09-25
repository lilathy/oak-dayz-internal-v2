#include "protection_client.h"
#include "runtime_offsets.h"
#include "../../../packages/protect/include/oak_str.hpp"
#include "../../../packages/protect/include/oak_runtime_pin.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
    // Multi-token authorization: patching a single global is no longer enough.
    volatile LONG g_AuthPrimary = 0;
    volatile LONG g_AuthSecondary = 0;
    volatile LONG g_AuthIntegrity = 0;
    volatile LONG g_Stop = 0;
    HANDLE g_RenewThread = NULL;
    std::string g_ApiUrl;
    std::string g_Slug;
    std::string g_ClientReleaseId;
    std::string g_HardwareId;
    std::string g_ClientNonce;
    std::string g_LeaseToken;
    std::string g_RenewChallenge;
    std::string g_CachedPackageSha;
    std::string g_CachedPublicPem;
    std::string g_CachedKeyId;
    std::uint32_t g_ExpectedIntegrity = 0;

    void ClearSecrets()
    {
        g_LeaseToken.assign(g_LeaseToken.size(), '\0');
        g_LeaseToken.clear();
        g_RenewChallenge.assign(g_RenewChallenge.size(), '\0');
        g_RenewChallenge.clear();
        g_ClientNonce.assign(g_ClientNonce.size(), '\0');
        g_ClientNonce.clear();
        g_CachedPublicPem.clear();
        g_CachedKeyId.clear();
        g_CachedPackageSha.clear();
    }

    void WriteAuthMark(const char* why)
    {
        if (!why) return;
        OAK_ENC_STR(flagDayz, "C:\\oak\\dayz\\protect_auth.flag", 0x2E);
        OAK_ENC_STR(flagLegacy, "C:\\oak\\protect_auth.flag", 0x2F);
        const char* paths[2] = { flagDayz, flagLegacy };
        for (int i = 0; i < 2; i++)
        {
            HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) continue;
            DWORD w = 0;
            WriteFile(hf, why, (DWORD)lstrlenA(why), &w, NULL);
            CloseHandle(hf);
        }
        SecureZeroMemory(flagDayz, sizeof(flagDayz));
        SecureZeroMemory(flagLegacy, sizeof(flagLegacy));
    }

    void SetAuthorized(bool ok)
    {
        if (!ok)
        {
            InterlockedExchange(&g_AuthPrimary, 0);
            InterlockedExchange(&g_AuthSecondary, 0);
            InterlockedExchange(&g_AuthIntegrity, 0);
            ClearSecrets();
            return;
        }
        const LONG tag = static_cast<LONG>(g_ExpectedIntegrity ? g_ExpectedIntegrity : OakProtectIntegrityTag());
        InterlockedExchange(&g_AuthPrimary, 0x6F416B31); // 'oAk1'
        InterlockedExchange(&g_AuthSecondary, 0x6F416B31 ^ tag);
        InterlockedExchange(&g_AuthIntegrity, tag);
    }

    bool AuthorizedUnlocked()
    {
        const LONG primary = InterlockedCompareExchange(&g_AuthPrimary, 0, 0);
        const LONG secondary = InterlockedCompareExchange(&g_AuthSecondary, 0, 0);
        const LONG integrity = InterlockedCompareExchange(&g_AuthIntegrity, 0, 0);
        if (!oak_protect::OpaqueTrue(static_cast<std::uint32_t>(primary ^ secondary)))
            return false;
        if (oak_protect::OpaqueFalse(static_cast<std::uint32_t>(primary)))
            return false;
        return primary == 0x6F416B31 &&
               secondary == (0x6F416B31 ^ integrity) &&
               integrity != 0 &&
               static_cast<std::uint32_t>(integrity) == OakProtectIntegrityTag();
    }

    std::string LeaseEndpoint(const std::string& slug)
    {
        OAK_ENC_STR(prefix, "/v1/products/", 0x3C);
        OAK_ENC_STR(suffix, "/lease", 0x91);
        return std::string(prefix) + slug + suffix;
    }

    std::string RenewEndpoint(const std::string& slug)
    {
        OAK_ENC_STR(suffix, "/renew", 0xA7);
        return LeaseEndpoint(slug) + suffix;
    }

    std::string ModuleBootstrapPath()
    {
        // Manual-map: GetModuleFileNameA on the mapped image usually fails or
        // returns the host EXE. Launcher stages oak_payload.dll under C:\oak\dayz\
        // (preferred) and writes .oak-bootstrap / .oak-lease beside it.
        static const char* kStagedCandidates[] = {
            "C:\\oak\\dayz\\oak_payload.dll.oak-bootstrap",
            "C:\\oak\\dayz\\oak_payload.dll.oak-bootstrap.tmp",
            "C:\\oak\\dayz\\dayz_internal.dll.oak-bootstrap",
            "C:\\oak\\dayz\\dayz_internal.dll.oak-bootstrap.tmp",
            "C:\\oak\\dayz_internal.dll.oak-bootstrap",
            "C:\\oak\\dayz_internal.dll.oak-bootstrap.tmp",
        };
        for (const char* candidate : kStagedCandidates)
        {
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
                return candidate;
        }

        HMODULE module = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&OakProtectionAuthorize),
                                &module) || !module)
            return "";
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(module, path, ARRAYSIZE(path)) || !path[0])
            return "";
        // Ignore host process path (DayZ_x64.exe) — handoff is never next to it.
        if (strstr(path, "DayZ") != NULL && strstr(path, "dayz_internal") == NULL
            && strstr(path, "oak_payload") == NULL)
            return "";
        return std::string(path) + ".oak-bootstrap";
    }

    std::string ModuleLeasePath()
    {
        static const char* kStagedCandidates[] = {
            "C:\\oak\\dayz\\oak_payload.dll.oak-lease",
            "C:\\oak\\dayz\\dayz_internal.dll.oak-lease",
            "C:\\oak\\dayz_internal.dll.oak-lease",
        };
        for (const char* candidate : kStagedCandidates)
        {
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
                return candidate;
        }
        const std::string boot = ModuleBootstrapPath();
        if (boot.size() > 14) // ".oak-bootstrap"
            return boot.substr(0, boot.size() - 14) + ".oak-lease";
        return "";
    }

    std::string ReadFileText(const std::string& path)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return "";
        DWORD size = GetFileSize(file, NULL);
        if (size == INVALID_FILE_SIZE || size == 0 || size > 16 * 1024)
        {
            CloseHandle(file);
            return "";
        }
        std::string out(size, '\0');
        DWORD read = 0;
        BOOL ok = ReadFile(file, &out[0], size, &read, NULL);
        CloseHandle(file);
        if (!ok || read != size) return "";
        return out;
    }

    std::string JsonString(const std::string& json, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        // Walk to unescaped closing quote (base64 / PEM may contain \u002B etc.)
        size_t end = pos + 1;
        while (end < json.size())
        {
            if (json[end] == '\\')
            {
                end += (end + 1 < json.size()) ? 2 : 1;
                continue;
            }
            if (json[end] == '"') break;
            ++end;
        }
        if (end >= json.size() || json[end] != '"') return "";
        std::string value = json.substr(pos + 1, end - pos - 1);
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] != '\\' || i + 1 >= value.size())
            {
                out.push_back(value[i]);
                continue;
            }
            const char n = value[i + 1];
            if (n == 'n') { out.push_back('\n'); i++; }
            else if (n == 'r') { out.push_back('\r'); i++; }
            else if (n == 't') { out.push_back('\t'); i++; }
            else if (n == '\\' || n == '"' || n == '/') { out.push_back(n); i++; }
            else if (n == 'u' && i + 5 < value.size())
            {
                // \uXXXX — used by System.Text.Json for '+' in base64 (\u002B)
                unsigned code = 0;
                bool ok = true;
                for (int k = 0; k < 4; k++)
                {
                    char c = value[i + 2 + k];
                    code <<= 4;
                    if (c >= '0' && c <= '9') code |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') code |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') code |= (unsigned)(c - 'A' + 10);
                    else { ok = false; break; }
                }
                if (ok && code <= 0xFF)
                {
                    out.push_back((char)code);
                    i += 5;
                }
                else
                    out.push_back(value[i]);
            }
            else
                out.push_back(value[i]);
        }
        return out;
    }

    bool IsSafeValue(const std::string& value, size_t maximum)
    {
        if (value.empty() || value.size() > maximum) return false;
        for (unsigned char c : value)
        {
            // Allow printable ASCII used by tokens / base64 / URLs.
            if (c < 0x21 || c == '"' || c == '\\') return false;
        }
        return true;
    }

    // PEM may include newlines after JSON unescape — not checked via IsSafeValue.
    bool IsSafePem(const std::string& value)
    {
        if (value.size() < 32 || value.size() > 8 * 1024) return false;
        return value.find("BEGIN PUBLIC KEY") != std::string::npos;
    }

    std::string RandomNonce()
    {
        BYTE bytes[24] = {};
        if (BCryptGenRandom(NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return "";
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        out.reserve(sizeof(bytes));
        for (BYTE b : bytes) out.push_back(alphabet[b & 63]);
        return out;
    }

    bool IsLoopbackHost(const std::wstring& host)
    {
        if (host.empty()) return false;
        std::wstring lower = host;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        return lower == L"127.0.0.1" || lower == L"localhost" || lower == L"::1";
    }

    bool CrackUrl(const std::string& base, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& secure)
    {
        std::wstring url(base.begin(), base.end());
        URL_COMPONENTS parts = {};
        wchar_t hostBuf[256] = {};
        wchar_t pathBuf[2048] = {};
        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = hostBuf;
        parts.dwHostNameLength = ARRAYSIZE(hostBuf);
        parts.lpszUrlPath = pathBuf;
        parts.dwUrlPathLength = ARRAYSIZE(pathBuf);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
            return false;
#if defined(OAK_REQUIRE_PROTECTION)
        // Release: HTTPS only, except loopback for local smoke testing.
        if (parts.nScheme == INTERNET_SCHEME_HTTP)
        {
            std::wstring h(parts.lpszHostName, parts.dwHostNameLength);
            if (!IsLoopbackHost(h)) return false;
        }
        else if (parts.nScheme != INTERNET_SCHEME_HTTPS)
        {
            return false;
        }
#else
        if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
            return false;
#endif
        host.assign(parts.lpszHostName, parts.dwHostNameLength);
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (path.empty()) path = L"/";
        port = parts.nPort;
        secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        return !host.empty();
    }

    bool RequestJson(const std::string& base, const std::string& endpoint,
                     const wchar_t* method, const std::string& body,
                     std::string& response)
    {
        std::wstring host, basePath;
        INTERNET_PORT port = 0;
        bool secure = false;
        if (!CrackUrl(base, host, basePath, port, secure)) return false;
        std::wstring endpointW(endpoint.begin(), endpoint.end());
        std::wstring object = basePath;
        if (object.back() == L'/' && !endpointW.empty() && endpointW.front() == L'/')
            object.pop_back();
        object += endpointW;

        HINTERNET session = WinHttpOpen(
            L"OakClient/1.0",
            IsLoopbackHost(host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);
        HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return false;
        }
        HINTERNET request = WinHttpOpenRequest(connect, method, object.c_str(), NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json";
        LPVOID requestBody = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
        BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                                     requestBody, static_cast<DWORD>(body.size()),
                                     static_cast<DWORD>(body.size()), 0) &&
                  WinHttpReceiveResponse(request, NULL);
        DWORD status = 0, statusSize = sizeof(status);
        if (ok)
            ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                     WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
        while (ok)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) { ok = FALSE; break; }
            if (!available) break;
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) { ok = FALSE; break; }
            response.append(chunk.data(), read);
            if (response.size() > 128 * 1024) { ok = FALSE; break; }
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    bool PostJson(const std::string& base, const std::string& endpoint,
                  const std::string& body, std::string& response)
    {
        return RequestJson(base, endpoint, L"POST", body, response);
    }

    bool GetJson(const std::string& base, const std::string& endpoint, std::string& response)
    {
        return RequestJson(base, endpoint, L"GET", "", response);
    }

    bool Base64Decode(const std::string& encoded, std::vector<BYTE>& decoded)
    {
        DWORD size = 0;
        if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                                  CRYPT_STRING_BASE64, NULL, &size, NULL, NULL) || !size)
            return false;
        decoded.resize(size);
        return CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                                    CRYPT_STRING_BASE64, decoded.data(), &size, NULL, NULL) != FALSE;
    }

    bool Sha256(const BYTE* data, DWORD size, BYTE hash[32])
    {
        BCRYPT_ALG_HANDLE algorithm = NULL;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
            return false;
        const NTSTATUS status = BCryptHash(algorithm, NULL, 0,
                                           const_cast<PUCHAR>(data), size,
                                           hash, 32);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return status >= 0;
    }

    bool HmacSha256(const BYTE* key, DWORD keySize, const BYTE* data, DWORD dataSize, BYTE out[32])
    {
        BCRYPT_ALG_HANDLE algorithm = NULL;
        BCRYPT_HASH_HANDLE hash = NULL;
        DWORD objectSize = 0, dataLen = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
            return false;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                              sizeof(objectSize), &dataLen, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        std::vector<BYTE> object(objectSize);
        bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                                   const_cast<PUCHAR>(key), keySize, 0) >= 0 &&
                  BCryptHashData(hash, const_cast<PUCHAR>(data), dataSize, 0) >= 0 &&
                  BCryptFinishHash(hash, out, 32, 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return ok;
    }

    std::string HexLower(const BYTE* data, size_t size)
    {
        static const char chars[] = "0123456789abcdef";
        std::string out(size * 2, '0');
        for (size_t i = 0; i < size; ++i)
        {
            out[i * 2] = chars[data[i] >> 4];
            out[i * 2 + 1] = chars[data[i] & 15];
        }
        return out;
    }

    std::string ChallengeResponse(const std::string& challenge)
    {
        BYTE keyMaterial[32] = {};
        const std::string keyInput = g_LeaseToken + "|" + g_ClientNonce;
        if (!Sha256(reinterpret_cast<const BYTE*>(keyInput.data()),
                    static_cast<DWORD>(keyInput.size()), keyMaterial))
            return "";
        BYTE mac[32] = {};
        if (!HmacSha256(keyMaterial, sizeof(keyMaterial),
                        reinterpret_cast<const BYTE*>(challenge.data()),
                        static_cast<DWORD>(challenge.size()), mac))
            return "";
        return HexLower(mac, sizeof(mac));
    }

    bool PackageCompatible(const std::string& payload)
    {
        if (payload.find("\"schemaVersion\":1") == std::string::npos) return false;
        if (JsonString(payload, "productSlug") != g_Slug) return false;
        if (JsonString(payload, "clientReleaseId") != g_ClientReleaseId) return false;
        if (payload.find("\"enabled\":false") != std::string::npos) return false;
        if (payload.find("\"enabled\":true") == std::string::npos) return false;

        const std::string expires = JsonString(payload, "expiresAt");
        SYSTEMTIME expiry = {};
        if (expires.size() < 19 ||
            sscanf_s(expires.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
                     &expiry.wYear, &expiry.wMonth, &expiry.wDay,
                     &expiry.wHour, &expiry.wMinute, &expiry.wSecond) != 6)
            return false;
        FILETIME expiryFile = {}, nowFile = {};
        if (!SystemTimeToFileTime(&expiry, &expiryFile)) return false;
        GetSystemTimeAsFileTime(&nowFile);
        ULARGE_INTEGER e = {}, n = {};
        e.LowPart = expiryFile.dwLowDateTime; e.HighPart = expiryFile.dwHighDateTime;
        n.LowPart = nowFile.dwLowDateTime; n.HighPart = nowFile.dwHighDateTime;
        return e.QuadPart > n.QuadPart;
    }

    bool VerifyRuntimePackage(const std::string& leaseResponse, bool applyOffsets)
    {
        auto failMark = [](const char* why) { WriteAuthMark(why); };

        const std::string packageSha = JsonString(leaseResponse, "sha256");
        if (!packageSha.empty() && packageSha == g_CachedPackageSha && !applyOffsets)
            return true;

        const std::string payload64 = JsonString(leaseResponse, "payloadBase64");
        const std::string signature64 = JsonString(leaseResponse, "signature");
        const std::string expectedKeyId = JsonString(leaseResponse, "signingKeyId");
        if (!IsSafeValue(payload64, 128 * 1024) ||
            !IsSafeValue(signature64, 512) ||
            !IsSafeValue(expectedKeyId, 128))
        {
            failMark("verify_fields");
            return false;
        }

        std::string pem = JsonString(leaseResponse, "publicKey");
        if (pem.empty() || expectedKeyId != g_CachedKeyId || pem != g_CachedPublicPem)
        {
            if (pem.empty())
            {
                std::string keyResponse;
                OAK_ENC_STR(keyPath, "/v1/products/", 0x55);
                OAK_ENC_STR(keyTail, "/runtime-package-key", 0x66);
                if (!GetJson(g_ApiUrl, std::string(keyPath) + g_Slug + keyTail, keyResponse))
                {
                    failMark("verify_key_http");
                    return false;
                }
                if (JsonString(keyResponse, "algorithm") != "ecdsa-p256-sha256" ||
                    JsonString(keyResponse, "keyId") != expectedKeyId)
                {
                    failMark("verify_key_meta");
                    return false;
                }
                pem = JsonString(keyResponse, "publicKey");
            }
            if (pem.empty() || !IsSafePem(pem))
            {
                failMark("verify_pem");
                return false;
            }
            g_CachedPublicPem = pem;
            g_CachedKeyId = expectedKeyId;
        }
        else
        {
            pem = g_CachedPublicPem;
        }

        std::vector<BYTE> der, payload, signature;
        DWORD derSize = 0;
        if (!CryptStringToBinaryA(pem.c_str(), static_cast<DWORD>(pem.size()),
                                  CRYPT_STRING_BASE64HEADER, NULL, &derSize, NULL, NULL) ||
            !derSize)
        {
            failMark("verify_pem_der");
            return false;
        }
        der.resize(derSize);
        if (!CryptStringToBinaryA(pem.c_str(), static_cast<DWORD>(pem.size()),
                                  CRYPT_STRING_BASE64HEADER, der.data(), &derSize, NULL, NULL))
        {
            failMark("verify_pem_der");
            return false;
        }
        BYTE keyHash[32] = {};
        if (!Sha256(der.data(), static_cast<DWORD>(der.size()), keyHash) ||
            HexLower(keyHash, sizeof(keyHash)) != expectedKeyId)
        {
            failMark("verify_keyid");
            return false;
        }
#if defined(OAK_REQUIRE_PROTECTION)
        // When a pin is baked in, refuse any server-supplied key that differs.
        {
            const char* pin = OAK_RUNTIME_SPKI_PIN;
            if (pin && pin[0] != '\0')
            {
                const std::string actual = HexLower(keyHash, sizeof(keyHash));
                if (actual != pin)
                {
                    failMark("verify_pin");
                    return false;
                }
            }
        }
#endif
        if (!Base64Decode(payload64, payload) || !Base64Decode(signature64, signature) ||
            signature.size() != 64 || payload.empty())
        {
            failMark("verify_b64");
            return false;
        }

        CERT_PUBLIC_KEY_INFO* publicInfo = NULL;
        DWORD infoSize = 0;
        if (!CryptDecodeObjectEx(
                X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                der.data(), static_cast<DWORD>(der.size()),
                CRYPT_DECODE_ALLOC_FLAG, NULL, &publicInfo, &infoSize))
        {
            failMark("verify_import");
            return false;
        }
        BCRYPT_KEY_HANDLE key = NULL;
        const BOOL imported = CryptImportPublicKeyInfoEx2(
            X509_ASN_ENCODING, publicInfo, 0, NULL, &key);
        LocalFree(publicInfo);
        if (!imported || !key)
        {
            failMark("verify_import");
            return false;
        }

        BYTE payloadHash[32] = {};
        const bool hashed = Sha256(payload.data(), static_cast<DWORD>(payload.size()), payloadHash);
        const NTSTATUS sigStatus = hashed
            ? BCryptVerifySignature(key, NULL, payloadHash, sizeof(payloadHash),
                                    signature.data(), static_cast<ULONG>(signature.size()), 0)
            : (NTSTATUS)0xC0000001;
        BCryptDestroyKey(key);
        if (!hashed || sigStatus < 0)
        {
            failMark("verify_sig");
            return false;
        }
        const std::string packagePayload(payload.begin(), payload.end());
        if (!PackageCompatible(packagePayload))
        {
            failMark("verify_compat");
            return false;
        }
        if (!OakApplyRuntimeOffsets(packagePayload, applyOffsets))
        {
            failMark("verify_offsets");
            return false;
        }
        g_CachedPackageSha = packageSha;
        return true;
    }

    bool AcquireLease(const std::string& ticket)
    {
        WriteAuthMark("lease_post");
        const std::string body = "{\"bootstrapTicket\":\"" + ticket +
                                 "\",\"hwid\":\"" + g_HardwareId +
                                 "\",\"clientNonce\":\"" + g_ClientNonce + "\"}";
        std::string response;
        if (!PostJson(g_ApiUrl, LeaseEndpoint(g_Slug), body, response))
        {
            WriteAuthMark("lease_http_fail");
            return false;
        }
        WriteAuthMark("lease_http_ok");
        g_LeaseToken = JsonString(response, "leaseToken");
        g_RenewChallenge = JsonString(response, "renewChallenge");
        if (!IsSafeValue(g_LeaseToken, 512) || !IsSafeValue(g_RenewChallenge, 128))
            return false;
        WriteAuthMark("lease_verify");
        return VerifyRuntimePackage(response, true);
    }

    bool RenewLease()
    {
        if (!AuthorizedUnlocked()) return false;
        const std::string proof = ChallengeResponse(g_RenewChallenge);
        if (proof.empty()) return false;
        const std::string body = "{\"leaseToken\":\"" + g_LeaseToken +
                                 "\",\"clientNonce\":\"" + g_ClientNonce +
                                 "\",\"challengeResponse\":\"" + proof + "\"}";
        std::string response;
        if (!PostJson(g_ApiUrl, RenewEndpoint(g_Slug), body, response))
            return false;
        const std::string nextChallenge = JsonString(response, "renewChallenge");
        if (!IsSafeValue(nextChallenge, 128)) return false;
        g_RenewChallenge = nextChallenge;
        const std::string nextSha = JsonString(response, "sha256");
        if (!nextSha.empty() && nextSha == g_CachedPackageSha)
            return true;
        return VerifyRuntimePackage(response, true);
    }

    DWORD WINAPI RenewThreadProc(LPVOID)
    {
        int missed = 0;
        while (InterlockedCompareExchange(&g_Stop, 0, 0) == 0)
        {
            for (int i = 0; i < 90 && InterlockedCompareExchange(&g_Stop, 0, 0) == 0; ++i)
                Sleep(1000);
            if (InterlockedCompareExchange(&g_Stop, 0, 0) != 0) break;
            if (RenewLease())
            {
                missed = 0;
                continue;
            }
            if (++missed >= 3)
                SetAuthorized(false);
        }
        return 0;
    }
}

bool OakProtectionAuthorize()
{
#if !defined(OAK_REQUIRE_PROTECTION)
    g_ExpectedIntegrity = OakProtectIntegrityTag();
    SetAuthorized(true);
    return true;
#else
    auto mark = [](const char* why) { WriteAuthMark(why); };
    auto fail = [&](const char* why) {
        mark(why);
        return false;
    };

    mark("start");
    g_ExpectedIntegrity = OakProtectIntegrityTag();
    if (!oak_protect::OpaqueTrue(g_ExpectedIntegrity))
        return fail("integrity_tag");

    // Refuse leftover unlock markers from Debug launchers — Release never honors them.
    {
        OAK_ENC_STR(unlockPath, "C:\\oak\\dayz\\oak_dev_unlock", 0x5A);
        DeleteFileA(unlockPath);
        SecureZeroMemory(unlockPath, sizeof(unlockPath));
        {
            OAK_ENC_STR(legacyUnlock, "C:\\oak\\oak_dev_unlock", 0x5B);
            DeleteFileA(legacyUnlock);
            SecureZeroMemory(legacyUnlock, sizeof(legacyUnlock));
        }
    }

    const std::string handoffPath = ModuleBootstrapPath();
    if (handoffPath.empty())
        return fail("handoff_path_empty");
    mark("path_ok");
    const std::string handoff = ReadFileText(handoffPath);
    if (handoff.empty())
        return fail("handoff_unreadable");
    mark("handoff_read");

    g_ApiUrl = JsonString(handoff, "ApiUrl");
    g_Slug = JsonString(handoff, "ProductSlug");
    g_ClientReleaseId = JsonString(handoff, "ClientReleaseId");
    const std::string ticket = JsonString(handoff, "BootstrapTicket");
    g_HardwareId = JsonString(handoff, "HardwareId");
    g_ClientNonce = JsonString(handoff, "ClientNonce");
    if (g_ClientNonce.empty())
        g_ClientNonce = RandomNonce();

    if (!IsSafeValue(g_ApiUrl, 512) || !IsSafeValue(g_Slug, 64) ||
        !IsSafeValue(g_ClientReleaseId, 64) ||
        !IsSafeValue(g_HardwareId, 256) ||
        !IsSafeValue(g_ClientNonce, 128))
        return fail("handoff_fields_invalid");
    if (g_Slug != OAK_PRODUCT_SLUG) return fail("slug_mismatch");

    // Prefer launcher-prefetched lease — never WinHttp from inside DayZ.
    const std::string leasePath = ModuleLeasePath();
    const std::string prefetched = leasePath.empty() ? std::string() : ReadFileText(leasePath);
    if (!prefetched.empty())
    {
        mark("lease_prefetch");
        g_LeaseToken = JsonString(prefetched, "leaseToken");
        g_RenewChallenge = JsonString(prefetched, "renewChallenge");
        if (!IsSafeValue(g_LeaseToken, 512) || !IsSafeValue(g_RenewChallenge, 128))
            return fail("prefetch_fields_invalid");
        if (!VerifyRuntimePackage(prefetched, true))
            return false; // VerifyRuntimePackage already wrote the specific fail mark
        DeleteFileA(leasePath.c_str());
        DeleteFileA(handoffPath.c_str());
        mark("lease_ok");
    }
    else
    {
        // Legacy fallback (unsafe under BE — kept for older launchers).
        if (!IsSafeValue(ticket, 512))
            return fail("handoff_fields_invalid");
        mark("lease_begin");
        if (!AcquireLease(ticket)) return fail("lease_failed");
        DeleteFileA(handoffPath.c_str());
        mark("lease_ok");
    }

    SetAuthorized(true);
    // Skip in-process renew for now — WinHttp from DayZ trips BattlEye.
    // Lease lifetime is long enough for a session; launcher can re-issue next launch.
    if (!AuthorizedUnlocked()) return fail("auth_tokens_inconsistent");
    mark("authorized");
    return true;
#endif
}

bool OakProtectionIsAuthorized()
{
    return AuthorizedUnlocked();
}

void OakProtectionShutdown()
{
    InterlockedExchange(&g_Stop, 1);
    if (g_RenewThread)
    {
        CloseHandle(g_RenewThread);
        g_RenewThread = NULL;
    }
    SetAuthorized(false);
}
