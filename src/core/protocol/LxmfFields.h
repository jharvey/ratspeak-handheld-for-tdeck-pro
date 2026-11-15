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

// A fields map holding only the audio field, as two spans: head (msgpack map/array/mode/bin
// header) then the borrowed audio bytes. total() is the fields length.
struct AudioField {
    uint8_t head[AudioHeaderMax] = {};
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
    if (rs_handheld_lxmf_audio_field_header(mode, audioLength, out.head, sizeof(out.head), &n) != RS_HANDHELD_OK)
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

} // namespace handheld::lxmf
