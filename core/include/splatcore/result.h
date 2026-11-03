#pragma once

#include <string>
#include <utility>

namespace splatcore {

// Minimal expected-style result: a value or an error message. splatcore reports failures through
// these instead of exceptions so results can cross the JNI boundary unchanged (std::expected is
// C++23; this is the project's stand-in).
template <typename T>
class Result {
public:
    static Result success(T value) {
        Result r;
        r.m_value = std::move(value);
        r.m_ok = true;
        return r;
    }
    static Result failure(std::string message) {
        Result r;
        r.m_error = std::move(message);
        return r;
    }

    explicit operator bool() const { return m_ok; }
    bool ok() const { return m_ok; }
    T& value() { return m_value; }
    const T& value() const { return m_value; }
    const std::string& error() const { return m_error; }

private:
    Result() = default;
    T m_value{};
    std::string m_error;
    bool m_ok = false;
};

// Result without a payload.
class Status {
public:
    static Status success() { return Status(); }
    static Status failure(std::string message) {
        Status s;
        s.m_error = std::move(message);
        s.m_ok = false;
        return s;
    }

    explicit operator bool() const { return m_ok; }
    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }

private:
    Status() = default;
    std::string m_error;
    bool m_ok = true;
};

}  // namespace splatcore
