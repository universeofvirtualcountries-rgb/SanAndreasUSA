// JNI transport over java.net.HttpURLConnection.
//
// No OpenSSL, no libcurl: the .so stays tiny and the Android system trust store
// (plus user-installed CAs) keeps working for free.
#pragma once

#include "cloud_sync.hpp"

#include <string>

namespace usa1to1 {
namespace net {

class AndroidHttpTransport final : public IHttpTransport {
  public:
    bool Get(const std::string &url, std::string &out, int timeoutSec) override;
    bool Download(const std::string &url, const std::string &localPath, int timeoutSec) override;

    const std::string &LastError() const { return m_lastError; }
    // True only when a usable JNIEnv/JavaVM was obtained from the loader. The
    // check validates the interface pointers: on loaders that predate the
    // GetJavaVM slot, aml->GetJavaVM() returns garbage, and calling through it
    // is an instant crash.
    bool Available() const;

  private:
    bool Fetch(const std::string &url, int timeoutSec, std::string *outBuf, FILE *outFile);
    mutable std::string m_lastError;
    mutable bool m_availableChecked = false;
    mutable bool m_available = false;
};

} // namespace net
} // namespace usa1to1
