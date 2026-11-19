// Short-lived status messages for the in-game overlay.
//
// The mod used to call aml->ShowToast() for these. That call is a JNI facade in
// AML: on Android 15 (ART) invoking it from the game thread aborts the process
// inside art::JavaVMExt::JniAbort, because the cached JNIEnv belongs to the
// Java/UI thread and Toast also wants a Looper. Status therefore lives here and
// is rendered by the ImGui overlay instead - no JNI, no second thread, no crash.
#pragma once

#include <cstddef>
#include <deque>
#include <string>

namespace usa1to1 {
namespace core {

class StatusFeed {
  public:
    static constexpr size_t kCapacity = 8;

    void Push(const std::string &text, double nowSeconds);

    // Newest message plus its age; false when the feed is empty.
    bool Latest(std::string &out, double nowSeconds) const;
    // True only while the newest message is younger than maxAgeSec.
    bool Fresh(std::string &out, double nowSeconds, double maxAgeSec = 3.0) const;

    const std::string &LastText() const { return m_lastText; }
    double LastAge(double nowSeconds) const;
    size_t Size() const { return m_entries.size(); }
    void Clear();

  private:
    struct Entry {
        std::string text;
        double time = 0.0;
    };
    std::deque<Entry> m_entries;
    std::string m_empty;
    std::string m_lastText;
    double m_lastTime = 0.0;
};

} // namespace core
} // namespace usa1to1
