#include "android_http.hpp"

#include "core/log.hpp"
#include "core/safe.hpp"

#include <mod/amlmod.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace usa1to1 {
namespace net {

#if defined(__ANDROID__)

namespace {

// A JNIEnv/JavaVM is a pointer to a struct whose first member is a pointer to a
// table of function pointers. Both are validated before any call goes through
// them: a loader that does not implement GetJavaVM hands back 0x7, not a VM.
bool IsUsableInterface(const void *iface) {
    if (!core::IsMappedRange(iface, sizeof(void *)))
        return false;
    void *const *table = *reinterpret_cast<void *const *const *>(iface);
    if (!core::IsSanePointer(table) || !core::IsMappedRange(table, sizeof(void *) * 8))
        return false;
    // First entries of JNIInvokeInterface/JNINativeInterface are real code.
    return core::IsSaneHandle(table[0]);
}

// Attaches the calling thread to the JVM if needed (CloudSync runs on its own
// worker thread, which the JVM does not know about).
class EnvScope {
  public:
    EnvScope() {
        if (!aml)
            return;
        JavaVM *vm = aml->GetJavaVM();
        if (!IsUsableInterface(vm))
            return; // loader predates GetJavaVM or returned garbage
        m_vm = vm;
        JNIEnv *env = nullptr;
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK && IsUsableInterface(env)) {
            m_env = env;
            return;
        }
        env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK && IsUsableInterface(env)) {
            m_env = env;
            m_attached = true;
        }
    }
    ~EnvScope() {
        if (m_attached && m_vm)
            m_vm->DetachCurrentThread();
    }
    EnvScope(const EnvScope &) = delete;
    EnvScope &operator=(const EnvScope &) = delete;

    JNIEnv *Get() const { return m_env; }
    bool Ok() const { return m_env != nullptr; }

  private:
    JavaVM *m_vm = nullptr;
    JNIEnv *m_env = nullptr;
    bool m_attached = false;
};

