#include "amf0.h"

#include <cstring>

namespace denpachan::rtmpd {

namespace {

constexpr int kMaxDepth = 16;
constexpr uint32_t kMaxItems = 4096;

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    bool done() const { return p_ >= end_; }

    bool value(AmfValue& out, int depth) {
        if (depth > kMaxDepth || !have(1)) return false;
        const uint8_t marker = *p_++;
        switch (marker) {
        case 0x00: {  // number
            if (!have(8)) return false;
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) bits = (bits << 8) | p_[i];
            p_ += 8;
            out.type = AmfValue::Type::Number;
            memcpy(&out.number, &bits, 8);
            return true;
        }
        case 0x01:  // boolean
            if (!have(1)) return false;
            out.type = AmfValue::Type::Boolean;
            out.boolean = *p_++ != 0;
            return true;
        case 0x02:  // string
            out.type = AmfValue::Type::String;
            return short_string(out.str);
        case 0x0C: {  // long string
            if (!have(4)) return false;
            const uint32_t len = u32();
            if (!have(len)) return false;
            out.type = AmfValue::Type::String;
            out.str.assign(reinterpret_cast<const char*>(p_), len);
            p_ += len;
            return true;
        }
        case 0x03:  // object
            out.type = AmfValue::Type::Object;
            return properties(out, depth);
        case 0x08:  // ECMA array (件数は信用せず、終端まで読む)
            if (!have(4)) return false;
            p_ += 4;
            out.type = AmfValue::Type::EcmaArray;
            return properties(out, depth);
        case 0x0A: {  // strict array
            if (!have(4)) return false;
            const uint32_t count = u32();
            if (count > kMaxItems) return false;
            out.type = AmfValue::Type::StrictArray;
            for (uint32_t i = 0; i < count; ++i) {
                AmfValue v;
                if (!value(v, depth + 1)) return false;
                out.items.push_back(std::move(v));
            }
            return true;
        }
        case 0x05:
            out.type = AmfValue::Type::Null;
            return true;
        case 0x06:
            out.type = AmfValue::Type::Undefined;
            return true;
        case 0x0B:  // date
            if (!have(10)) return false;
            p_ += 10;
            out.type = AmfValue::Type::Other;
            return true;
        default:
            return false;  // 対応しない型 (AMF3 への切り替えなど)
        }
    }

private:
    bool have(size_t n) const { return size_t(end_ - p_) >= n; }
    uint32_t u32() {
        const uint32_t v = (uint32_t(p_[0]) << 24) | (uint32_t(p_[1]) << 16) | (uint32_t(p_[2]) << 8) | p_[3];
        p_ += 4;
        return v;
    }
    bool short_string(std::string& out) {
        if (!have(2)) return false;
        const uint16_t len = uint16_t((p_[0] << 8) | p_[1]);
        p_ += 2;
        if (!have(len)) return false;
        out.assign(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return true;
    }
    bool properties(AmfValue& out, int depth) {
        for (uint32_t i = 0; i < kMaxItems; ++i) {
            std::string key;
            if (!short_string(key)) return false;
            if (key.empty()) {
                if (!have(1)) return false;
                if (*p_ == 0x09) {  // object end
                    ++p_;
                    return true;
                }
            }
            AmfValue v;
            if (!value(v, depth + 1)) return false;
            out.props.emplace_back(std::move(key), std::move(v));
        }
        return false;
    }

    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace

std::vector<AmfValue> amf0_decode(const uint8_t* data, size_t size) {
    std::vector<AmfValue> values;
    Reader r(data, size);
    while (!r.done() && values.size() < 64) {
        AmfValue v;
        if (!r.value(v, 0)) break;
        values.push_back(std::move(v));
    }
    return values;
}

void AmfWriter::put_u16(uint16_t v) {
    buf_.push_back(uint8_t(v >> 8));
    buf_.push_back(uint8_t(v));
}

AmfWriter& AmfWriter::number(double v) {
    buf_.push_back(0x00);
    uint64_t bits = 0;
    memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; --i) buf_.push_back(uint8_t(bits >> (i * 8)));
    return *this;
}

AmfWriter& AmfWriter::boolean(bool v) {
    buf_.push_back(0x01);
    buf_.push_back(v ? 1 : 0);
    return *this;
}

AmfWriter& AmfWriter::string(const std::string& v) {
    buf_.push_back(0x02);
    put_u16(uint16_t(v.size()));
    buf_.insert(buf_.end(), v.begin(), v.end());
    return *this;
}

AmfWriter& AmfWriter::null() {
    buf_.push_back(0x05);
    return *this;
}

AmfWriter& AmfWriter::undefined() {
    buf_.push_back(0x06);
    return *this;
}

AmfWriter& AmfWriter::begin_object() {
    buf_.push_back(0x03);
    return *this;
}

AmfWriter& AmfWriter::key(const std::string& k) {
    put_u16(uint16_t(k.size()));
    buf_.insert(buf_.end(), k.begin(), k.end());
    return *this;
}

AmfWriter& AmfWriter::end_object() {
    put_u16(0);
    buf_.push_back(0x09);
    return *this;
}

}  // namespace denpachan::rtmpd
