#pragma once
// AMF0 (RTMP のコマンドで使う形式) の最小限の読み書き。
// 受信データは信用しない: 長さ・入れ子の深さを確かめ、壊れていたらそこで読むのをやめる。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace denpachan::rtmpd {

struct AmfValue {
    enum class Type { Number, Boolean, String, Object, Null, Undefined, EcmaArray, StrictArray, Other };
    Type type = Type::Null;
    double number = 0;
    bool boolean = false;
    std::string str;
    std::vector<std::pair<std::string, AmfValue>> props;  // Object / EcmaArray
    std::vector<AmfValue> items;                          // StrictArray

    const AmfValue* get(const std::string& key) const {
        for (const auto& [k, v] : props)
            if (k == key) return &v;
        return nullptr;
    }
    std::string get_string(const std::string& key) const {
        const AmfValue* v = get(key);
        return v && v->type == Type::String ? v->str : std::string();
    }
};

// data を先頭から読めるだけ読む (壊れていればそこまで)
std::vector<AmfValue> amf0_decode(const uint8_t* data, size_t size);

class AmfWriter {
public:
    AmfWriter& number(double v);
    AmfWriter& boolean(bool v);
    AmfWriter& string(const std::string& v);
    AmfWriter& null();
    AmfWriter& undefined();
    AmfWriter& begin_object();
    AmfWriter& key(const std::string& k);  // オブジェクトのプロパティ名 (続けて値を書く)
    AmfWriter& end_object();

    const std::vector<uint8_t>& data() const { return buf_; }

private:
    void put_u16(uint16_t v);
    std::vector<uint8_t> buf_;
};

}  // namespace denpachan::rtmpd
