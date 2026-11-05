#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct SSEEvent {
    std::string name; // "event:" field, or empty if none
    std::string data; // joined "data:" field (multi-line joined with \n)
};

// Stateful SSE (text/event-stream) parser. Feed raw bytes as they arrive from
// the socket; each time a blank line terminates an event, onEvent fires.
// Handles multi-line data fields, comment lines, \r\n and \n line endings.
class SSEParser {
public:
    explicit SSEParser(const std::function<void(SSEEvent)>& onEvent)
        : onEvent_(onEvent) {}

    void feed(const char* bytes, size_t len) {
        buf_.append(bytes, len);
        size_t start = 0;
        for (size_t i = 0; i < buf_.size(); i++) {
            if (buf_[i] != '\n')
                continue;
            std::string line = buf_.substr(start, i - start);
            start = i + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            handleLine(line);
        }
        buf_ = buf_.substr(start);
    }

    void feed(std::string_view bytes) {
        feed(bytes.data(), bytes.size());
    }

private:
    void handleLine(const std::string& line) {
        if (line.empty()) {
            dispatch();
            return;
        }
        if (line[0] == ':') // comment
            return;

        size_t colon = line.find(':');
        std::string field = (colon == std::string::npos) ? line : line.substr(0, colon);
        std::string value;
        if (colon != std::string::npos) {
            value = line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ')
                value.erase(0, 1);
        }

        if (field == "data") {
            if (data_.has_value())
                data_->push_back('\n');
            else
                data_ = std::string();
            *data_ += value;
        } else if (field == "event") {
            eventName_ = value;
        }
        // "id:" and "retry:" are not needed for chat completions; ignore.
    }

    void dispatch() {
        if (!data_.has_value() && eventName_.empty())
            return;
        onEvent_(SSEEvent{eventName_, data_.value_or("")});
        eventName_.clear();
        data_.reset();
    }

    std::function<void(SSEEvent)> onEvent_;
    std::string buf_;       // incomplete trailing line
    std::string eventName_;
    std::optional<std::string> data_;
};
