#pragma once

// LXMF message fields: voice messages (FIELD_AUDIO), Sideband compatible.
//
// An LXMF message carries a msgpack map of fields next to title/content. A voice message has
// fields = {0x07: [audio_mode, audio_bytes]} (LXMF/LXMF.py FIELD_AUDIO and AM_* modes). For
// Codec2, audio_bytes are the raw concatenated frames of an 8 kHz Codec2 encoder with NO header
// (that is what Sideband sends and plays); the mode tells the decoder the frame size.
//
// Send:     ProtocolBackend::lxmfSubmitAudio(dest, mode, audio, audioLength, text, textLength)
//           (same ticket / poll / acknowledge / status flow as lxmfSubmit).
// Receive:  the committed-message callback gives a RecordKey; read the record with
//           MessageStore::requestRecord(key, 0, 4096, true /*withFields*/). The payload is
//           StoredRecordHeader, then title||content||fields; header.fieldsLength == 0 means no
//           fields. findAudio() on the fields bytes gives the mode and the audio bytes.
// Stored records keep only the audio field (other incoming fields are dropped), in the
// record's "fields" JSON string.
//
// Size: title + content + fields <= 3659 bytes (storage body and LXMF resource limit), so with
// no text the audio may be up to MaxAudioBytes (3542 B): Codec2 700C ~40 s, 1600 ~17 s, 3200
// ~8.8 s. Messages that do not fit one LoRa frame go over a link as a Resource (slow on LoRa:
// budget airtime; 700C = 87.5 B/s of speech).
//
// The Rust functions are declared weak below so this firmware also links against an older
// protocol archive without them: available() is then false, lxmfSubmitAudio returns
// Rejection::Invalid and incoming audio is dropped (text kept).

#include <stddef.h>
#include <stdint.h>
#include "ratspeak_protocol.h"

extern "C" {
rs_handheld_status_t rs_handheld_lxmf_packed_size_fields(size_t, size_t, size_t, size_t*) __attribute__((weak));
rs_handheld_status_t rs_handheld_rns_lxmf_build_fields(const rs_handheld_rns_t*, const uint8_t[64], double,
    uint64_t, uint64_t, const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t,
    const uint8_t[32], const uint8_t[16], uint8_t*, size_t, size_t*, uint8_t[16], uint8_t[32]) __attribute__((weak));
rs_handheld_status_t rs_handheld_rns_lxmf_build_link_fields(const rs_handheld_rns_t*, const uint8_t[64], double,
    const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t,
    uint8_t*, size_t, size_t*, uint8_t[16], uint8_t[32]) __attribute__((weak));
rs_handheld_status_t rs_handheld_lxmf_audio_field_header(uint8_t, size_t, uint8_t*, size_t, size_t*) __attribute__((weak));
rs_handheld_status_t rs_handheld_lxmf_fields_audio(const uint8_t*, size_t, uint8_t*, size_t*, size_t*) __attribute__((weak));
}

