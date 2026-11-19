// Dependency-free JSON value + parser + writer.
// Used by: objects_db.json, missions, world_delta.json, assets/manifest.json.
// Deliberately small but strict: full string escapes (incl. \uXXXX -> UTF-8),
// int64 and double numbers, ordered objects (stable output for delta diffs).
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace usa1to1 {
namespace core {

class JsonError : public std::runtime_error {
  public:
    explicit JsonError(const std::string &msg) : std::runtime_error(msg) {}
};

class Json {
  public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };
    using ValueList = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Members = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : m_type(Type::Bool), m_bool(v) {}
    Json(int v) : m_type(Type::Int), m_int(v) {}
    Json(long v) : m_type(Type::Int), m_int(v) {}
    Json(long long v) : m_type(Type::Int), m_int(v) {}
    Json(unsigned v) : m_type(Type::Int), m_int(v) {}
    Json(unsigned long long v) : m_type(Type::Int), m_int(static_cast<int64_t>(v)) {}
    Json(double v) : m_type(Type::Double), m_double(v) {}
    Json(const char *v) : m_type(Type::String), m_str(v ? v : "") {}
    Json(std::string v) : m_type(Type::String), m_str(std::move(v)) {}

    static Json Object() {
        Json j;
        j.m_type = Type::Object;
        return j;
    }
    static Json MakeArray() {
        Json j;
        j.m_type = Type::Array;
        return j;
    }

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsObject() const { return m_type == Type::Object; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsString() const { return m_type == Type::String; }
    bool IsNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool IsBool() const { return m_type == Type::Bool; }

    // Typed getters. getOr<>() variants never throw and validate type first.
    bool AsBool() const {
        Require(Type::Bool);
        return m_bool;
    }
    int64_t AsInt() const {
        if (m_type == Type::Int)
            return m_int;
        if (m_type == Type::Double)
            return static_cast<int64_t>(m_double);
        Require(Type::Int);
        return 0;
    }
    double AsDouble() const {
        if (m_type == Type::Double)
            return m_double;
        if (m_type == Type::Int)
            return static_cast<double>(m_int);
        Require(Type::Double);
        return 0.0;
    }
    const std::string &AsString() const {
        Require(Type::String);
        return m_str;
    }

    bool GetBool(const std::string &key, bool def) const {
        const Json *v = Find(key);
        return (v && v->IsBool()) ? v->AsBool() : def;
    }
    int64_t GetInt(const std::string &key, int64_t def) const {
        const Json *v = Find(key);
        return (v && v->IsNumber()) ? v->AsInt() : def;
    }
    double GetDouble(const std::string &key, double def) const {
        const Json *v = Find(key);
        return (v && v->IsNumber()) ? v->AsDouble() : def;
    }
    std::string GetString(const std::string &key, const std::string &def) const {
        const Json *v = Find(key);
        return (v && v->IsString()) ? v->AsString() : def;
    }

    size_t Size() const { return m_type == Type::Array ? m_arr.size() : m_mem.size(); }

    // Object access -----------------------------------------------------------
    const Json *Find(const std::string &key) const {
        if (m_type != Type::Object)
            return nullptr;
        for (const auto &kv : m_mem)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
    Json *Find(const std::string &key) {
        if (m_type != Type::Object)
            return nullptr;
        for (auto &kv : m_mem)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
    const Json &At(const std::string &key) const {
        const Json *v = Find(key);
        if (!v)
            throw JsonError("missing key: " + key);
        return *v;
    }
    bool Has(const std::string &key) const { return Find(key) != nullptr; }

    Json &Set(const std::string &key, Json value) {
        if (m_type == Type::Null)
            m_type = Type::Object;
        Require(Type::Object);
        for (auto &kv : m_mem) {
            if (kv.first == key) {
                kv.second = std::move(value);
                return *this;
            }
        }
        m_mem.emplace_back(key, std::move(value));
        return *this;
    }
    // Missing key returns a shared null value (read-only usage).
    const Json &operator[](const std::string &key) const {
        static const Json kNull;
        const Json *v = Find(key);
        return v ? *v : kNull;
    }
    const Members &GetMembers() const { return m_mem; }

    // Array access ------------------------------------------------------------
    Json &Push(Json value) {
        if (m_type == Type::Null)
            m_type = Type::Array;
        Require(Type::Array);
        m_arr.push_back(std::move(value));
        return *this;
    }
    const Json &operator[](size_t i) const { return m_arr[i]; }
    Json &operator[](size_t i) { return m_arr[i]; }
    const ValueList &GetArray() const { return m_arr; }
    ValueList &GetArray() { return m_arr; }

    // Serialisation -----------------------------------------------------------
    std::string Dump(bool pretty = false) const;

    static Json Parse(const std::string &text);
    static bool TryParse(const std::string &text, Json &out, std::string *error = nullptr);

  private:
    void Require(Type expected) const {
        if (m_type != expected)
            throw JsonError("json type mismatch");
    }

    Type m_type = Type::Null;
    bool m_bool = false;
    int64_t m_int = 0;
    double m_double = 0.0;
    std::string m_str;
    ValueList m_arr;
    Members m_mem;
};

} // namespace core
} // namespace usa1to1
