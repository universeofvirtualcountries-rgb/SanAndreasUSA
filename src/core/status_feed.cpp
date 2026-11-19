#include "status_feed.hpp"

namespace usa1to1 {
namespace core {

void StatusFeed::Push(const std::string &text, double nowSeconds) {
    if (text.empty())
        return;
    m_entries.push_back(Entry{text, nowSeconds});
    while (m_entries.size() > kCapacity)
        m_entries.pop_front();
    m_lastText = text;
    m_lastTime = nowSeconds;
}

bool StatusFeed::Latest(std::string &out, double) const {
    if (m_entries.empty())
        return false;
    out = m_entries.back().text;
    return true;
}

bool StatusFeed::Fresh(std::string &out, double nowSeconds, double maxAgeSec) const {
    if (m_entries.empty())
        return false;
    const Entry &e = m_entries.back();
    const double age = nowSeconds - e.time;
    if (age < 0.0 || age > maxAgeSec)
        return false;
    out = e.text;
    return true;
}

double StatusFeed::LastAge(double nowSeconds) const {
    if (m_entries.empty())
        return 1e9;
    const double age = nowSeconds - m_entries.back().time;
    return age < 0.0 ? 0.0 : age;
}

void StatusFeed::Clear() {
    m_entries.clear();
    m_lastText.clear();
    m_lastTime = 0.0;
}

} // namespace core
} // namespace usa1to1