namespace handheld::lxmf {

constexpr uint8_t FieldAudio = RS_HANDHELD_LXMF_FIELD_AUDIO;               // 0x07
constexpr uint8_t AudioCodec2_700C = RS_HANDHELD_LXMF_AM_CODEC2_700C;      // 0x03
constexpr uint8_t AudioCodec2_1600 = RS_HANDHELD_LXMF_AM_CODEC2_1600;      // 0x07
constexpr uint8_t AudioCodec2_3200 = RS_HANDHELD_LXMF_AM_CODEC2_3200;      // 0x09
constexpr size_t AudioHeaderMax = RS_HANDHELD_LXMF_AUDIO_HEADER_MAX;       // 10
// Largest audio with no title/content: 3659 packed - 96 prefix - 14 payload - 7 field header.
constexpr size_t MaxAudioBytes = 3542;

// True when the linked protocol archive has the fields API.
inline bool available() {
    return rs_handheld_lxmf_packed_size_fields && rs_handheld_rns_lxmf_build_fields &&
           rs_handheld_rns_lxmf_build_link_fields && rs_handheld_lxmf_audio_field_header &&
           rs_handheld_lxmf_fields_audio;
}

// The audio field of a raw fields map (e.g. a record's fields bytes). audio points into fields.
inline bool findAudio(const uint8_t* fields, size_t length, uint8_t& mode, const uint8_t*& audio,
                      size_t& audioLength) {
    if (!rs_handheld_lxmf_fields_audio || !fields || !length) return false;
    size_t offset = 0, count = 0; uint8_t m = 0;
    if (rs_handheld_lxmf_fields_audio(fields, length, &m, &offset, &count) != RS_HANDHELD_OK) return false;
    mode = m; audio = fields + offset; audioLength = count;
    return true;
}

// ---- Group messages (FIELD_GROUP) -------------------------------------------------------
// A message sent to a RatSpeak group is an ordinary direct message to each member, with
//   fields = {0x0B: "<8 hex group id> <8 hex send number> <group name>"}   (one msgpack str)
// A radio that has a group with that id shows the message in the group's thread; any other
// LXMF client ignores the field and shows the text as a normal direct message. The send number
// is the same on every member's copy of one message (the sender counts its copies by it).
// The value is plain text on purpose: stored records keep the fields bytes in a JSON string.
constexpr uint8_t FieldGroup = 0x0B;                 // LXMF.py FIELD_GROUP
constexpr size_t GroupNameMax = 16;
constexpr size_t GroupFieldMax = 2 + 2 + 8 + 1 + 8 + 1 + GroupNameMax;   // 38
constexpr size_t FieldHeadMax = 40;                  // >= AudioHeaderMax and GroupFieldMax

struct GroupTag {
    uint8_t id[4] = {};
    uint32_t seq = 0;
    char name[GroupNameMax + 1] = "";
};

// The fields map {FieldGroup: "<id> <seq> <name>"}; returns its length (0 = does not fit).
inline size_t makeGroupField(const GroupTag& tag, uint8_t* out, size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    size_t nameLength = 0;
    while (nameLength < GroupNameMax && tag.name[nameLength]) nameLength++;
    const size_t textLength = 8 + 1 + 8 + 1 + nameLength;
    if (!out || capacity < 4 + textLength) return 0;
    size_t n = 0;
    out[n++] = 0x81; out[n++] = FieldGroup; out[n++] = 0xD9; out[n++] = uint8_t(textLength);
    for (int i = 0; i < 4; i++) { out[n++] = uint8_t(hex[tag.id[i] >> 4]); out[n++] = uint8_t(hex[tag.id[i] & 15]); }
    out[n++] = ' ';
    for (int i = 7; i >= 0; i--) out[n++] = uint8_t(hex[(tag.seq >> (i * 4)) & 15]);
    out[n++] = ' ';
    for (size_t i = 0; i < nameLength; i++) out[n++] = uint8_t(tag.name[i]);
    return n;
}

// One msgpack object at p: the address after it, or nullptr (damaged / too deep).
inline const uint8_t* msgpackSkip(const uint8_t* p, const uint8_t* end, int depth = 0) {
    if (!p || p >= end || depth > 6) return nullptr;
    const uint8_t t = *p++;
    auto be = [&](int count, uint32_t& v) {
        if (end - p < count) return false;
        v = 0;
        for (int i = 0; i < count; i++) v = (v << 8) | *p++;
        return true;
    };
    auto bytes = [&](uint32_t n) -> const uint8_t* { return size_t(end - p) < n ? nullptr : p + n; };
    auto items = [&](uint32_t n) -> const uint8_t* {
        for (uint32_t i = 0; i < n; i++) { p = msgpackSkip(p, end, depth + 1); if (!p) return nullptr; }
        return p;
    };
    uint32_t n = 0;
    if (t <= 0x7F || t >= 0xE0 || t == 0xC0 || t == 0xC2 || t == 0xC3) return p;
    if (t <= 0x8F) return items(uint32_t(t & 15) * 2);
    if (t <= 0x9F) return items(t & 15);
    if (t <= 0xBF) return bytes(t & 31);
    switch (t) {
        case 0xC4: case 0xD9: return be(1, n) ? bytes(n) : nullptr;
        case 0xC5: case 0xDA: return be(2, n) ? bytes(n) : nullptr;
        case 0xC6: case 0xDB: return be(4, n) ? bytes(n) : nullptr;
        case 0xC7: return be(1, n) ? bytes(n + 1) : nullptr;
        case 0xC8: return be(2, n) ? bytes(n + 1) : nullptr;
        case 0xC9: return be(4, n) && n < 0xFFFFFFFFu ? bytes(n + 1) : nullptr;
        case 0xCA: case 0xCE: case 0xD2: return bytes(4);
        case 0xCB: case 0xCF: case 0xD3: return bytes(8);
        case 0xCC: case 0xD0: return bytes(1);
        case 0xCD: case 0xD1: return bytes(2);
        case 0xD4: return bytes(2);
        case 0xD5: return bytes(3);
        case 0xD6: return bytes(5);
        case 0xD7: return bytes(9);
        case 0xD8: return bytes(17);
        case 0xDC: return be(2, n) ? items(n) : nullptr;
        case 0xDD: return be(4, n) && n <= 4096 ? items(n) : nullptr;
        case 0xDE: return be(2, n) ? items(n * 2) : nullptr;
        case 0xDF: return be(4, n) && n <= 2048 ? items(n * 2) : nullptr;
        default: return nullptr;                     // 0xC1: never used
    }
}

// The group tag of a raw fields map (any other fields are stepped over).
inline bool findGroup(const uint8_t* fields, size_t length, GroupTag& out) {
    if (!fields || length < 4) return false;
    const uint8_t* p = fields;
    const uint8_t* const end = fields + length;
    uint32_t pairs = 0;
    if ((*p & 0xF0) == 0x80) pairs = *p++ & 15;
    else if (*p == 0xDE && length >= 3) { pairs = (uint32_t(p[1]) << 8) | p[2]; p += 3; }
    else return false;
    for (uint32_t i = 0; i < pairs && p && p < end; i++) {
        const bool mine = *p == FieldGroup;
        p = msgpackSkip(p, end);                     // the key
        if (!p || p >= end) return false;
        if (!mine) { p = msgpackSkip(p, end); continue; }
        size_t n = 0;
        if ((*p & 0xE0) == 0xA0) n = *p++ & 31;
        else if (*p == 0xD9 && end - p >= 2) { n = p[1]; p += 2; }
        else return false;
        if (size_t(end - p) < n || n < 18 || n > 18 + GroupNameMax || p[8] != ' ' || p[17] != ' ') return false;
        auto nib = [](uint8_t c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        out = GroupTag();
        for (int k = 0; k < 4; k++) {
            const int h = nib(p[2 * k]), l = nib(p[2 * k + 1]);
            if (h < 0 || l < 0) return false;
            out.id[k] = uint8_t((h << 4) | l);
        }
        for (int k = 0; k < 8; k++) {
            const int v = nib(p[9 + k]);
            if (v < 0) return false;
            out.seq = (out.seq << 4) | uint32_t(v);
        }
        for (size_t k = 18; k < n; k++) out.name[k - 18] = (p[k] >= 32 && p[k] < 127) ? char(p[k]) : '?';
        return true;
    }
    return false;
}

// A fields map holding only the audio field, as two spans: head (msgpack map/array/mode/bin
// header) then the borrowed audio bytes. total() is the fields length.
struct AudioField {
    uint8_t head[FieldHeadMax] = {};
    size_t headLength = 0;
    const uint8_t* audio = nullptr;
    size_t audioLength = 0;
    size_t total() const { return headLength + audioLength; }
    bool valid() const { return headLength != 0; }
};

inline bool makeAudioField(uint8_t mode, const uint8_t* audio, size_t audioLength, AudioField& out) {
    out = {};
    if (!rs_handheld_lxmf_audio_field_header || (audioLength && !audio)) return false;
    size_t n = 0;
    if (rs_handheld_lxmf_audio_field_header(mode, audioLength, out.head, AudioHeaderMax, &n) != RS_HANDHELD_OK)
        return false;
    out.headLength = n; out.audio = audio; out.audioLength = audioLength;
    return true;
}

// Incoming: keep only the audio field of a received fields map (re-packed canonically).
inline bool keepAudio(const uint8_t* fields, size_t length, AudioField& out) {
    uint8_t mode = 0; const uint8_t* audio = nullptr; size_t audioLength = 0;
    out = {};
    return findAudio(fields, length, mode, audio, audioLength) && makeAudioField(mode, audio, audioLength, out);
}

// Incoming: what this firmware stores of a received fields map: the audio field (a voice
// message), else the group tag (re-packed canonically). Other fields are dropped.
inline bool keepKnown(const uint8_t* fields, size_t length, AudioField& out) {
    if (keepAudio(fields, length, out)) return true;
    GroupTag tag;
    out = {};
    if (!findGroup(fields, length, tag)) return false;
    out.headLength = makeGroupField(tag, out.head, sizeof(out.head));
    return out.headLength != 0;
}

} // namespace handheld::lxmf