void ClearException(JNIEnv *env) {
    if (env && env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

struct JniString {
    JNIEnv *env;
    jstring value = nullptr;
    JniString(JNIEnv *e, const std::string &s) : env(e), value(e->NewStringUTF(s.c_str())) {
        if (!value)
            ClearException(env);
    }
    ~JniString() {
        if (value)
            env->DeleteLocalRef(value);
    }
};

} // namespace

bool AndroidHttpTransport::Available() const {
    if (m_availableChecked)
        return m_available;
    m_availableChecked = true;
    if (!aml) {
        m_lastError = "AML interface missing";
        return false;
    }
    JavaVM *vm = aml->GetJavaVM();
    if (!IsUsableInterface(vm)) {
        m_lastError = std::string("GetJavaVM returned ") + core::HexPointer(vm) + " (loader too old?)";
        return false;
    }
    EnvScope scope;
    m_available = scope.Ok();
    if (!m_available)
        m_lastError = "cannot attach to the JVM";
    return m_available;
}

bool AndroidHttpTransport::Fetch(const std::string &url, int timeoutSec, std::string *outBuf, FILE *outFile) {
    EnvScope scope;
    JNIEnv *env = scope.Get();
    if (!env) {
        m_lastError = "no JNIEnv";
        return false;
    }

    jclass clsURL = env->FindClass("java/net/URL");
    if (!clsURL) {
        ClearException(env);
        m_lastError = "java/net/URL missing";
        return false;
    }
    jmethodID ctorURL = env->GetMethodID(clsURL, "<init>", "(Ljava/lang/String;)V");
    jmethodID openConnection = env->GetMethodID(clsURL, "openConnection", "()Ljava/net/URLConnection;");
    if (!ctorURL || !openConnection) {
        ClearException(env);
        env->DeleteLocalRef(clsURL);
        m_lastError = "URL methods missing";
        return false;
    }

    JniString jurl(env, url);
    jobject urlObj = env->NewObject(clsURL, ctorURL, jurl.value);
    if (!urlObj) {
        ClearException(env);
        env->DeleteLocalRef(clsURL);
        m_lastError = "URL ctor failed";
        return false;
    }

    jobject conn = env->CallObjectMethod(urlObj, openConnection);
    if (!conn) {
        ClearException(env);
        env->DeleteLocalRef(urlObj);
        env->DeleteLocalRef(clsURL);
        m_lastError = "openConnection failed";
        return false;
    }

    jclass clsHttp = env->FindClass("java/net/HttpURLConnection");
    jmethodID setTimeout = env->GetMethodID(clsHttp, "setConnectTimeout", "(I)V");
    jmethodID setReadTimeout = env->GetMethodID(clsHttp, "setReadTimeout", "(I)V");
    jmethodID setRequestMethod = env->GetMethodID(clsHttp, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setUseCaches = env->GetMethodID(clsHttp, "setUseCaches", "(Z)V");
    jmethodID setInstanceFollow = env->GetMethodID(clsHttp, "setInstanceFollowRedirects", "(Z)V");
    jmethodID connect = env->GetMethodID(clsHttp, "connect", "()V");
    jmethodID getResponseCode = env->GetMethodID(clsHttp, "getResponseCode", "()I");
    jmethodID getInputStream = env->GetMethodID(clsHttp, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID disconnect = env->GetMethodID(clsHttp, "disconnect", "()V");

    bool ok = false;
    jobject stream = nullptr;
    jbyteArray buffer = nullptr;

    do {
        if (!setTimeout || !connect || !getInputStream) {
            ClearException(env);
            m_lastError = "HttpURLConnection methods missing";
            break;
        }
        const int ms = (timeoutSec > 0 ? timeoutSec : 15) * 1000;
        env->CallVoidMethod(conn, setTimeout, ms);
        env->CallVoidMethod(conn, setReadTimeout, ms);
        if (setUseCaches)
            env->CallVoidMethod(conn, setUseCaches, JNI_FALSE);
        if (setInstanceFollow)
            env->CallVoidMethod(conn, setInstanceFollow, JNI_TRUE);
        if (setRequestMethod) {
            JniString jget(env, "GET");
            env->CallVoidMethod(conn, setRequestMethod, jget.value);
        }
        if (env->ExceptionCheck()) {
            ClearException(env);
            m_lastError = "request setup failed";
            break;
        }

        env->CallVoidMethod(conn, connect);
        if (env->ExceptionCheck()) {
            ClearException(env);
            m_lastError = "connect failed: " + url;
            break;
        }

        const int code = getResponseCode ? env->CallIntMethod(conn, getResponseCode) : 200;
        if (env->ExceptionCheck()) {
            ClearException(env);
            m_lastError = "response code failed";
            break;
        }
        if (code < 200 || code >= 300) {
            m_lastError = "HTTP " + std::to_string(code);
            break;
        }

        stream = env->CallObjectMethod(conn, getInputStream);
        if (!stream || env->ExceptionCheck()) {
            ClearException(env);
            m_lastError = "input stream failed";
            break;
        }

        jclass clsStream = env->FindClass("java/io/InputStream");
        jmethodID read = env->GetMethodID(clsStream, "read", "([B)I");
        jmethodID close = env->GetMethodID(clsStream, "close", "()V");
        buffer = env->NewByteArray(32768);
        if (!read || !buffer) {
            ClearException(env);
            m_lastError = "stream methods missing";
            break;
        }

        std::vector<char> chunk(32768);
        while (true) {
            const jint n = env->CallIntMethod(stream, read, buffer);
            if (env->ExceptionCheck()) {
                ClearException(env);
                m_lastError = "read failed";
                break;
            }
            if (n <= 0)
                break;
            env->GetByteArrayRegion(buffer, 0, n, reinterpret_cast<jbyte *>(chunk.data()));
            if (outBuf)
                outBuf->append(chunk.data(), static_cast<size_t>(n));
            if (outFile) {
                if (std::fwrite(chunk.data(), 1, static_cast<size_t>(n), outFile) != static_cast<size_t>(n)) {
                    m_lastError = "file write failed";
                    break;
                }
            }
        }
        if (!m_lastError.empty())
            break;
        if (close)
            env->CallVoidMethod(stream, close);
        ok = true;
    } while (false);

    if (buffer)
        env->DeleteLocalRef(buffer);
    if (stream)
        env->DeleteLocalRef(stream);
    if (disconnect)
        env->CallVoidMethod(conn, disconnect);
    if (clsHttp)
        env->DeleteLocalRef(clsHttp);
    env->DeleteLocalRef(conn);
    env->DeleteLocalRef(urlObj);
    env->DeleteLocalRef(clsURL);
    return ok;
}

bool AndroidHttpTransport::Get(const std::string &url, std::string &out, int timeoutSec) {
    out.clear();
    m_lastError.clear();
    return Fetch(url, timeoutSec, &out, nullptr);
}

bool AndroidHttpTransport::Download(const std::string &url, const std::string &localPath, int timeoutSec) {
    m_lastError.clear();
    std::error_code ec;
    const std::filesystem::path p(localPath);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);

    FILE *f = std::fopen(localPath.c_str(), "wb");
    if (!f) {
        m_lastError = "cannot create " + localPath;
        return false;
    }
    const bool ok = Fetch(url, timeoutSec, nullptr, f);
    std::fclose(f);
    if (!ok) {
        std::filesystem::remove(localPath, ec);
        USA_LOG_W("AndroidHttp: download failed %s: %s", url.c_str(), m_lastError.c_str());
    }
    return ok;
}

#else // !__ANDROID__

// Host build: the test suite injects its own fake transport, so this one just
// refuses politely instead of pretending to have a network.
bool AndroidHttpTransport::Fetch(const std::string &url, int, std::string *, FILE *) {
    m_lastError = "JNI transport is Android-only (" + url + ")";
    return false;
}

bool AndroidHttpTransport::Available() const {
    m_lastError = "JNI transport is Android-only";
    return false;
}

bool AndroidHttpTransport::Get(const std::string &url, std::string &out, int timeoutSec) {
    out.clear();
    return Fetch(url, timeoutSec, &out, nullptr);
}

bool AndroidHttpTransport::Download(const std::string &url, const std::string &path, int timeoutSec) {
    (void)path;
    return Fetch(url, timeoutSec, nullptr, nullptr);
}

#endif

} // namespace net
} // namespace usa1to1
